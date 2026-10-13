#include "AudioHardware.h"
#include "config.h"

namespace {

constexpr int kLedcChannel = 2;       // canal LEDC para o alto-falante
constexpr int kLedcResolution = 8;    // resolução de 8 bits (0-255)
constexpr int kSilenceDuty = 0;       // duty cycle 0 = nível baixo (protege o falante)
constexpr int kActiveDuty = 128;      // duty cycle 50% = onda quadrada máxima

bool gInitialized = false;

}  // namespace

namespace AudioHardware {

void begin() {
  if (gInitialized) return;

  // Configuração do alto-falante no GPIO 18
  pinMode(SPEAKER_PIN, OUTPUT);
  digitalWrite(SPEAKER_PIN, LOW);
  ledcSetup(kLedcChannel, 2000, kLedcResolution);
  ledcAttachPin(SPEAKER_PIN, kLedcChannel);
  ledcWrite(kLedcChannel, kSilenceDuty);

  // Configuração do microfone no GPIO 34 (ADC1_CH6)
  pinMode(MIC_PIN, INPUT);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);

  gInitialized = true;
  Serial.printf("[AUDIO] Driver inicializado. Falante: GPIO %d, Mic: GPIO %d\n", SPEAKER_PIN, MIC_PIN);
}

void playTone(unsigned int freq, unsigned int durationMs) {
  if (!gInitialized) begin();

  if (freq > 0) {
    ledcWriteTone(kLedcChannel, freq);
    ledcWrite(kLedcChannel, kActiveDuty);
  } else {
    ledcWrite(kLedcChannel, kSilenceDuty);
  }

  delay(durationMs);
  stopTone();
}

void stopTone() {
  ledcWrite(kLedcChannel, kSilenceDuty);
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

bool detectLoudSound(int threshold) {
  return readMicPeakToPeak(35) > threshold;
}

void runSelfTest() {
  Serial.println("\n--- [AUDIO SELF-TEST] ---");
  Serial.println("1. Testando Alto-Falante (escala musical)...");
  playBootChime();
  delay(300);

  Serial.println("2. Testando Microfone (GPIO 34) por 3 segundos...");
  Serial.println("   (Fale perto do microfone ou bata palma para ver a resposta)");
  uint32_t t0 = millis();
  while (millis() - t0 < 3000) {
    int raw = readMicRaw();
    int p2p = readMicPeakToPeak(50);
    int pct = getSoundLevelPercent();
    Serial.printf("   Raw: %4d | Amplitude P2P: %4d | Nivel: %3d%%\n", raw, p2p, pct);
    delay(150);
  }
  Serial.println("--- [FIM DO TESTE] ---\n");
  playSuccess();
}

}  // namespace AudioHardware
