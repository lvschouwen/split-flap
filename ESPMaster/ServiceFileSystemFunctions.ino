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

  uint32_t magic = readSettingMagic();
  uint8_t  ver   = EEPROM.read(OFF_VERSION);

  if (magic != SETTINGS_MAGIC || ver > SETTINGS_VERSION) {
    SerialPrintln(F("Settings EEPROM blank/stale — initialising with defaults"));
    writeSettingString(OFF_ALIGNMENT,        LEN_ALIGNMENT,        ALIGNMENT_MODE_LEFT);
    writeSettingString(OFF_FLAPSPEED,        LEN_FLAPSPEED,        "80");
    writeSettingString(OFF_DEVICEMODE,       LEN_DEVICEMODE,       DEVICE_MODE_TEXT);
    writeSettingString(OFF_TIMEZONE,         LEN_TIMEZONE,         "");
    writeSettingString(OFF_INTENDED_VERSION, LEN_INTENDED_VERSION, "");
    writeSettingMagic();
    EEPROM.commit();
  } else if (ver < SETTINGS_VERSION) {
    //Migrate in place, preserving alignment/flapSpeed/deviceMode.
    SerialPrint(F("Settings EEPROM migrating v"));
    SerialPrint(ver);
    SerialPrint(F(" -> v"));
    SerialPrintln(SETTINGS_VERSION);
    //v1 -> v2: OFF_TIMEZONE carved from former RESERVED_2. Zero the slot
    //so readSettingString() returns empty (-> UTC fallback) instead of
    //whatever garbage was left there by earlier firmwares (#48).
    if (ver < 2) writeSettingString(OFF_TIMEZONE, LEN_TIMEZONE, "");
    //v2 -> v3: OFF_INTENDED_VERSION carved from former RESERVED_2. Zero
    //the slot so the first post-migration boot compares against an empty
    //string (-> "no intended version recorded yet", not an OTA revert
    //false-positive against whatever garbage was in RESERVED_2). See #52.
    if (ver < 3) writeSettingString(OFF_INTENDED_VERSION, LEN_INTENDED_VERSION, "");
    EEPROM.write(OFF_VERSION, SETTINGS_VERSION);
    EEPROM.commit();
  }

  SerialPrintln(F("Settings EEPROM ready"));
}

void loadValuesFromFileSystem() {
  alignment            = readSettingString(OFF_ALIGNMENT,  LEN_ALIGNMENT);
  flapSpeed            = readSettingString(OFF_FLAPSPEED,  LEN_FLAPSPEED);
  deviceMode           = readSettingString(OFF_DEVICEMODE, LEN_DEVICEMODE);
  timezonePosixSetting = readSettingString(OFF_TIMEZONE,   LEN_TIMEZONE);

  SerialPrintln(F("Loaded Settings:"));
  SerialPrintln("   Alignment: " + alignment);
  SerialPrintln("   Flap Speed: " + flapSpeed);
  SerialPrintln("   Device Mode: " + deviceMode);
  SerialPrintln("   Timezone: " + (timezonePosixSetting.length() ? timezonePosixSetting : String("(default)")));
}

// Called from the web server handlers whenever a setting changes.
void saveAlignment()    { writeSettingString(OFF_ALIGNMENT,  LEN_ALIGNMENT,  alignment);            EEPROM.commit(); }
void saveFlapSpeed()    { writeSettingString(OFF_FLAPSPEED,  LEN_FLAPSPEED,  flapSpeed);            EEPROM.commit(); }
void saveDeviceMode()   { writeSettingString(OFF_DEVICEMODE, LEN_DEVICEMODE, deviceMode);           EEPROM.commit(); }
void saveTimezone()     { writeSettingString(OFF_TIMEZONE,   LEN_TIMEZONE,   timezonePosixSetting); EEPROM.commit(); }

// Persist the caller-supplied intended version at the start of a master OTA
// upload. Read back on the next boot to detect a silent revert (image
// rejected by eboot or crashed fast enough to trip recovery). See #52.
void saveIntendedVersion(const String& v) {
  writeSettingString(OFF_INTENDED_VERSION, LEN_INTENDED_VERSION, v);
  EEPROM.commit();
}
String readIntendedVersion() {
  return readSettingString(OFF_INTENDED_VERSION, LEN_INTENDED_VERSION);
}

//Resolve effective POSIX TZ: runtime EEPROM setting wins, else the
//compile-time `timezonePosix` const, else "UTC0". Called at boot and
//whenever the web UI updates the timezone so the clock picks up the
//change without a reboot. Issue #48.
void applyTimezoneAndNtp() {
  const char* tz;
  if (timezonePosixSetting.length() > 0) {
    tz = timezonePosixSetting.c_str();
  } else if (strlen(timezonePosix) > 0) {
    tz = timezonePosix;
  } else {
    tz = "UTC0";
  }
  const char* ntp = (strlen(timezoneServer) > 0) ? timezoneServer : "pool.ntp.org";
  configTime(tz, ntp);

  SerialPrint(F("NTP sync (tz="));
  SerialPrint(tz);
  SerialPrint(F(", server="));
  SerialPrint(ntp);
  SerialPrintln(F(")"));
}
