// Settings persistence layout on EEPROM.
//
// Shared header between ServiceSettingsFunctions.ino and the host-side
// test env. The including TU must already have the global `EEPROM`
// object (ESP8266 core provides it via <EEPROM.h>; the native test env
// provides a fake EEPROMClass instance).
//
// Layout (all strings are null-terminated, zero-padded to their slot):
//   offset size   field
//   0      4      MAGIC (0x5F1A70BE) - "has the blob been written"
//   4      1      VERSION (7) - bump + migrate on struct changes
//   5      4      CRC32 over the full payload [OFF_ALIGNMENT, end of blob),
//                 reserved regions included - v7 (#151). The covered range is
//                 deliberately version-independent (magic/version/CRC header
//                 excluded) so OLDER firmware can validate a NEWER blob's CRC
//                 on the downgrade path (#152). Every save*() recomputes it
//                 before its EEPROM.commit(); every future migration step
//                 must end with updateSettingsCrc(). NOTE: bytes 9..24
//                 (RESERVED_1) sit OUTSIDE the covered range — a future slot
//                 carved there would be CRC-unprotected; carve from
//                 RESERVED_2 instead.
//   9      16     RESERVED (remainder of former countdownToDateUnix, #26)
//   25     8      alignment ("left"/"center"/"right")
//   33     4      flapSpeed (decimal int)
//   37     20     deviceMode
//   57     40     timezonePosix (POSIX TZ string; empty -> UTC) - added in v2 (#48)
//   97     24     intendedVersion (GIT_REV of most-recently-uploaded firmware) - v3 (#52)
//   121    16     lastFlashResult ("" / "ok" / "reverted") - set on boot when an
//                 RTC-cookie flash attempt is resolved; exposed in /settings so
//                 remote flashers can spot a silent eboot revert - v4 (#53)
//   137    25     deviceName (complete network identity, 24 chars max + NUL;
//                 empty -> chip-id default "split-flap-<hex>") - v5 (#125)
//   162    65     mqttHost (broker hostname/IP; empty -> MQTT disabled) - v6 (#57)
//   227    6      mqttPort (decimal string 1..65535; empty -> 1883) - v6 (#57)
//   233    33     mqttUser (optional broker auth; empty -> anonymous) - v6 (#57)
//   266    65     mqttPassword (write-only via web UI, never echoed in
//                 /settings) - v6 (#57)
//   331    1701   RESERVED (formerly scheduledMessages JSON, removed with #38)
//   2032            end of blob
//
// Reserved slots keep existing EEPROM blobs (same SETTINGS_VERSION) valid —
// no migration required when features are removed. When a new slot is
// carved from a RESERVED region, bump SETTINGS_VERSION and handle the
// migration in initialiseSettings().

#pragma once

#include <Arduino.h>

#define SETTINGS_EEPROM_SIZE      2048
#define SETTINGS_MAGIC            0x5F1A70BEUL
#define SETTINGS_VERSION          7

#define OFF_MAGIC                 0
#define OFF_VERSION               4
#define OFF_SETTINGS_CRC          5
#define LEN_SETTINGS_CRC          4
#define OFF_RESERVED_1            9
#define LEN_RESERVED_1            16
#define OFF_ALIGNMENT             25
#define LEN_ALIGNMENT             8
#define OFF_FLAPSPEED             33
#define LEN_FLAPSPEED             4
#define OFF_DEVICEMODE            37
#define LEN_DEVICEMODE            20
#define OFF_TIMEZONE              57
#define LEN_TIMEZONE              40
#define OFF_INTENDED_VERSION      97
#define LEN_INTENDED_VERSION      24
#define OFF_LAST_FLASH_RESULT     121
#define LEN_LAST_FLASH_RESULT     16
#define OFF_DEVICE_NAME           137
#define LEN_DEVICE_NAME           25
#define OFF_MQTT_HOST             162
#define LEN_MQTT_HOST             65
#define OFF_MQTT_PORT             227
#define LEN_MQTT_PORT             6
#define OFF_MQTT_USER             233
#define LEN_MQTT_USER             33
#define OFF_MQTT_PASSWORD         266
#define LEN_MQTT_PASSWORD         65
#define OFF_RESERVED_2            331
#define LEN_RESERVED_2            1701

inline String readSettingString(int offset, int maxLen) {
  String out;
  out.reserve(maxLen);
  for (int i = 0; i < maxLen; i++) {
    char c = (char)EEPROM.read(offset + i);
    if (c == '\0') break;
    out += c;
  }
  return out;
}

inline void writeSettingString(int offset, int maxLen, const String& value) {
  int len = value.length();
  if (len >= maxLen) len = maxLen - 1;  // leave room for NUL
  for (int i = 0; i < len; i++) {
    EEPROM.write(offset + i, (uint8_t)value[i]);
  }
  for (int i = len; i < maxLen; i++) {
    EEPROM.write(offset + i, 0);
  }
}

inline uint32_t readSettingMagic() {
  uint32_t magic = 0;
  for (int i = 0; i < 4; i++) {
    magic |= ((uint32_t)EEPROM.read(OFF_MAGIC + i)) << (i * 8);
  }
  return magic;
}

inline void writeSettingMagic() {
  uint32_t magic = SETTINGS_MAGIC;
  for (int i = 0; i < 4; i++) {
    EEPROM.write(OFF_MAGIC + i, (uint8_t)((magic >> (i * 8)) & 0xFF));
  }
  EEPROM.write(OFF_VERSION, SETTINGS_VERSION);
}

// --- payload CRC (#151) ----------------------------------------------------

// CRC32 (reflected, poly 0xEDB88320 — the zlib/PNG polynomial) over the full
// settings payload. Bitwise, no lookup table: 2 KB × 8 shifts is microseconds
// on the ESP8266 and runs only at boot + on settings saves.
inline uint32_t computeSettingsCrc() {
  uint32_t crc = 0xFFFFFFFFUL;
  for (int i = OFF_ALIGNMENT; i < OFF_RESERVED_2 + LEN_RESERVED_2; i++) {
    crc ^= EEPROM.read(i);
    for (int b = 0; b < 8; b++) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
  }
  return ~crc;
}

inline uint32_t readSettingsCrc() {
  uint32_t crc = 0;
  for (int i = 0; i < LEN_SETTINGS_CRC; i++) {
    crc |= ((uint32_t)EEPROM.read(OFF_SETTINGS_CRC + i)) << (i * 8);
  }
  return crc;
}

inline void writeSettingsCrc(uint32_t crc) {
  for (int i = 0; i < LEN_SETTINGS_CRC; i++) {
    EEPROM.write(OFF_SETTINGS_CRC + i, (uint8_t)((crc >> (i * 8)) & 0xFF));
  }
}

// Recompute + store. Call after any payload write, before EEPROM.commit().
inline void updateSettingsCrc() {
  writeSettingsCrc(computeSettingsCrc());
}

// --- boot-time blob verdict (#151/#152) -------------------------------------

// What initialiseSettings() should do with the blob it found. Pure so the
// decision table is natively testable; the .ino turns the verdict into
// writes/logging.
enum SettingsBlobVerdict {
  SETTINGS_BLOB_BLANK,      // no/foreign magic -> initialise defaults
  SETTINGS_BLOB_OK,         // current version, CRC valid -> use as-is
  SETTINGS_BLOB_MIGRATE,    // older version, CRC INVALID -> a genuine pre-v7
                            // blob (its CRC slot is RESERVED_1 garbage);
                            // run the migration ladder
  SETTINGS_BLOB_ADOPT,      // wrong version but CRC VALID -> the payload is
                            // provably intact: an OTA revert left a newer
                            // blob (#152), or the version byte alone
                            // bit-rotted low (a genuine old blob matching
                            // the CRC is a 2^-32 coincidence). Preserve the
                            // slots we understand, pin the version.
  SETTINGS_BLOB_CORRUPT,    // current/newer version with CRC mismatch (torn
                            // commit, or a torn header masquerading as a
                            // newer version) -> defaults
};

inline SettingsBlobVerdict assessSettingsBlob(uint32_t magic, uint8_t ver,
                                              uint32_t storedCrc, uint32_t computedCrc) {
  if (magic != SETTINGS_MAGIC)      return SETTINGS_BLOB_BLANK;
  if (storedCrc == computedCrc) {
    return (ver == SETTINGS_VERSION) ? SETTINGS_BLOB_OK : SETTINGS_BLOB_ADOPT;
  }
  if (ver < SETTINGS_VERSION)       return SETTINGS_BLOB_MIGRATE;
  return SETTINGS_BLOB_CORRUPT;
}
