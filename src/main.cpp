// Assistente pessoal no ESP32
// --------------------------------------------------------------------------
// O ESP32 é o "cérebro": recebe texto do celular via Bluetooth LE, consulta o
// Gemini pelo Wi-Fi, executa ferramentas (LED, lembretes...) e devolve a
// resposta. O celular faz a parte de voz (reconhecimento e síntese de fala).
//
// Protocolo BLE (JSON por linha):
//   celular -> ESP32: {"t":"ask","text":"..."} | {"t":"status"} | {"t":"reset"}
//                     {"t":"cfg","ssid":"..","pass":"..","key":"..","model":".."}
//   ESP32 -> celular: {"t":"status",...} | {"t":"thinking"} | {"t":"tool","name":".."}
//                     {"t":"reply","text":".."} | {"t":"error","text":".."}
//                     {"t":"alert","text":".."} | {"t":"info","text":".."}
//
// Para testar sem o celular, digite no Serial Monitor:
//   uma pergunta qualquer        -> pergunta ao assistente
//   /wifi NomeDaRede;senha       -> configura o Wi-Fi
//   /status  /reset
// --------------------------------------------------------------------------

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <time.h>

#include <vector>

#include "Assistant.h"
#include "AudioHardware.h"
#include "BleLink.h"
#include "Settings.h"
#include "Tools.h"
#include "config.h"

static Settings settings;
static Assistant assistant;
static QueueHandle_t inbox;              // char* com linhas JSON recebidas
static std::vector<String> pendingAlerts;  // lembretes que venceram sem celular conectado

// ---------------------------------------------------------------------------
// Envio de mensagens ao celular
// ---------------------------------------------------------------------------

static void sendJson(const JsonDocument& doc) {
  std::string out;
  serializeJson(doc, out);
  Serial.printf("[TX] %.300s\n", out.c_str());
  Ble.send(out);
}

static void sendEvent(const char* type, const String& text = "") {
  JsonDocument d;
  d["t"] = type;
  if (text.length()) d["text"] = text;
  sendJson(d);
}

static void sendStatus() {
  JsonDocument d;
  d["t"] = "status";
  d["name"] = ASSISTANT_NAME;
  d["fw"] = FIRMWARE_VERSION;
  bool wifi = WiFi.status() == WL_CONNECTED;
  d["wifi"] = wifi;
  d["ssid"] = settings.ssid;
  if (wifi) {
    d["ip"] = WiFi.localIP().toString();
    d["rssi"] = WiFi.RSSI();
  }
  d["hasKey"] = !settings.apiKey.isEmpty();
  d["model"] = settings.model;
  d["heap"] = ESP.getFreeHeap();
  d["uptime"] = millis() / 1000;
  d["led"] = Tools::ledOn();
  d["timers"] = Tools::activeTimers();
  d["memory"] = assistant.historySize();
  struct tm t;
  if (getLocalTime(&t, 5)) {
    char buf[20];
    strftime(buf, sizeof buf, "%d/%m %H:%M", &t);
    d["time"] = buf;
  }
  sendJson(d);
}

static void flushPendingAlerts() {
  if (!Ble.isConnected()) return;
  for (const auto& a : pendingAlerts) sendEvent("alert", a);
  pendingAlerts.clear();
}

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------

static void connectWifi() {
  WiFi.disconnect();
  delay(100);
  if (settings.ssid.isEmpty()) {
    Serial.println("[WiFi] nenhuma rede configurada (use o app ou /wifi rede;senha)");
    return;
  }
  Serial.printf("[WiFi] conectando a \"%s\"...\n", settings.ssid.c_str());
  WiFi.begin(settings.ssid.c_str(), settings.pass.c_str());
}

// ---------------------------------------------------------------------------
// Tratamento das mensagens recebidas
// ---------------------------------------------------------------------------

static void handleAsk(const String& text) {
  Serial.printf("[ASK] %s\n", text.c_str());
  AudioHardware::playChirp();  // feedback sonoro instantâneo no alto-falante
  sendEvent("thinking");
  uint32_t t0 = millis();

  Assistant::Result r = assistant.ask(text, [](const char* tool) {
    JsonDocument d;
    d["t"] = "tool";
    d["name"] = tool;
    sendJson(d);
  });

  JsonDocument d;
  d["t"] = r.ok ? "reply" : "error";
  d["text"] = r.text;
  if (r.model.length()) d["model"] = r.model;
  d["ms"] = millis() - t0;
  sendJson(d);
}

static void handleConfig(JsonObjectConst in) {
  bool wifiChanged = false;
  if (in["ssid"].is<const char*>()) {
    settings.ssid = in["ssid"].as<const char*>();
    settings.pass = in["pass"] | "";
    wifiChanged = true;
  }
  if (in["key"].is<const char*>() && strlen(in["key"]) > 0) settings.apiKey = in["key"].as<const char*>();
  if (in["model"].is<const char*>() && strlen(in["model"]) > 0) settings.model = in["model"].as<const char*>();
  settings.save();
  sendEvent("info", "Configurações salvas no ESP32.");
  if (wifiChanged) connectWifi();
  sendStatus();
}

static void handleLine(const char* line) {
  JsonDocument in;
  if (deserializeJson(in, line)) {
    sendEvent("error", "Mensagem inválida recebida.");
    return;
  }
  const char* t = in["t"] | "";

  if (!strcmp(t, "ask")) {
    String text = in["text"] | "";
    text.trim();
    if (text.length()) handleAsk(text);
  } else if (!strcmp(t, "status")) {
    sendStatus();
    flushPendingAlerts();
  } else if (!strcmp(t, "cfg")) {
    handleConfig(in.as<JsonObjectConst>());
  } else if (!strcmp(t, "reset")) {
    assistant.clearHistory();
    sendEvent("info", "Memória da conversa apagada.");
  } else if (!strcmp(t, "chirp")) {
    AudioHardware::playChirp();
  } else if (!strcmp(t, "ping")) {
    sendEvent("pong");
  }
}

static void enqueue(const std::string& line) {
  char* copy = strdup(line.c_str());
  if (copy && xQueueSend(inbox, &copy, 0) != pdTRUE) free(copy);
}

// Tarefa principal do assistente (pilha grande por causa do TLS + JSON)
static void assistantTask(void*) {
  bool lastWifi = false;
  for (;;) {
    char* line = nullptr;
    if (xQueueReceive(inbox, &line, pdMS_TO_TICKS(250)) == pdTRUE) {
      Serial.printf("[RX] %.300s\n", line);
      handleLine(line);
      free(line);
    }

    Tools::loop([](const String& label) {
      String msg = "Lembrete: " + label;
      Serial.printf("[TIMER] %s\n", msg.c_str());
      AudioHardware::playAlarm();
      if (Ble.isConnected()) {
        sendEvent("alert", msg);
      } else if (pendingAlerts.size() < 8) {
        pendingAlerts.push_back(msg);
      }
      Tools::blink(6, 250);
    });

    bool wifi = WiFi.status() == WL_CONNECTED;
    if (wifi != lastWifi) {
      lastWifi = wifi;
      if (wifi) {
        Serial.printf("[WiFi] conectado, IP %s, RSSI %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      } else {
        Serial.println("[WiFi] desconectado");
      }
      sendStatus();
    }
  }
}

// ---------------------------------------------------------------------------
// Entrada pelo Serial Monitor (para testes sem o celular)
// ---------------------------------------------------------------------------

static void handleSerialLine(String s) {
  s.trim();
  if (s.isEmpty()) return;
  JsonDocument d;
  if (s.startsWith("/wifi ")) {
    String arg = s.substring(6);
    int sep = arg.indexOf(';');
    d["t"] = "cfg";
    d["ssid"] = sep >= 0 ? arg.substring(0, sep) : arg;
    d["pass"] = sep >= 0 ? arg.substring(sep + 1) : "";
  } else if (s == "/status") {
    d["t"] = "status";
  } else if (s == "/reset") {
    d["t"] = "reset";
  } else if (s == "/som" || s == "/teste") {
    AudioHardware::runSelfTest();
    return;
  } else if (s == "/mic") {
    int raw = AudioHardware::readMicRaw();
    int p2p = AudioHardware::readMicPeakToPeak(60);
    int pct = AudioHardware::getSoundLevelPercent();
    Serial.printf("[MIC] Raw: %d | Pico-a-Pico: %d | Nivel: %d%%\n", raw, p2p, pct);
    return;
  } else {
    d["t"] = "ask";
    d["text"] = s;
  }
  std::string line;
  serializeJson(d, line);
  enqueue(line);
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n\n=== %s — assistente ESP32 v%s ===\n", ASSISTANT_NAME, FIRMWARE_VERSION);

  settings.load();
  Tools::begin();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("esp32-assistente");
  WiFi.setAutoReconnect(true);
  connectWifi();
  configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);

  assistant.begin(&settings);
  inbox = xQueueCreate(6, sizeof(char*));
  Ble.begin(BLE_DEVICE_NAME, [](std::string&& line) { enqueue(line); });

  xTaskCreatePinnedToCore(assistantTask, "assistant", 20 * 1024, nullptr, 1, nullptr, 1);

  Tools::blink(2, 200);
  AudioHardware::playBootChime();  // Toca melodia alegre no alto-falante ao ligar!
  Serial.printf("[SYS] pronto. heap livre: %u bytes\n", ESP.getFreeHeap());
}

void loop() {
  static String buf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (buf.length()) handleSerialLine(buf);
      buf = "";
    } else if (buf.length() < 1000) {
      buf += c;
    }
  }
  delay(20);
}
