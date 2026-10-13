#pragma once
#include <Arduino.h>

namespace SpeechPlayer {

void begin();

// Converte texto em fala via TTS em português e reproduz no alto-falante (GPIO 18)
bool say(const String& text);

// Retorna se está reproduzindo áudio no momento
bool isSpeaking();

}  // namespace SpeechPlayer
