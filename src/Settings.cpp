#include "Settings.h"

#include <Preferences.h>

#include "config.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef SECRET_WIFI_SSID
#define SECRET_WIFI_SSID ""
#endif
#ifndef SECRET_WIFI_PASS
#define SECRET_WIFI_PASS ""
#endif
#ifndef SECRET_GEMINI_KEY
#define SECRET_GEMINI_KEY ""
#endif

static const char* kNamespace = "assist";

void Settings::load() {
  Preferences p;
  p.begin(kNamespace, false);
  ssid   = p.getString("ssid", SECRET_WIFI_SSID);
  pass   = p.getString("pass", SECRET_WIFI_PASS);
  apiKey = p.getString("key", SECRET_GEMINI_KEY);
  model  = p.getString("model", DEFAULT_MODEL);
  p.end();
  if (model.isEmpty()) model = DEFAULT_MODEL;
}

void Settings::save() const {
  Preferences p;
  p.begin(kNamespace, false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.putString("key", apiKey);
  p.putString("model", model);
  p.end();
}
