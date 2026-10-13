#pragma once
#include <Arduino.h>
#include <vector>

// Driver de hardware para o Alto-falante (GPIO 18) e Microfone de Eletreto (GPIO 34).
namespace AudioHardware {

void begin();

// ---- Alto-falante (GPIO 18 via 2N2222A) ----
void playTone(unsigned int freq, unsigned int durationMs);
void stopTone();
void playBootChime();
void playAlarm();
void playSuccess();
void playBeep();
void playChirp();

// ---- Microfone de Eletreto (GPIO 34 / ADC1) ----
// Retorna a amplitude pico-a-pico (volume acústico real) em uma janela de tempo.
int readMicPeakToPeak(unsigned int windowMs = 40);

// Retorna leitura bruta instantânea do ADC (0 a 4095)
int readMicRaw();

// Retorna nível de som/ruído estimado de 0% a 100%
int getSoundLevelPercent();

// Retorna true se detectar um som alto (palma, assobio, voz próxima)
bool detectLoudSound(int threshold = 350);

// ---- Feedback Visual Ativo com LED (GPIO 2) ----
// Pisca o LED onboard em tempo real quando o microfone detecta voz/som
void setMicLedFeedback(bool enabled);
bool isMicLedFeedbackEnabled();
void updateMicLedFeedback();

// Executa um autoteste de hardware (escala musical no falante + leitura do mic)
void runSelfTest();

// Grava áudio do microfone (GPIO 34) em formato WAV (8kHz, 16-bit mono com ganho digital)
bool recordWav(std::vector<uint8_t>& outWav, unsigned int durationMs = 2800);

}  // namespace AudioHardware
