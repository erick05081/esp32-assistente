#include "Assistant.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <base64.h>
#include <time.h>

#include <deque>

#include "Settings.h"
#include "Tools.h"
#include "config.h"

namespace {

struct Msg {
  bool user;
  String text;
};

std::deque<Msg> gHistory;
Settings* gSettings = nullptr;

constexpr int kMaxToolRounds = 5;

String nowDescription() {
  struct tm t;
  if (!getLocalTime(&t, 20)) return "";
  static const char* kDays[] = {"domingo", "segunda-feira", "terça-feira", "quarta-feira",
                                "quinta-feira", "sexta-feira", "sábado"};
  static const char* kMonths[] = {"janeiro", "fevereiro", "março",    "abril",   "maio",     "junho",
                                  "julho",   "agosto",    "setembro", "outubro", "novembro", "dezembro"};
  char buf[96];
  snprintf(buf, sizeof buf, "%s, %d de %s de %d, %02d:%02d", kDays[t.tm_wday], t.tm_mday,
           kMonths[t.tm_mon], t.tm_year + 1900, t.tm_hour, t.tm_min);
  return buf;
}

String systemPrompt() {
  String s;
  s.reserve(1200);
  s += "Você é " ASSISTANT_NAME ", um assistente pessoal de voz que roda em um microcontrolador ESP32. ";
  s += "O usuário fala com você pelo celular: a fala dele é transcrita para texto e a sua resposta "
       "será lida em voz alta por síntese de voz. ";
  s += "Regras: responda sempre em português do Brasil, de forma natural, simpática e BREVE "
       "(normalmente 1 a 3 frases, a menos que peçam detalhes). ";
  s += "Nunca use markdown, listas, asteriscos, emojis, tabelas ou código: apenas texto corrido, fácil de ouvir. ";
  s += "Escreva números, horas e unidades de forma que soem bem quando falados. ";
  s += "Se a transcrição parecer truncada ou sem sentido, peça gentilmente para repetir. ";
  s += "Você pode controlar o LED da placa, tocar sons no alto-falante físico, medir o nível de ruído pelo microfone, "
       "criar, listar e cancelar lembretes e consultar o status do dispositivo usando as ferramentas disponíveis; "
       "use-as quando o pedido envolver isso e confirme o que fez. ";
  s += "Você não tem acesso à internet em tempo real (notícias, clima, cotações); se pedirem, "
       "diga isso brevemente e ofereça o que souber. ";
  String now = nowDescription();
  if (now.length()) {
    s += "Data e hora atuais (Brasília): ";
    s += now;
    s += ". ";
  }
  return s;
}

bool isRetryable(int code) { return code <= 0 || code == 429 || code == 500 || code == 503 || code == 504; }

String friendlyError(int code, const String& detail) {
  if (code == 429) return "Atingi o limite de uso da API do Gemini. Tente de novo em instantes.";
  if (code == 503 || code == 500 || code == 504)
    return "Os servidores do Gemini estão sobrecarregados agora. Tente novamente daqui a pouco.";
  if (code == 400 && detail.indexOf("API key") >= 0) return "A chave da API do Gemini parece inválida.";
  if (code == 401 || code == 403) return "A chave da API do Gemini foi recusada. Verifique a configuração.";
  if (code <= 0) return "Não consegui falar com o servidor do Gemini. Verifique a conexão Wi-Fi.";
  return "Erro ao consultar o Gemini (" + String(code) + "). " + detail;
}

// Faz um POST generateContent. Retorna o código HTTP (<=0 em erro de rede).
int postGemini(const String& model, const String& body, JsonDocument& out, String& detail) {
  WiFiClientSecure client;
  client.setInsecure();  // simplicidade: sem validação de certificado (ver README)

  HTTPClient http;
  String url = "https://generativelanguage.googleapis.com/v1beta/models/" + model + ":generateContent";
  if (!http.begin(client, url)) {
    detail = "falha ao iniciar HTTPS";
    return -1;
  }
  http.useHTTP10(true);  // evita chunked encoding -> permite parse direto do stream
  http.setConnectTimeout(15000);
  http.setTimeout(GEMINI_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-goog-api-key", gSettings->apiKey);

  int code = http.POST(body);
  if (code <= 0) {
    detail = http.errorToString(code);
    http.end();
    return code;
  }

  // Guardamos só o necessário (o conteúdo completo do candidato é preciso
  // para devolver 'thoughtSignature' nas chamadas de função).
  JsonDocument filter;
  filter["candidates"][0]["content"] = true;
  filter["candidates"][0]["finishReason"] = true;
  filter["error"]["message"] = true;
  filter["modelVersion"] = true;

  DeserializationError err = deserializeJson(out, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (code != 200) {
    detail = out["error"]["message"] | "";
    return code;
  }
  if (err) {
    detail = String("resposta inválida: ") + err.c_str();
    return -2;
  }
  return code;
}

}  // namespace

void Assistant::begin(Settings* settings) { gSettings = settings; }

void Assistant::clearHistory() { gHistory.clear(); }

size_t Assistant::historySize() const { return gHistory.size(); }

Assistant::Result Assistant::ask(const String& userText, const ToolNotifier& onTool) {
  Result res;

  if (WiFi.status() != WL_CONNECTED) {
    res.text = "Estou sem conexão Wi-Fi. Configure a rede nas configurações do app.";
    return res;
  }
  if (gSettings->apiKey.isEmpty()) {
    res.text = "Falta configurar a chave da API do Gemini.";
    return res;
  }

  // ---- Monta a requisição -------------------------------------------------
  JsonDocument req;
  req["systemInstruction"]["parts"].to<JsonArray>().add<JsonObject>()["text"] = systemPrompt();

  JsonArray contents = req["contents"].to<JsonArray>();
  for (const auto& m : gHistory) {
    JsonObject c = contents.add<JsonObject>();
    c["role"] = m.user ? "user" : "model";
    c["parts"].to<JsonArray>().add<JsonObject>()["text"] = m.text;
  }
  {
    JsonObject c = contents.add<JsonObject>();
    c["role"] = "user";
    c["parts"].to<JsonArray>().add<JsonObject>()["text"] = userText;
  }

  JsonArray decls = req["tools"].to<JsonArray>().add<JsonObject>()["functionDeclarations"].to<JsonArray>();
  Tools::declare(decls);
  req["generationConfig"]["temperature"] = 0.7;

  // Modelos: o configurado e, se falhar por sobrecarga/limite, o reserva.
  String models[2] = {gSettings->model, FALLBACK_MODEL};
  int modelCount = (gSettings->model == FALLBACK_MODEL) ? 1 : 2;
  int modelIdx = 0;

  String reply;

  for (int round = 0; round < kMaxToolRounds; round++) {
    String body;
    serializeJson(req, body);

    JsonDocument resp;
    int code = 0;
    String detail;
    while (true) {
      uint32_t t0 = millis();
      code = postGemini(models[modelIdx], body, resp, detail);
      Serial.printf("[GEMINI] %s -> HTTP %d em %lu ms (heap %u)\n", models[modelIdx].c_str(), code,
                    (unsigned long)(millis() - t0), ESP.getFreeHeap());
      if (code == 200) break;
      Serial.printf("[GEMINI] erro: %s\n", detail.c_str());
      if (isRetryable(code) && modelIdx + 1 < modelCount) {
        modelIdx++;  // tenta o modelo reserva
        resp.clear();
        delay(300);
        continue;
      }
      break;
    }
    if (code != 200) {
      res.text = friendlyError(code, detail);
      return res;
    }

    JsonObject content = resp["candidates"][0]["content"];
    if (content.isNull() || !content["parts"].is<JsonArray>()) {
      const char* reason = resp["candidates"][0]["finishReason"] | "desconhecido";
      Serial.printf("[GEMINI] sem conteúdo, finishReason=%s\n", reason);
      res.text = "Desculpe, não consegui responder isso.";
      return res;
    }

    // ---- Processa a resposta: texto e/ou chamadas de função ----------------
    bool hadCalls = false;
    JsonArray toolParts;
    String text;
    for (JsonObject part : content["parts"].as<JsonArray>()) {
      if (part["thought"] | false) continue;
      if (part["text"].is<const char*>()) text += part["text"].as<const char*>();

      JsonObject call = part["functionCall"];
      if (call.isNull()) continue;
      if (!hadCalls) {
        hadCalls = true;
        contents.add(content);  // devolve o turno do modelo (com thoughtSignature)
        JsonObject turn = contents.add<JsonObject>();
        turn["role"] = "user";
        toolParts = turn["parts"].to<JsonArray>();
      }
      const char* name = call["name"] | "";
      JsonObject fr = toolParts.add<JsonObject>()["functionResponse"].to<JsonObject>();
      fr["name"] = name;
      if (call["id"].is<const char*>()) fr["id"] = call["id"];
      Tools::execute(name, call["args"].as<JsonObjectConst>(), fr["response"].to<JsonObject>());
      if (onTool) onTool(name);
    }

    if (!hadCalls) {
      reply = text;
      break;
    }
  }

  reply.trim();
  if (reply.isEmpty()) reply = "Pronto.";

  // ---- Atualiza memória da conversa --------------------------------------
  gHistory.push_back({true, userText});
  gHistory.push_back({false, reply});
  while (gHistory.size() > HISTORY_MAX_MSGS) {
    gHistory.pop_front();
    gHistory.pop_front();
  }

  res.ok = true;
  res.text = reply;
  res.model = models[modelIdx];
  return res;
}

Assistant::Result Assistant::askAudio(std::vector<uint8_t>& wav, const ToolNotifier& onTool) {
  Result res;

  if (WiFi.status() != WL_CONNECTED) {
    res.text = "Estou sem conexão Wi-Fi. Configure a rede pelo app.";
    return res;
  }
  if (gSettings->apiKey.isEmpty()) {
    res.text = "Falta configurar a chave da API do Gemini.";
    return res;
  }
  if (wav.size() < 44) {
    res.text = "Áudio vazio.";
    return res;
  }

  // 1. Gera base64 da gravação
  String b64 = base64::encode(wav.data(), wav.size());

  // 2. Libera a memória do vetor de áudio bruto IMEDIATAMENTE (recupera ~29 KB de RAM!)
  wav.clear();
  wav.shrink_to_fit();

  // 3. Monta o corpo JSON diretamente na String body para evitar duplicar
  //    a base64 dentro de uma estrutura JsonDocument (isso economiza > 50 KB de RAM e evita erro BIGNUM no SSL)
  String body;
  body.reserve(b64.length() + 2500);
  body = "{\"contents\":[{\"role\":\"user\",\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"";
  body += b64;
  body += "\"}}]}],\"systemInstruction\":{\"parts\":[{\"text\":\"";
  body += "Você é " ASSISTANT_NAME ", uma assistente de voz em um interfone com alto-falante e microfone. "
          "O usuário falou pelo microfone físico. "
          "1. Se o áudio for apenas silêncio, ruído estático ou inaudível, responda exatamente: '[IGNORAR]'. "
          "2. Se for uma chamada ('Aurora') ou pergunta/pedido, responda em português com texto limpo, natural e DIRETO (máximo 1 a 2 frases curtas), "
          "sem emojis, sem markdown e sem listas, pois sua resposta será falada em voz alta no alto-falante físico do interfone. "
          "Use as ferramentas disponíveis para controlar LED, timers ou status se solicitado.";
  body += "\"}}],";

  // Declara ferramentas
  {
    JsonDocument toolsDoc;
    JsonArray decls = toolsDoc["tools"].to<JsonArray>().add<JsonObject>()["functionDeclarations"].to<JsonArray>();
    Tools::declare(decls);
    String toolsStr;
    serializeJson(toolsDoc, toolsStr);
    if (toolsStr.startsWith("{") && toolsStr.endsWith("}")) {
      body += toolsStr.substring(1, toolsStr.length() - 1);
      body += ",";
    }
  }

  body += "\"generationConfig\":{\"temperature\":0.4}}";

  // 4. Libera a String b64 IMEDIATAMENTE antes da conexão SSL!
  b64 = "";

  Serial.printf("[GEMINI-AUDIO] Payload pronto. Heap livre antes do SSL: %u bytes\n", ESP.getFreeHeap());

  String models[2] = {gSettings->model, FALLBACK_MODEL};
  int modelCount = (gSettings->model == FALLBACK_MODEL) ? 1 : 2;
  int modelIdx = 0;
  String reply;

  JsonDocument resp;
  int code = 0;
  String detail;
  while (true) {
    uint32_t t0 = millis();
    code = postGemini(models[modelIdx], body, resp, detail);
    Serial.printf("[GEMINI-AUDIO] %s -> HTTP %d em %lu ms (heap %u)\n", models[modelIdx].c_str(), code,
                  (unsigned long)(millis() - t0), ESP.getFreeHeap());
    if (code == 200) break;
    Serial.printf("[GEMINI-AUDIO] erro: %s\n", detail.c_str());
    if (isRetryable(code) && modelIdx + 1 < modelCount) {
      modelIdx++;
      resp.clear();
      delay(300);
      continue;
    }
    break;
  }
  if (code != 200) {
    res.text = friendlyError(code, detail);
    return res;
  }

  JsonObject content = resp["candidates"][0]["content"];
  JsonArray parts = content["parts"];
  if (parts.isNull() || parts.size() == 0) {
    res.text = "Não consegui entender.";
    return res;
  }

  String text;
  for (JsonObject part : parts) {
    if (part["thought"] | false) continue;
    if (part["text"].is<const char*>()) text += part["text"].as<const char*>();

    JsonObject call = part["functionCall"];
    if (call.isNull()) continue;
    const char* name = call["name"] | "";
    JsonObject dummyResp;
    Tools::execute(name, call["args"].as<JsonObjectConst>(), dummyResp);
    if (onTool) onTool(name);
  }

  reply = text;
  reply.trim();
  if (reply.isEmpty()) reply = "[IGNORAR]";

  if (reply != "[IGNORAR]") {
    gHistory.push_back({true, "(áudio do microfone)"});
    gHistory.push_back({false, reply});
    while (gHistory.size() > HISTORY_MAX_MSGS) {
      gHistory.pop_front();
      gHistory.pop_front();
    }
  }

  res.ok = true;
  res.text = reply;
  res.model = models[modelIdx];
  return res;
}

Assistant::Result Assistant::askAudio(const uint8_t* wavBytes, size_t wavLen, const ToolNotifier& onTool) {
  if (!wavBytes || wavLen < 44) {
    Result r;
    r.text = "Áudio vazio.";
    return r;
  }
  std::vector<uint8_t> temp(wavBytes, wavBytes + wavLen);
  return askAudio(temp, onTool);
}

