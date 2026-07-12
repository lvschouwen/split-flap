// Per-device network identity (#125).
//
// Pure logic only — no EEPROM or WiFi access — so it is exercised by the
// host-side native tests (test/test_device_identity/). The boot-time
// resolver in ESPMaster.ino reads the EEPROM slot and feeds it through
// resolveDeviceName(); every network-facing consumer (mDNS, hostname,
// MQTT client id / topics, captive-portal AP) then reads the single
// resolved global.
//
// The device name is the COMPLETE identity (not a suffix): "kitchen"
// yields kitchen.local, hostname kitchen, MQTT id kitchen, setup AP
// "kitchen-setup". Empty/invalid/unreadable EEPROM falls back to
// "<prefix>-<hex chip id>" which is unique per device and always valid.

#pragma once

#include <Arduino.h>

// 24 chars keeps every composed AP SSID inside the 32-byte 802.11 limit
// given the suffixes composed from the name: "-setup" here (24 + 6 = 30)
// and the rescue app's "-rescue" (24 + 7 = 31, asserted in its own tree —
// RescueIdentity.h keeps its own copy of this limit).
#define DEVICE_NAME_MAX_LEN 24

#define AP_SUFFIX_SETUP "-setup"

static_assert(DEVICE_NAME_MAX_LEN + sizeof(AP_SUFFIX_SETUP) - 1 <= 32, "captive-portal SSID would exceed 32 bytes");

// Lowercase a candidate name. Callers normalize before validating so
// "Kitchen" just works from the web UI.
inline String normalizeDeviceName(const String& raw) {
  String out = raw;
  for (unsigned int i = 0; i < out.length(); i++) {
    char c = out[i];
    if (c >= 'A' && c <= 'Z') out.setCharAt(i, c + ('a' - 'A'));
  }
  return out;
}

// A valid name is label-safe for mDNS, DHCP hostname, MQTT topic segment
// and SSID all at once: [a-z0-9-], no leading/trailing hyphen, 1..24 chars.
// Empty is NOT valid here — the empty slot means "use the chip-id default"
// and resolveDeviceName() handles that fallthrough.
inline bool isValidDeviceName(const String& name) {
  unsigned int len = name.length();
  if (len < 1 || len > DEVICE_NAME_MAX_LEN) return false;
  if (name[0] == '-' || name[len - 1] == '-') return false;
  for (unsigned int i = 0; i < len; i++) {
    char c = name[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    if (!ok) return false;
  }
  return true;
}

// Factory default: "<prefix>-<hex chip id>", e.g. "split-flap-9a3c1f".
// "split-flap-" (11) + at most 8 hex chars = 19 <= DEVICE_NAME_MAX_LEN.
inline String defaultDeviceName(const char* prefix, uint32_t chipId) {
  return String(prefix) + "-" + String(chipId, HEX);
}

// Resolution order: stored EEPROM name if the settings blob was readable
// (magic/version verified by the caller) and the slot holds a valid name;
// otherwise the chip-id default. eepromValid=false covers the recovery-
// mode path where the SoftAP comes up before initialiseSettings().
inline String resolveDeviceName(bool eepromValid, const String& storedName,
                                const char* prefix, uint32_t chipId) {
  if (eepromValid && isValidDeviceName(storedName)) {
    return storedName;
  }
  return defaultDeviceName(prefix, chipId);
}
