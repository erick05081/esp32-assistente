#include "Tools.h"

#include <WiFi.h>

#include "AudioHardware.h"
#include "config.h"

namespace {

struct Timer {
  bool active = false;
  uint32_t due = 0;
  String label;
};

Timer gTimers[MAX_TIMERS];
bool gLed = false;

void setLed(bool on) {
  gLed = on;
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

JsonObject newFunction(JsonArray decls, const char* name, const char* description) {
  JsonObject f = decls.add<JsonObject>();
  f["name"] = name;
  f["description"] = description;
  return f;
}

JsonObject params(JsonObject fn) {
  JsonObject p = fn["parameters"].to<JsonObject>();
  p["type"] = "object";
  return p["properties"].to<JsonObject>();
}

void prop(JsonObject props, const char* name, const char* type, const char* desc) {
  JsonObject o = props[name].to<JsonObject>();
  o["type"] = type;
  o["description"] = desc;
}

void required(JsonObject fn, std::initializer_list<const char*> names) {
  JsonArray r = fn["parameters"]["required"].to<JsonArray>();
  for (const char* n : names) r.add(n);
}

}  // namespace

namespace Tools {

void begin() {
  pinMode(LED_PIN, OUTPUT);
  setLed(false);
  AudioHardware::begin();
}

bool ledOn() { return gLed; }

int activeTimers() {
  int n = 0;
  for (auto& t : gTimers) n += t.active ? 1 : 0;
  return n;
}

void blink(int times, int periodMs) {
  bool prev = gLed;
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(periodMs / 2);
    digitalWrite(LED_PIN, LOW);
    delay(periodMs / 2);
  }
  setLed(prev);
}

void declare(JsonArray d) {
  JsonObject f;

  f = newFunction(d, "set_led", "Liga ou desliga o LED azul da placa ESP32.");
  prop(params(f), "on", "boolean", "true para ligar, false para desligar");
  required(f, {"on"});

  f = newFunction(d, "blink_led", "Pisca o LED da placa algumas vezes (útil para chamar atenção ou como teste).");
  prop(params(f), "times", "integer", "Quantidade de piscadas, de 1 a 10");
  required(f, {"times"});

  f = newFunction(d, "set_timer",
                  "Cria um lembrete/timer. Quando o tempo acabar o ESP32 avisa o usuário por voz no celular. "
                  "Converta minutos e horas para segundos.");
  {
    JsonObject p = params(f);
    prop(p, "seconds", "integer", "Daqui a quantos segundos avisar (máximo 7 dias)");
    prop(p, "label", "string", "Texto curto do lembrete, ex.: 'tirar o bolo do forno'");
  }
  required(f, {"seconds", "label"});

  newFunction(d, "list_timers", "Lista os lembretes/timers ativos e quanto tempo falta para cada um.");

  f = newFunction(d, "cancel_timers", "Cancela lembretes. Se 'label' for vazio, cancela todos.");
  prop(params(f), "label", "string", "Parte do texto do lembrete a cancelar (opcional)");

  f = newFunction(d, "play_sound",
                  "Toca um efeito sonoro ou tom de áudio no alto-falante físico conectado ao ESP32.");
  prop(params(f), "sound", "string", "Tipo de som: 'beep', 'alarm', 'chime', 'success' ou 'chirp'");
  required(f, {"sound"});

  newFunction(d, "get_sound_level",
              "Mede o nível de som e ruído do ambiente usando o microfone analógico conectado ao ESP32.");

  newFunction(d, "get_device_status",
              "Retorna o estado do ESP32: memória livre, tempo ligado, sinal Wi-Fi, IP, LED, temperatura do chip.");
}

void execute(const char* name, JsonObjectConst args, JsonObject result) {
  Serial.printf("[TOOL] %s\n", name);

  if (!strcmp(name, "set_led")) {
    bool on = args["on"] | false;
    setLed(on);
    result["ok"] = true;
    result["led"] = on ? "ligado" : "desligado";

  } else if (!strcmp(name, "blink_led")) {
    int times = constrain((int)(args["times"] | 3), 1, 10);
    blink(times);
    result["ok"] = true;
    result["piscadas"] = times;

  } else if (!strcmp(name, "set_timer")) {
    long seconds = args["seconds"] | 0L;
    String label = args["label"] | "lembrete";
    if (seconds <= 0 || seconds > 7L * 24 * 3600) {
      result["ok"] = false;
      result["error"] = "duração inválida (1 segundo a 7 dias)";
      return;
    }
    for (auto& t : gTimers) {
      if (!t.active) {
        t.active = true;
        t.due = millis() + (uint32_t)seconds * 1000UL;
        t.label = label;
        result["ok"] = true;
        result["dispara_em_segundos"] = seconds;
        return;
      }
    }
    result["ok"] = false;
    result["error"] = "limite de lembretes simultâneos atingido";

  } else if (!strcmp(name, "list_timers")) {
    JsonArray arr = result["timers"].to<JsonArray>();
    for (auto& t : gTimers) {
      if (!t.active) continue;
      JsonObject o = arr.add<JsonObject>();
      o["label"] = t.label;
      o["restante_segundos"] = (long)(t.due - millis()) / 1000;
    }
    result["total"] = arr.size();

  } else if (!strcmp(name, "cancel_timers")) {
    String filter = args["label"] | "";
    filter.toLowerCase();
    int n = 0;
    for (auto& t : gTimers) {
      if (!t.active) continue;
      String l = t.label;
      l.toLowerCase();
      if (filter.isEmpty() || l.indexOf(filter) >= 0) {
        t.active = false;
        n++;
      }
    }
    result["ok"] = true;
    result["cancelados"] = n;

  } else if (!strcmp(name, "play_sound")) {
    String sound = args["sound"] | "beep";
    if (sound == "alarm") AudioHardware::playAlarm();
    else if (sound == "chime") AudioHardware::playBootChime();
    else if (sound == "success") AudioHardware::playSuccess();
    else if (sound == "chirp") AudioHardware::playChirp();
    else AudioHardware::playBeep();
    result["ok"] = true;
    result["som_tocado"] = sound;

  } else if (!strcmp(name, "get_sound_level")) {
    int pct = AudioHardware::getSoundLevelPercent();
    int p2p = AudioHardware::readMicPeakToPeak(60);
    result["ok"] = true;
    result["nivel_som_percentual"] = pct;
    result["amplitude_p2p"] = p2p;
    result["ambiente"] = (pct < 15) ? "silencioso" : (pct < 50) ? "moderado" : "barulhento";

  } else if (!strcmp(name, "get_device_status")) {
    result["heap_livre_bytes"] = ESP.getFreeHeap();
    result["uptime_segundos"] = millis() / 1000;
    result["wifi_conectado"] = WiFi.status() == WL_CONNECTED;
    result["wifi_rssi_dbm"] = WiFi.RSSI();
    result["ip"] = WiFi.localIP().toString();
    result["led"] = gLed ? "ligado" : "desligado";
    result["temperatura_chip_c"] = (int)temperatureRead();
    result["lembretes_ativos"] = activeTimers();

  } else {
    result["ok"] = false;
    result["error"] = "ferramenta desconhecida";
  }
}

void loop(const std::function<void(const String& label)>& onTimerFired) {
  uint32_t now = millis();
  for (auto& t : gTimers) {
    if (t.active && (int32_t)(now - t.due) >= 0) {
      t.active = false;
      onTimerFired(t.label);
    }
  }
}

}  // namespace Tools
