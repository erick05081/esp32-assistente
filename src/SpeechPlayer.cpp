#include "SpeechPlayer.h"

#include <HTTPClient.h>
#include <WiFi.h>

#include <memory>
#include <vector>

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "config.h"

namespace {

constexpr int kSpeakerLedcChannel = 2;
bool gIsSpeaking = false;

String urlEncode(const String& str) {
  String encoded = "";
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (isalnum((unsigned char)c)) {
      encoded += c;
    } else if (c == ' ') {
      encoded += '+';
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      encoded += buf;
    }
  }
  return encoded;
}

String cleanForSpeech(const String& text) {
  String out = "";
  bool inBracket = false;
  for (unsigned int i = 0; i < text.length(); i++) {
    char c = text.charAt(i);
    if (c == '[') { inBracket = true; continue; }
    if (c == ']') { inBracket = false; continue; }
    if (inBracket) continue;
    // Remove markdown e caracteres que soam estranho em TTS
    if (c == '*' || c == '#' || c == '`' || c == '_' || c == '>' || c == '~' || c == '|' || c == '\\') continue;
    out += c;
  }
  out.trim();
  return out;
}

std::vector<String> splitSentences(const String& text, size_t maxLen = 90) {
  std::vector<String> res;
  String current = "";

  for (unsigned int i = 0; i < text.length(); i++) {
    char c = text.charAt(i);
    current += c;
    if ((c == '.' || c == '!' || c == '?' || c == ';' || c == ':') || current.length() >= maxLen) {
      current.trim();
      if (!current.isEmpty()) {
        res.push_back(current);
        current = "";
      }
    }
  }
  current.trim();
  if (!current.isEmpty()) {
    res.push_back(current);
  }
  return res;
}

bool playSentence(const String& sentence) {
  if (sentence.isEmpty() || WiFi.status() != WL_CONNECTED) return false;

  String enc = urlEncode(sentence);
  String url = "http://translate.google.com/translate_tts?ie=UTF-8&client=tw-ob&tl=pt-BR&q=" + enc;

  HTTPClient http;
  http.begin(url);
  http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
  http.setTimeout(10000);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[TTS] Erro HTTP %d ao buscar audio\n", httpCode);
    http.end();
    return false;
  }

  int len = http.getSize();
  WiFiClient* stream = http.getStreamPtr();
  std::vector<uint8_t> mp3Buf;
  mp3Buf.reserve(len > 0 ? len : 16384);

  uint32_t startRead = millis();
  while (http.connected() && (len > 0 ? (int)mp3Buf.size() < len : true) && (millis() - startRead < 8000)) {
    size_t avail = stream->available();
    if (avail > 0) {
      size_t cur = mp3Buf.size();
      mp3Buf.resize(cur + avail);
      stream->readBytes((char*)&mp3Buf[cur], avail);
    } else {
      delay(5);
      if (!stream->available() && !http.connected()) break;
    }
  }
  http.end();

  if (mp3Buf.empty()) {
    Serial.println("[TTS] Buffer MP3 vazio");
    return false;
  }

  Serial.printf("[TTS] Audio recebido (%u bytes). Reproduzindo no alto-falante...\n", (unsigned int)mp3Buf.size());

  // Inicializa o decodificador minimp3 no heap para proteger a pilha do FreeRTOS
  std::unique_ptr<mp3dec_t> dec(new mp3dec_t);
  mp3dec_init(dec.get());

  mp3dec_frame_info_t info;
  std::vector<int16_t> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME);

  size_t offset = 0;
  size_t mp3Size = mp3Buf.size();
  const uint8_t* data = mp3Buf.data();

  // Configura LEDC para PWM de alta frequencia (62.5 kHz, 8 bits) no falante (GPIO 18)
  ledcSetup(kSpeakerLedcChannel, 62500, 8);
  ledcAttachPin(SPEAKER_PIN, kSpeakerLedcChannel);

  while (offset < mp3Size) {
    int samples = mp3dec_decode_frame(dec.get(), data + offset, mp3Size - offset, pcm.data(), &info);
    if (samples > 0) {
      offset += info.frame_bytes;
      uint32_t intervalUs = (info.hz > 0) ? (1000000 / info.hz) : 41;

      for (int i = 0; i < samples; i++) {
        int16_t s = pcm[i * info.channels];
        // Converte amostra assinada de 16-bit (-32768..32767) para 8-bit PWM (0..255)
        int pwmVal = (s >> 8) + 128;
        if (pwmVal < 0) pwmVal = 0;
        if (pwmVal > 255) pwmVal = 255;
        ledcWrite(kSpeakerLedcChannel, pwmVal);
        delayMicroseconds(intervalUs);
      }
    } else {
      offset += (info.frame_bytes > 0) ? info.frame_bytes : 1;
    }
  }

  // Corta o PWM para zerar corrente continua no alto-falante e no transistor
  ledcWrite(kSpeakerLedcChannel, 0);
  digitalWrite(SPEAKER_PIN, LOW);

  return true;
}

}  // namespace

namespace SpeechPlayer {

void begin() {
  pinMode(SPEAKER_PIN, OUTPUT);
  digitalWrite(SPEAKER_PIN, LOW);
}

bool isSpeaking() {
  return gIsSpeaking;
}

bool say(const String& text) {
  String cleaned = cleanForSpeech(text);
  if (cleaned.isEmpty()) return false;

  gIsSpeaking = true;
  auto parts = splitSentences(cleaned);
  bool anyOk = false;

  for (const auto& part : parts) {
    if (part.length() > 0) {
      if (playSentence(part)) {
        anyOk = true;
        delay(60); // Pequena pausa natural entre frases
      }
    }
  }

  gIsSpeaking = false;
  return anyOk;
}

}  // namespace SpeechPlayer
