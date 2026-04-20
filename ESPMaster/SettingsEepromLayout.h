// Settings persistence layout on EEPROM.
//
// Shared header between ServiceFileSystemFunctions.ino and the host-side
// test env. The including TU must already have the global `EEPROM`
// object (ESP8266 core provides it via <EEPROM.h>; the native test env
// provides a fake EEPROMClass instance).
//
// Layout (all strings are null-terminated, zero-padded to their slot):
//   offset size   field
//   0      4      MAGIC (0x5F1A70BE) - "has the blob been written"
//   4      1      VERSION (1) - bump + migrate on struct changes
//   5      20     RESERVED (formerly countdownToDateUnix, removed with #26)
//   25     8      alignment ("left"/"center"/"right")
//   33     4      flapSpeed (decimal int)
//   37     20     deviceMode
//   57     1975   scheduledMessages JSON
//   2032            end of blob

#pragma once

#include <Arduino.h>

#define SETTINGS_EEPROM_SIZE      2048
#define SETTINGS_MAGIC            0x5F1A70BEUL
#define SETTINGS_VERSION          1

#define OFF_MAGIC                 0
#define OFF_VERSION               4
// Formerly countdownToDateUnix; slot kept reserved so existing EEPROM blobs
// (same SETTINGS_VERSION) keep their alignment/flapSpeed/deviceMode/sched
// messages valid across the #26 upgrade.
#define OFF_RESERVED_1            5
#define LEN_RESERVED_1            20
#define OFF_ALIGNMENT             25
#define LEN_ALIGNMENT             8
#define OFF_FLAPSPEED             33
#define LEN_FLAPSPEED             4
#define OFF_DEVICEMODE            37
#define LEN_DEVICEMODE            20
#define OFF_SCHEDMSGS             57
#define LEN_SCHEDMSGS             1975

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
