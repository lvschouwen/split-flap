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
    writeSettingString(OFF_ALIGNMENT,  LEN_ALIGNMENT,  ALIGNMENT_MODE_LEFT);
    writeSettingString(OFF_FLAPSPEED,  LEN_FLAPSPEED,  "80");
    writeSettingString(OFF_DEVICEMODE, LEN_DEVICEMODE, DEVICE_MODE_TEXT);
    writeSettingString(OFF_TIMEZONE,   LEN_TIMEZONE,   "");
    writeSettingString(OFF_MQTT_HOST,  LEN_MQTT_HOST,  "");
    writeSettingString(OFF_MQTT_PORT,  LEN_MQTT_PORT,  "1883");
    writeSettingString(OFF_MQTT_USER,  LEN_MQTT_USER,  "");
    writeSettingString(OFF_MQTT_PASS,  LEN_MQTT_PASS,  "");
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
    //v2 -> v3: MQTT slots carved from former RESERVED_2. Zero them so
    //empty host reads as "" (-> MQTT disabled), and seed the default port
    //so users who enable MQTT don't have to remember 1883 (#50).
    if (ver < 3) {
      writeSettingString(OFF_MQTT_HOST, LEN_MQTT_HOST, "");
      writeSettingString(OFF_MQTT_PORT, LEN_MQTT_PORT, "1883");
      writeSettingString(OFF_MQTT_USER, LEN_MQTT_USER, "");
      writeSettingString(OFF_MQTT_PASS, LEN_MQTT_PASS, "");
    }
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
  mqttHostSetting      = readSettingString(OFF_MQTT_HOST,  LEN_MQTT_HOST);
  mqttPortSetting      = readSettingString(OFF_MQTT_PORT,  LEN_MQTT_PORT);
  mqttUserSetting      = readSettingString(OFF_MQTT_USER,  LEN_MQTT_USER);
  mqttPassSetting      = readSettingString(OFF_MQTT_PASS,  LEN_MQTT_PASS);

  SerialPrintln(F("Loaded Settings:"));
  SerialPrintln("   Alignment: " + alignment);
  SerialPrintln("   Flap Speed: " + flapSpeed);
  SerialPrintln("   Device Mode: " + deviceMode);
  SerialPrintln("   Timezone: " + (timezonePosixSetting.length() ? timezonePosixSetting : String("(default)")));
  //Don't log mqttPass — the log (eventually streamed to MQTT) must not echo
  //the broker password back out. Host/port/user are fine.
  SerialPrintln("   MQTT: " + (mqttHostSetting.length()
    ? (mqttHostSetting + ":" + mqttPortSetting + (mqttUserSetting.length() ? String(" user=") + mqttUserSetting : String("")))
    : String("(disabled)")));
}

// Called from the web server handlers whenever a setting changes.
void saveAlignment()    { writeSettingString(OFF_ALIGNMENT,  LEN_ALIGNMENT,  alignment);            EEPROM.commit(); }
void saveFlapSpeed()    { writeSettingString(OFF_FLAPSPEED,  LEN_FLAPSPEED,  flapSpeed);            EEPROM.commit(); }
void saveDeviceMode()   { writeSettingString(OFF_DEVICEMODE, LEN_DEVICEMODE, deviceMode);           EEPROM.commit(); }
void saveTimezone()     { writeSettingString(OFF_TIMEZONE,   LEN_TIMEZONE,   timezonePosixSetting); EEPROM.commit(); }
//All four MQTT slots commit together — avoids a partially-applied config
//(e.g. new host with stale password) if the ESP browns out mid-save.
void saveMqttConfig() {
  writeSettingString(OFF_MQTT_HOST, LEN_MQTT_HOST, mqttHostSetting);
  writeSettingString(OFF_MQTT_PORT, LEN_MQTT_PORT, mqttPortSetting);
  writeSettingString(OFF_MQTT_USER, LEN_MQTT_USER, mqttUserSetting);
  writeSettingString(OFF_MQTT_PASS, LEN_MQTT_PASS, mqttPassSetting);
  EEPROM.commit();
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
