#pragma once

// ---------------------------------------------------------------------------
// Configurações gerais do assistente
// ---------------------------------------------------------------------------

#define FIRMWARE_VERSION   "1.0.0"
#define ASSISTANT_NAME     "Aurora"            // nome com que o assistente se apresenta
#define BLE_DEVICE_NAME    "ESP32-Assistente"  // nome anunciado via Bluetooth

// LED azul onboard da maioria das placas ESP32 DevKit
#define LED_PIN            2

// Pinos de Áudio (Interfone: Alto-falante de 8 ohms e Microfone de eletreto)
#define SPEAKER_PIN        18   // Alto-falante PWM via 2N2222A (lado direito)
#define MIC_PIN            34   // Microfone analógico ADC1_CH6 (lado esquerdo)

// Fuso horário de Brasília (POSIX TZ). Sem horário de verão.
#define TZ_INFO            "<-03>3"
#define NTP_SERVER_1       "pool.ntp.org"
#define NTP_SERVER_2       "time.google.com"

// Modelos Gemini. Se o principal estiver sobrecarregado (503/429),
// o firmware tenta automaticamente o modelo reserva.
#define DEFAULT_MODEL      "gemini-flash-latest"
#define FALLBACK_MODEL     "gemini-flash-lite-latest"

// Quantas mensagens (usuário + assistente) manter como memória da conversa
#define HISTORY_MAX_MSGS   10

// Timeout da requisição HTTPS ao Gemini (ms)
#define GEMINI_TIMEOUT_MS  45000

// Máximo de lembretes/timers simultâneos
#define MAX_TIMERS         6
