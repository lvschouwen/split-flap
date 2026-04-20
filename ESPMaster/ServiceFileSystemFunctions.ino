// Settings persistence on EEPROM (no filesystem).
//
// The ESP-01 has 1 MB of flash, all of which is given over to the sketch +
// OTA staging area. Settings that used to live in LittleFS files now sit
// in the 4 KB EEPROM region (0x402FB000) reserved by the ESP8266 core.
// Layout + helpers live in SettingsEepromLayout.h so they're exercised
// by the host-side unit tests against a fake EEPROMClass.
//
// Commits happen whenever a String is written back. EEPROM wear isn't a
// concern — settings change maybe dozens of times a day at most.

#include <EEPROM.h>
#include "SettingsEepromLayout.h"

void initialiseFileSystem() {
  EEPROM.begin(SETTINGS_EEPROM_SIZE);

  if (readSettingMagic() != SETTINGS_MAGIC || EEPROM.read(OFF_VERSION) != SETTINGS_VERSION) {
    SerialPrintln("Settings EEPROM blank/stale — initialising with defaults");
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
  SerialPrint("   Scheduled Message Count: ");
  SerialPrintln(scheduledMessages.size());
}

// Called from the web server handlers whenever a setting changes.
void saveAlignment()    { writeSettingString(OFF_ALIGNMENT,  LEN_ALIGNMENT,  alignment);           EEPROM.commit(); }
void saveFlapSpeed()    { writeSettingString(OFF_FLAPSPEED,  LEN_FLAPSPEED,  flapSpeed);           EEPROM.commit(); }
void saveDeviceMode()   { writeSettingString(OFF_DEVICEMODE, LEN_DEVICEMODE, deviceMode);          EEPROM.commit(); }
void saveScheduledMessagesJson(const String& json) {
  writeSettingString(OFF_SCHEDMSGS, LEN_SCHEDMSGS, json);
  EEPROM.commit();
}
