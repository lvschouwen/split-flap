// Settings persistence on EEPROM (no filesystem).
//
// The ESP-01 has 1 MB of flash, all of which is given over to the sketch +
// OTA staging area. Settings that used to live in LittleFS files now sit
// in the 4 KB EEPROM region (0x402FB000) reserved by the ESP8266 core.
//
// Layout (all strings are null-terminated, zero-padded to their slot):
//   offset size   field
//   0      4      MAGIC (0x5F1A70BE) — "has the blob been written"
//   4      1      VERSION (1) — bump + migrate on struct changes
//   5      20     countdownToDateUnix
//   25     8      alignment ("left"/"center"/"right")
//   33     4      flapSpeed (decimal int)
//   37     20     deviceMode
//   57     1975   scheduledMessages JSON
//   2032            end of blob
//
// Commits happen whenever a String is written back. EEPROM wear isn't a
// concern — settings change maybe dozens of times a day at most.

#include <EEPROM.h>

#define SETTINGS_EEPROM_SIZE      2048
#define SETTINGS_MAGIC            0x5F1A70BEUL
#define SETTINGS_VERSION          1

#define OFF_MAGIC                 0
#define OFF_VERSION               4
#define OFF_COUNTDOWN             5
#define LEN_COUNTDOWN             20
#define OFF_ALIGNMENT             25
#define LEN_ALIGNMENT             8
#define OFF_FLAPSPEED             33
#define LEN_FLAPSPEED             4
#define OFF_DEVICEMODE            37
#define LEN_DEVICEMODE            20
#define OFF_SCHEDMSGS             57
#define LEN_SCHEDMSGS             1975

static String readSettingString(int offset, int maxLen) {
  String out;
  out.reserve(maxLen);
  for (int i = 0; i < maxLen; i++) {
    char c = (char)EEPROM.read(offset + i);
    if (c == '\0') break;
    out += c;
  }
  return out;
}

static void writeSettingString(int offset, int maxLen, const String& value) {
  int len = value.length();
  if (len >= maxLen) len = maxLen - 1;  // leave room for NUL
  for (int i = 0; i < len; i++) {
    EEPROM.write(offset + i, (uint8_t)value[i]);
  }
  for (int i = len; i < maxLen; i++) {
    EEPROM.write(offset + i, 0);
  }
}

static uint32_t readSettingMagic() {
  uint32_t magic = 0;
  for (int i = 0; i < 4; i++) {
    magic |= ((uint32_t)EEPROM.read(OFF_MAGIC + i)) << (i * 8);
  }
  return magic;
}

static void writeSettingMagic() {
  uint32_t magic = SETTINGS_MAGIC;
  for (int i = 0; i < 4; i++) {
    EEPROM.write(OFF_MAGIC + i, (uint8_t)((magic >> (i * 8)) & 0xFF));
  }
  EEPROM.write(OFF_VERSION, SETTINGS_VERSION);
}

void initialiseFileSystem() {
  EEPROM.begin(SETTINGS_EEPROM_SIZE);

  if (readSettingMagic() != SETTINGS_MAGIC || EEPROM.read(OFF_VERSION) != SETTINGS_VERSION) {
    SerialPrintln("Settings EEPROM blank/stale — initialising with defaults");
    writeSettingString(OFF_COUNTDOWN,  LEN_COUNTDOWN,  "0");
    writeSettingString(OFF_ALIGNMENT,  LEN_ALIGNMENT,  ALIGNMENT_MODE_LEFT);
    writeSettingString(OFF_FLAPSPEED,  LEN_FLAPSPEED,  "80");
    writeSettingString(OFF_DEVICEMODE, LEN_DEVICEMODE, DEVICE_MODE_TEXT);
    writeSettingString(OFF_SCHEDMSGS,  LEN_SCHEDMSGS,  "[]");
    writeSettingMagic();
    EEPROM.commit();
  }

  SerialPrintln("Settings EEPROM ready");
}

void loadValuesFromFileSystem() {
  countdownToDateUnix = readSettingString(OFF_COUNTDOWN,  LEN_COUNTDOWN);
  alignment           = readSettingString(OFF_ALIGNMENT,  LEN_ALIGNMENT);
  flapSpeed           = readSettingString(OFF_FLAPSPEED,  LEN_FLAPSPEED);
  deviceMode          = readSettingString(OFF_DEVICEMODE, LEN_DEVICEMODE);

  String scheduledMessagesJson = readSettingString(OFF_SCHEDMSGS, LEN_SCHEDMSGS);
  if (scheduledMessagesJson.length() > 0 && scheduledMessagesJson != "[]") {
    SerialPrintln("Loading Scheduled Messages");
    readScheduledMessagesFromJson(scheduledMessagesJson);
  }

  SerialPrintln("Loaded Settings:");
  SerialPrintln("   Alignment: " + alignment);
  SerialPrintln("   Flap Speed: " + flapSpeed);
  SerialPrintln("   Device Mode: " + deviceMode);
  SerialPrintln("   Countdown to Date UNIX: " + countdownToDateUnix);
  SerialPrint("   Scheduled Message Count: ");
  SerialPrintln(scheduledMessages.size());
}

// Called from the web server handlers whenever a setting changes.
void saveCountdown()    { writeSettingString(OFF_COUNTDOWN,  LEN_COUNTDOWN,  countdownToDateUnix); EEPROM.commit(); }
void saveAlignment()    { writeSettingString(OFF_ALIGNMENT,  LEN_ALIGNMENT,  alignment);           EEPROM.commit(); }
void saveFlapSpeed()    { writeSettingString(OFF_FLAPSPEED,  LEN_FLAPSPEED,  flapSpeed);           EEPROM.commit(); }
void saveDeviceMode()   { writeSettingString(OFF_DEVICEMODE, LEN_DEVICEMODE, deviceMode);          EEPROM.commit(); }
void saveScheduledMessagesJson(const String& json) {
  writeSettingString(OFF_SCHEDMSGS, LEN_SCHEDMSGS, json);
  EEPROM.commit();
}
