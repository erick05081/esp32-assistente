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
bool gMicLedFeedback = true;
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

  // Faz leitura em intervalos de 20ms
  if (now - gLastMicSampleTime < 20) return;
  gLastMicSampleTime = now;

  // Decai lentamente o piso de ruído para se adaptar ao ambiente
  if (now - gLastFloorDecay >= 2000) {
    gLastFloorDecay = now;
    if (gAdaptiveNoiseFloor > 35) gAdaptiveNoiseFloor -= 5;
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

  // Atualiza o piso de ruído mínimo
  if (p2p < gAdaptiveNoiseFloor) {
    gAdaptiveNoiseFloor = (gAdaptiveNoiseFloor * 3 + p2p) / 4;
  }

  // Limiar de disparo da voz: piso de ruído + 80 (mínimo 100, máximo 300)
  int threshold = constrain(gAdaptiveNoiseFloor + 80, 100, 300);

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

}  // namespace AudioHardware
