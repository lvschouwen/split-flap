#pragma once
// NvsRead.h — reading a string that may not be stored yet. Preferences logs
// a missing key as an error, and on a new board every key is missing: ask
// first. Target-only (ArduinoFake has no Preferences).

#include <Preferences.h>

inline String nvsReadString(Preferences& prefs, const char* key,
                            const String& fallback = String()) {
  return prefs.isKey(key) ? prefs.getString(key, fallback) : fallback;
}
