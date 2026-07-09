#pragma once
// SettingsStore.h — minimal storage seam under the settings layer (#185).
//
// The real store is NVS via Preferences (NvsSettingsStore.h), which
// ArduinoFake cannot mock — so everything above this interface (defaults,
// sanitize-on-load, write-only password) is pure logic tested natively
// against an in-memory fake, and the Preferences wrapper below it stays
// thin enough to verify on hardware by inspection.

#include <Arduino.h>

class SettingsStore {
 public:
  virtual ~SettingsStore() {}
  virtual String getString(const char* key, const String& def) = 0;
  virtual void putString(const char* key, const String& value) = 0;
  virtual int getInt(const char* key, int def) = 0;
  virtual void putInt(const char* key, int value) = 0;
};
