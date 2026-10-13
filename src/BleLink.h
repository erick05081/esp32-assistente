#pragma once
#include <Arduino.h>

#include <functional>
#include <string>

// Link BLE com o celular usando o Nordic UART Service (NUS).
// Protocolo: mensagens JSON, uma por linha ('\n'). Linhas longas são
// fragmentadas automaticamente de acordo com o MTU negociado.
class BleLink {
 public:
  using LineHandler = std::function<void(std::string&& line)>;

  void begin(const char* deviceName, LineHandler onLine);
  bool send(const std::string& line);
  bool isConnected() const;
};

extern BleLink Ble;
