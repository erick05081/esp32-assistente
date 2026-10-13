#pragma once
#include <Arduino.h>

// Configurações persistentes (NVS). Valores padrão vêm de secrets.h / config.h
// e podem ser alterados em tempo de execução pelo app do celular.
struct Settings {
  String ssid;
  String pass;
  String apiKey;
  String model;

  void load();
  void save() const;
};
