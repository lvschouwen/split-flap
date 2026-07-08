#pragma once
// SettingsValidation.h — pure per-field validators for the POST / settings
// handler (#153). No globals, no Arduino calls beyond String, so the whole
// file runs natively (test/test_settings_validation). The handler in
// ServiceWebEndpoints.ino is a thin adapter: it trims/normalizes, calls
// these, and stages accepted values. EEPROM slot lengths (LEN_*, from
// SettingsEepromLayout.h) are passed in by the caller so this header stays
// layout-independent. Device-name validation is NOT here — it predates this
// header and lives in DeviceIdentity.h (isValidDeviceName).

#include <Arduino.h>
#include <stdlib.h>

// Mirrors HelpersStringHandling.ino's isNumber() exactly (strtol-based, so
// leading whitespace/sign parse like they always did — the extraction must
// not tighten what a raw POST could previously persist). Local copy because
// this header must not depend on a .ino.
static inline bool settingsIsNumber(const String& str) {
  if (str.length() == 0) {
    return false;
  }
  char* endPtr;
  strtol(str.c_str(), &endPtr, 10);
  return *endPtr == '\0';
}

// Shared character check: printable ASCII, with a caller-chosen floor —
// 0x20 ("space allowed": timezone, password) or 0x21 ("no spaces": MQTT
// host/user, which are also trimmed by the caller first).
static inline bool settingsIsPrintableAscii(const String& v, char lowest) {
  for (unsigned int i = 0; i < v.length(); i++) {
    char c = v[i];
    if (c < lowest || c > 0x7E) return false;
  }
  return true;
}

// "left" / "center" / "right" — must stay equal to the ALIGNMENT_MODE_*
// wire values in ESPMaster.ino (frozen /settings API strings).
static inline bool isValidAlignmentValue(const String& v) {
  return v == "left" || v == "center" || v == "right";
}

// "text" / "clock" — must stay equal to DEVICE_MODE_* in ESPMaster.ino.
static inline bool isValidDeviceModeValue(const String& v) {
  return v == "text" || v == "clock";
}

// The HTML slider enforces 1..100 client-side only; this is the server-side
// bound so a raw POST can't persist an out-of-range speed (#95).
static inline bool isValidFlapSpeedValue(const String& v) {
  return settingsIsNumber(v) && v.toInt() >= 1 && v.toInt() <= 100;
}

// POSIX TZ string (#48). Empty = UTC fallback. Printable ASCII (spaces OK)
// and must fit the EEPROM slot — reject rather than silently truncate.
static inline bool isValidTimezoneValue(const String& v, int slotLen) {
  return (int)v.length() < slotLen && settingsIsPrintableAscii(v, 0x20);
}

// MQTT host/user (#57): printable ASCII without spaces, must fit the slot.
// Empty is valid (empty host = MQTT disabled). Caller trims first.
static inline bool isValidMqttHostOrUserValue(const String& trimmed, int slotLen) {
  return (int)trimmed.length() < slotLen && settingsIsPrintableAscii(trimmed, 0x21);
}

// MQTT port (#57): empty = default 1883, else 1..65535. The length bound is
// load-bearing too: "0001883" parses to 1883 but would truncate to "00018"
// in the 6-byte slot — reject rather than corrupt.
static inline bool isValidMqttPortValue(const String& trimmed, int slotLen) {
  if (trimmed.length() == 0) return true;
  return (int)trimmed.length() < slotLen &&
         settingsIsNumber(trimmed) &&
         trimmed.toInt() >= 1 && trimmed.toInt() <= 65535;
}

// MQTT password (#57): printable ASCII (spaces OK), must fit the slot.
// Write-only semantics live in the caller: an EMPTY submission means "keep
// the stored password" and is never routed here.
static inline bool isValidMqttPasswordValue(const String& v, int slotLen) {
  return (int)v.length() < slotLen && settingsIsPrintableAscii(v, 0x20);
}
