#include "AudioHardware.h"
#include "Tools.h"
#include "config.h"

namespace {

constexpr int kLedcChannel = 2;       // canal LEDC para o alto-falante
constexpr int kLedcResolution = 8;    // resolução de 8 bits (0-255)
constexpr int kSilenceDuty = 0;       // duty cycle 0 = nível baixo (protege o falante)
constexpr int kActiveDuty = 128;      // duty cycle 50% = onda quadrada máxima

bool gInitialized = false;

// Feedback ativo do microfone no LED onboard (GPIO 2)
bool gMicLedFeedback = false;
bool gLedActiveByMic = false;
uint32_t gLastMicSampleTime = 0;
uint32_t gLedPulseUntil = 0;
uint32_t gNextBlinkAllowed = 0;
int gAdaptiveNoiseFloor = 40;
uint32_t gLastFloorDecay = 0;

}  // namespace

namespace AudioHardware {

void begin() {
  if (gInitialized) return;

  // Configuração inicial do alto-falante no GPIO 18
  pinMode(SPEAKER_PIN, OUTPUT);
  digitalWrite(SPEAKER_PIN, LOW);
  ledcSetup(kLedcChannel, 2000, kLedcResolution);

  // Configuração do microfone no GPIO 34 (ADC1_CH6)
  pinMode(MIC_PIN, INPUT);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);

  // Calibra o piso inicial de ruído real do circuito
  int sumNoise = 0;
  for (int i = 0; i < 4; i++) {
    sumNoise += readMicPeakToPeak(25);
    delay(10);
  }
  gAdaptiveNoiseFloor = constrain(sumNoise / 4, 60, 400);

  gInitialized = true;
  Serial.printf("[AUDIO] Driver inicializado. Falante: GPIO %d, Mic: GPIO %d (piso ruído: %d)\n",
                SPEAKER_PIN, MIC_PIN, gAdaptiveNoiseFloor);
}

void playTone(unsigned int freq, unsigned int durationMs) {
  if (!gInitialized) begin();

  if (freq > 0) {
    ledcAttachPin(SPEAKER_PIN, kLedcChannel);
    ledcWriteTone(kLedcChannel, freq);
  } else {
    stopTone();
  }

  delay(durationMs);
  stopTone();
}

void stopTone() {
  ledcWriteTone(kLedcChannel, 0);
  ledcDetachPin(SPEAKER_PIN);
  pinMode(SPEAKER_PIN, OUTPUT);
  digitalWrite(SPEAKER_PIN, LOW);
}

void playBootChime() {
  // Som de inicialização: Do5 (523Hz), Mi5 (659Hz), Sol5 (784Hz), Do6 (1046Hz)
  playTone(523, 70);
  delay(20);
  playTone(659, 70);
  delay(20);
  playTone(784, 70);
  delay(20);
  playTone(1046, 160);
  delay(50);
}

void playAlarm() {
  // Toque de alarme / lembrete: 3 pulsos agudos chamativos
  for (int i = 0; i < 3; i++) {
    playTone(1800, 100);
    delay(40);
    playTone(2400, 150);
    delay(80);
  }
}

void playSuccess() {
  playTone(880, 80);
  delay(30);
  playTone(1320, 140);
}

void playBeep() {
  playTone(1200, 70);
}

void playChirp() {
  // Efeito estilo assistente de voz pronto para ouvir
  for (int f = 600; f <= 1600; f += 200) {
    playTone(f, 15);
  }
}

int readMicRaw() {
  return analogRead(MIC_PIN);
}

int readMicPeakToPeak(unsigned int windowMs) {
  uint32_t start = millis();
  int minVal = 4095;
  int maxVal = 0;

  while (millis() - start < windowMs) {
    int sample = analogRead(MIC_PIN);
    if (sample < minVal) minVal = sample;
    if (sample > maxVal) maxVal = sample;
  }

  int p2p = maxVal - minVal;
  return (p2p >= 0) ? p2p : 0;
}

int getSoundLevelPercent() {
  int p2p = readMicPeakToPeak(50);
  // Ruído de fundo típico: 30 a 80 unidades. Som de voz: 200 a 1200 unidades.
  int level = map(p2p, 30, 900, 0, 100);
  return constrain(level, 0, 100);
}

bool detectVoice() {
  int p2p = readMicPeakToPeak(35);

  // Acompanha o ruído ambiente dinamicamente (para cima e para baixo suavemente)
  if (p2p < gAdaptiveNoiseFloor) {
    gAdaptiveNoiseFloor = (gAdaptiveNoiseFloor * 7 + p2p) / 8;
  } else if (p2p < gAdaptiveNoiseFloor + 80) {
    gAdaptiveNoiseFloor = (gAdaptiveNoiseFloor * 31 + p2p) / 32;
  }

  // Limiar para voz humana: precisa superar o piso de ruído elétrico/ambiente em pelo menos 130
  // e ter amplitude mínima de 300 (evita que zumbido ou ruído da fonte de alimentação dispare)
  int threshold = max(gAdaptiveNoiseFloor + 130, 300);

  if (p2p >= threshold) {
    // Confirmação de dois estágios: voz humana dura várias centenas de milissegundos,
    // enquanto ruído de RF/Wi-Fi ou clique elétrico dura menos de 1ms.
    delay(15);
    int p2p_confirm = readMicPeakToPeak(25);
    if (p2p_confirm >= (threshold - 50)) {
      return true;
    }
  }
  return false;
}

bool detectLoudSound(int threshold) {
  return readMicPeakToPeak(35) > threshold;
}

void setMicLedFeedback(bool enabled) {
  gMicLedFeedback = enabled;
  if (!enabled && gLedActiveByMic) {
    gLedActiveByMic = false;
    digitalWrite(LED_PIN, Tools::ledOn() ? HIGH : LOW);
  }
  Serial.printf("[AUDIO] Feedback do microfone no LED: %s\n", enabled ? "ATIVADO" : "DESATIVADO");
}

bool isMicLedFeedbackEnabled() {
  return gMicLedFeedback;
}

void updateMicLedFeedback() {
  if (!gMicLedFeedback) return;
  if (Tools::isBlinking()) return;  // Não sobrepõe piscadas de alerta do sistema

  uint32_t now = millis();

  // Se o LED foi ativado pelo som da voz e acabou a duração da piscada, restaura
  if (gLedActiveByMic && now >= gLedPulseUntil) {
    gLedActiveByMic = false;
    digitalWrite(LED_PIN, Tools::ledOn() ? HIGH : LOW);
  }

  // Faz leitura em intervalos de 25ms
  if (now - gLastMicSampleTime < 25) return;
  gLastMicSampleTime = now;

  // Decai lentamente o piso de ruído para se adaptar ao ambiente se a sala ficar quieta
  if (now - gLastFloorDecay >= 2000) {
    gLastFloorDecay = now;
    if (gAdaptiveNoiseFloor > 60) gAdaptiveNoiseFloor -= 3;
  }

  // Amostragem rápida em janela curta (20 amostras com 70us entre elas = ~1.6ms)
  int minVal = 4095;
  int maxVal = 0;
  for (int i = 0; i < 20; i++) {
    int s = analogRead(MIC_PIN);
    if (s < minVal) minVal = s;
    if (s > maxVal) maxVal = s;
    delayMicroseconds(70);
  }

  int p2p = maxVal - minVal;

  // Atualiza o piso de ruído
  if (p2p < gAdaptiveNoiseFloor) {
    gAdaptiveNoiseFloor = (gAdaptiveNoiseFloor * 7 + p2p) / 8;
  } else if (p2p < gAdaptiveNoiseFloor + 70) {
    gAdaptiveNoiseFloor = (gAdaptiveNoiseFloor * 31 + p2p) / 32;
  }

  // Limiar de disparo do LED: apenas quando há voz real acima do ruído contínuo
  int threshold = max(gAdaptiveNoiseFloor + 110, 280);

  // Se detectou voz/som acima do limiar
  if (p2p >= threshold) {
    // Pisca o LED com cadência de ~10 Hz (50ms ligado, 45ms desligado)
    if (now >= gNextBlinkAllowed) {
      digitalWrite(LED_PIN, HIGH);
      gLedActiveByMic = true;
      gLedPulseUntil = now + 50;
      gNextBlinkAllowed = now + 95;
    }
  }
}

void runSelfTest() {
  Serial.println("\n--- [AUDIO SELF-TEST] ---");
  Serial.println("1. Testando Alto-Falante (escala musical)...");
  playBootChime();
  delay(300);

  Serial.println("2. Testando Microfone (GPIO 34) e LED (GPIO 2) por 4 segundos...");
  Serial.println("   (Fale perto do microfone ou bata palma: o LED piscará a cada palavra!)");
  uint32_t t0 = millis();
  while (millis() - t0 < 4000) {
    updateMicLedFeedback();
    int raw = readMicRaw();
    int p2p = readMicPeakToPeak(35);
    int pct = getSoundLevelPercent();
    Serial.printf("   Raw: %4d | P2P: %4d | Nivel: %3d%% | LED: %s\n",
                  raw, p2p, pct, digitalRead(LED_PIN) ? "LIGADO (VOZ)" : "desligado");
    delay(100);
  }
  Serial.println("--- [FIM DO TESTE] ---\n");
  playSuccess();
}

bool recordWav(std::vector<uint8_t>& outWav, unsigned int durationMs) {
  const uint32_t sampleRate = 8000;
  const uint32_t numSamples = (sampleRate * durationMs) / 1000;
  const uint32_t dataSize = numSamples * 2;
  const uint32_t fileSize = 44 + dataSize;

  outWav.resize(fileSize);
  uint8_t* buf = outWav.data();

  // Cabeçalho WAV RIFF
  memcpy(buf, "RIFF", 4);
  uint32_t riffSize = fileSize - 8;
  memcpy(buf + 4, &riffSize, 4);
  memcpy(buf + 8, "WAVEfmt ", 8);
  uint32_t subchunk1Size = 16;
  memcpy(buf + 16, &subchunk1Size, 4);
  uint16_t audioFormat = 1; // PCM linear
  memcpy(buf + 20, &audioFormat, 2);
  uint16_t numChannels = 1; // Mono
  memcpy(buf + 22, &numChannels, 2);
  memcpy(buf + 24, &sampleRate, 4);
  uint32_t byteRate = sampleRate * numChannels * 2;
  memcpy(buf + 28, &byteRate, 4);
  uint16_t blockAlign = numChannels * 2;
  memcpy(buf + 32, &blockAlign, 2);
  uint16_t bitsPerSample = 16;
  memcpy(buf + 34, &bitsPerSample, 2);
  memcpy(buf + 36, "data", 4);
  memcpy(buf + 40, &dataSize, 4);

  int16_t* samples = (int16_t*)(buf + 44);

  // Calcula baseline inicial (DC offset)
  long sumDc = 0;
  for (int i = 0; i < 64; i++) {
    sumDc += analogRead(MIC_PIN);
    delayMicroseconds(50);
  }
  int dcOffset = sumDc / 64;

  uint32_t intervalUs = 1000000 / sampleRate;
  uint32_t nextSampleTime = micros();

  // LED aceso continuamente enquanto grava a voz do usuário
  digitalWrite(LED_PIN, HIGH);

  float lastX = 0;
  float lastY = 0;

  for (uint32_t i = 0; i < numSamples; i++) {
    while ((int32_t)(micros() - nextSampleTime) < 0) {
      // espera precisa
    }
    nextSampleTime += intervalUs;

    int raw = analogRead(MIC_PIN);
    // Filtro passa-altas de 1ª ordem (remove offset DC e atenua zumbido elétrico de 60Hz da fonte)
    float x = (float)raw;
    float y = x - lastX + 0.94f * lastY;
    lastX = x;
    lastY = y;

    // Ganho digital de 12x sobre o sinal de voz filtrado
    int val = (int)(y * 12.0f);
    if (val > 32767) val = 32767;
    if (val < -32768) val = -32768;

    samples[i] = (int16_t)val;
  }

  digitalWrite(LED_PIN, LOW);
  return true;
}

}  // namespace AudioHardware
