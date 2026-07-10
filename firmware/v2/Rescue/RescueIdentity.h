#pragma once
// RescueIdentity.h — trimmed COPY of Master's DeviceIdentity.h for the
// rescue app (#195; repo convention: rescue shares no compiled code with
// Master — fix bugs in both while both trees are alive). Only the pieces
// rescue uses: name validation, chip-id fallback, and the "-rescue" AP
// suffix. Natively tested (test/test_rescue_identity).

#include <Arduino.h>

// 24 chars keeps the composed AP SSID inside the 32-byte 802.11 limit:
// 24 + strlen("-rescue") = 31.
#define DEVICE_NAME_MAX_LEN 24
#define AP_SUFFIX_RESCUE "-rescue"

static_assert(DEVICE_NAME_MAX_LEN + sizeof(AP_SUFFIX_RESCUE) - 1 <= 32,
              "rescue SSID would exceed 32 bytes");

// A valid name is label-safe for mDNS, DHCP hostname and SSID at once:
// [a-z0-9-], no leading/trailing hyphen, 1..24 chars. Empty is NOT valid —
// the empty slot means "use the chip-id default" (resolveDeviceName).
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
inline String defaultDeviceName(const char* prefix, uint32_t chipId) {
  return String(prefix) + "-" + String(chipId, HEX);
}

// Stored NVS name if readable and valid, else the chip-id default.
inline String resolveDeviceName(bool storeValid, const String& storedName,
                                const char* prefix, uint32_t chipId) {
  if (storeValid && isValidDeviceName(storedName)) {
    return storedName;
  }
  return defaultDeviceName(prefix, chipId);
}
