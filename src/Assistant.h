#pragma once
#include <Arduino.h>

#include <functional>
#include <vector>

struct Settings;

// "Cérebro" do assistente: conversa com a API Gemini via HTTPS,
// mantém memória curta da conversa e executa ferramentas (function calling).
class Assistant {
 public:
  struct Result {
    bool ok = false;
    String text;   // resposta (ou mensagem de erro amigável, se !ok)
    String model;  // modelo que respondeu
  };
  using ToolNotifier = std::function<void(const char* toolName)>;

  void begin(Settings* settings);
  Result ask(const String& userText, const ToolNotifier& onTool = nullptr);
  Result askAudio(std::vector<uint8_t>& wav, const ToolNotifier& onTool = nullptr);
  Result askAudio(const uint8_t* wavBytes, size_t wavLen, const ToolNotifier& onTool = nullptr);
  void clearHistory();
  size_t historySize() const;
};
