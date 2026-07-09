#pragma once
// NvsSettingsStore.h — the on-target SettingsStore over ESP32 NVS (#185).
//
// Deliberately a passthrough: all policy (defaults, sanitation, write-only
// password) lives in Settings.h where the native tests can reach it. Only
// included from target code — ArduinoFake has no Preferences, so this file
// must never be pulled into the native env.

#include <Preferences.h>

#include "SettingsStore.h"

class NvsSettingsStore : public SettingsStore {
 public:
  // read_only=false also creates the namespace on first boot.
  void begin() { prefs_.begin("splitflap", false); }

  String getString(const char* key, const String& def) override {
    return prefs_.getString(key, def);
  }
  void putString(const char* key, const String& value) override {
    prefs_.putString(key, value);
  }
  int getInt(const char* key, int def) override {
    return prefs_.getInt(key, def);
  }
  void putInt(const char* key, int value) override { prefs_.putInt(key, value); }

 private:
  Preferences prefs_;
};
