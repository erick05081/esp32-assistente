#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

#include <functional>

// Ferramentas (function calling) que o Gemini pode acionar no ESP32:
// LED, lembretes/timers e status do dispositivo.
namespace Tools {

void begin();

// Adiciona as declarações das funções no formato da API Gemini.
void declare(JsonArray functionDeclarations);

// Executa uma função pedida pelo modelo e preenche 'result'.
void execute(const char* name, JsonObjectConst args, JsonObject result);

// Deve ser chamado periodicamente; dispara os lembretes vencidos.
void loop(const std::function<void(const String& label)>& onTimerFired);

bool ledOn();
void blink(int times, int periodMs = 300);
int activeTimers();

}  // namespace Tools
