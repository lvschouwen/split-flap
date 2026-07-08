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

//Full re-initialisation — the blank-EEPROM path, also taken when the CRC
//says the blob is torn (#151). Every slot gets a sane default.
void resetSettingsToDefaults() {
  writeSettingString(OFF_ALIGNMENT,         LEN_ALIGNMENT,         ALIGNMENT_MODE_LEFT);
  writeSettingString(OFF_FLAPSPEED,         LEN_FLAPSPEED,         "80");
  writeSettingString(OFF_DEVICEMODE,        LEN_DEVICEMODE,        DEVICE_MODE_TEXT);
  //Default TZ to CE(S)T so a wipe+reflash yields a correctly-clocked
  //device without needing a web-UI round-trip. Falls through to the
  //compile-time `timezonePosix` const if this slot is ever empty (#53).
  writeSettingString(OFF_TIMEZONE,          LEN_TIMEZONE,          "CET-1CEST,M3.5.0,M10.5.0/3");
  writeSettingString(OFF_INTENDED_VERSION,  LEN_INTENDED_VERSION,  "");
  writeSettingString(OFF_LAST_FLASH_RESULT, LEN_LAST_FLASH_RESULT, "");
  writeSettingString(OFF_DEVICE_NAME,       LEN_DEVICE_NAME,       "");
  writeSettingString(OFF_MQTT_HOST,         LEN_MQTT_HOST,         "");
  writeSettingString(OFF_MQTT_PORT,         LEN_MQTT_PORT,         "");
  writeSettingString(OFF_MQTT_USER,         LEN_MQTT_USER,         "");
  writeSettingString(OFF_MQTT_PASSWORD,     LEN_MQTT_PASSWORD,     "");
  updateSettingsCrc();
  writeSettingMagic();
  EEPROM.commit();
}

void initialiseSettings() {
  EEPROM.begin(SETTINGS_EEPROM_SIZE);

  uint32_t magic = readSettingMagic();
  uint8_t  ver   = EEPROM.read(OFF_VERSION);

  SettingsBlobVerdict verdict = assessSettingsBlob(magic, ver, readSettingsCrc(), computeSettingsCrc());

  if (verdict == SETTINGS_BLOB_BLANK) {
    SerialPrintln(F("Settings EEPROM blank — initialising with defaults"));
    resetSettingsToDefaults();
  } else if (verdict == SETTINGS_BLOB_CORRUPT) {
    //Torn commit (#151): magic intact but the payload doesn't match its
    //CRC — most likely a power sag mid-EEPROM.commit(). Garbage settings
    //must not be trusted; fall back to defaults, loudly.
    SerialPrintln(F("Settings EEPROM CRC MISMATCH (torn write?) — re-initialising with defaults"));
    resetSettingsToDefaults();
  } else if (verdict == SETTINGS_BLOB_ADOPT) {
    //Version byte disagrees but the CRC proves the payload intact: an OTA
    //revert left a newer blob (#152), or the version byte alone bit-rotted.
    //Versions only carve slots out of RESERVED_2 (layout policy: existing
    //offsets are frozen) and the CRC range is version-independent, so every
    //slot we understand is valid. Adopt the blob instead of wiping it; just
    //pin the version so the migration ladder stays coherent.
    SerialPrint(F("Settings EEPROM version v"));
    SerialPrint(ver);
    SerialPrint(F(" with valid CRC — preserving known slots, pinning to v"));
    SerialPrintln(SETTINGS_VERSION);
    EEPROM.write(OFF_VERSION, SETTINGS_VERSION);
    EEPROM.commit();
  } else if (verdict == SETTINGS_BLOB_MIGRATE) {
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
    //v3 -> v4: OFF_LAST_FLASH_RESULT carved from former RESERVED_2. Zero
    //so a fresh post-migration /settings reports lastFlashResult="" (no
    //prior attempt recorded) rather than garbage. See #53.
    if (ver < 4) writeSettingString(OFF_LAST_FLASH_RESULT, LEN_LAST_FLASH_RESULT, "");
    //v4 -> v5: OFF_DEVICE_NAME carved from former RESERVED_2. Zero so a
    //migrated device reads an empty name (-> chip-id default identity),
    //not RESERVED_2 leftovers. See #125.
    if (ver < 5) writeSettingString(OFF_DEVICE_NAME, LEN_DEVICE_NAME, "");
    //v5 -> v6: MQTT broker config carved from former RESERVED_2 (#57).
    //Zero all four slots so a migrated device reads an empty host (-> MQTT
    //stays disabled, matching the pre-#57 compiled-out default), not
    //RESERVED_2 leftovers.
    if (ver < 6) {
      writeSettingString(OFF_MQTT_HOST,     LEN_MQTT_HOST,     "");
      writeSettingString(OFF_MQTT_PORT,     LEN_MQTT_PORT,     "");
      writeSettingString(OFF_MQTT_USER,     LEN_MQTT_USER,     "");
      writeSettingString(OFF_MQTT_PASSWORD, LEN_MQTT_PASSWORD, "");
    }
    //v6 -> v7: CRC32 carved from RESERVED_1 (#151). No per-slot zeroing —
    //the updateSettingsCrc() below stamps the slot with a valid checksum.
    //Future steps: keep updateSettingsCrc() as the LAST write before the
    //version byte so a migrated blob always validates on the next boot.
    updateSettingsCrc();
    EEPROM.write(OFF_VERSION, SETTINGS_VERSION);
    EEPROM.commit();
  }

  SerialPrintln(F("Settings EEPROM ready"));
}

void loadSettings() {
  alignment            = readSettingString(OFF_ALIGNMENT,   LEN_ALIGNMENT);
  flapSpeed            = readSettingString(OFF_FLAPSPEED,   LEN_FLAPSPEED);
  deviceMode           = readSettingString(OFF_DEVICEMODE,  LEN_DEVICEMODE);
  timezonePosixSetting = readSettingString(OFF_TIMEZONE,    LEN_TIMEZONE);
  //Raw slot value for the web UI / rename detection. The EFFECTIVE identity
  //was already resolved by resolveDeviceIdentity() at the top of setup().
  deviceNameSetting    = readSettingString(OFF_DEVICE_NAME, LEN_DEVICE_NAME);
  //MQTT broker config (#57). Empty host = MQTT disabled. Applied by
  //initMqtt() at boot; web-UI changes require a reboot to take effect.
  mqttHostSetting      = readSettingString(OFF_MQTT_HOST,     LEN_MQTT_HOST);
  mqttPortSetting      = readSettingString(OFF_MQTT_PORT,     LEN_MQTT_PORT);
  mqttUserSetting      = readSettingString(OFF_MQTT_USER,     LEN_MQTT_USER);
  mqttPasswordSetting  = readSettingString(OFF_MQTT_PASSWORD, LEN_MQTT_PASSWORD);

  SerialPrintln(F("Loaded Settings:"));
  SerialPrintln("   Alignment: " + alignment);
  SerialPrintln("   Flap Speed: " + flapSpeed);
  SerialPrintln("   Device Mode: " + deviceMode);
  SerialPrintln("   Timezone: " + (timezonePosixSetting.length() ? timezonePosixSetting : String("(default)")));
  SerialPrintln("   Device Name: " + (deviceNameSetting.length() ? deviceNameSetting : String("(chip-id default)")));
  //Never log the password.
  SerialPrintln("   MQTT Broker: " + (mqttHostSetting.length() ? mqttHostSetting + ":" + (mqttPortSetting.length() ? mqttPortSetting : String("1883")) : String("(disabled)")));
}

// Called from the web server handlers whenever a setting changes. Every
// payload write refreshes the blob CRC before committing (#151).
void saveAlignment()    { writeSettingString(OFF_ALIGNMENT,  LEN_ALIGNMENT,  alignment);            updateSettingsCrc(); EEPROM.commit(); }
void saveFlapSpeed()    { writeSettingString(OFF_FLAPSPEED,  LEN_FLAPSPEED,  flapSpeed);            updateSettingsCrc(); EEPROM.commit(); }
void saveDeviceMode()   { writeSettingString(OFF_DEVICEMODE, LEN_DEVICEMODE, deviceMode);           updateSettingsCrc(); EEPROM.commit(); }
void saveTimezone()     { writeSettingString(OFF_TIMEZONE,   LEN_TIMEZONE,   timezonePosixSetting); updateSettingsCrc(); EEPROM.commit(); }
//Applied to the live identity on the next reboot — see resolveDeviceIdentity().
void saveDeviceName()   { writeSettingString(OFF_DEVICE_NAME, LEN_DEVICE_NAME, deviceNameSetting);  updateSettingsCrc(); EEPROM.commit(); }
//MQTT broker config (#57): all four slots in one commit; applied on reboot.
void saveMqttSettings() {
  writeSettingString(OFF_MQTT_HOST,     LEN_MQTT_HOST,     mqttHostSetting);
  writeSettingString(OFF_MQTT_PORT,     LEN_MQTT_PORT,     mqttPortSetting);
  writeSettingString(OFF_MQTT_USER,     LEN_MQTT_USER,     mqttUserSetting);
  writeSettingString(OFF_MQTT_PASSWORD, LEN_MQTT_PASSWORD, mqttPasswordSetting);
  updateSettingsCrc();
  EEPROM.commit();
}

// Persist the caller-supplied intended version at the start of a master OTA
// upload. Read back on the next boot to detect a silent revert (image
// rejected by eboot or crashed fast enough to trip recovery). See #52.
void saveIntendedVersion(const String& v) {
  writeSettingString(OFF_INTENDED_VERSION, LEN_INTENDED_VERSION, v);
  updateSettingsCrc();
  EEPROM.commit();
}
String readIntendedVersion() {
  return readSettingString(OFF_INTENDED_VERSION, LEN_INTENDED_VERSION);
}

// Record the outcome of the most recent master-OTA attempt. Written by the
// boot-time RTC-cookie check: "ok" (new sketchMD5 != pre-flash cookie) or
// "reverted" (same sketchMD5 post-reboot -> eboot rejected the copy). See #53.
void saveLastFlashResult(const String& v) {
  writeSettingString(OFF_LAST_FLASH_RESULT, LEN_LAST_FLASH_RESULT, v);
  updateSettingsCrc();
  EEPROM.commit();
}
String readLastFlashResult() {
  return readSettingString(OFF_LAST_FLASH_RESULT, LEN_LAST_FLASH_RESULT);
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

//Deferred apply for POST / (#150). The async handler only validates and
//stages into pendingSettingsPost; every shared-String mutation, EEPROM
//commit and configTime call happens here in loop() context. No yields
//inside — the drain is atomic with respect to async handlers, so an
//overlaying POST can never observe a half-applied state.
void applyPendingSettingsPost() {
  if (!pendingSettingsPost.pending) return;

  //"Last Received" tracks messages, not settings saves — with per-card
  //posts (#128) only a message/mode submission stamps it.
  if (pendingSettingsPost.inputTextProvided || pendingSettingsPost.deviceModeProvided) {
    lastReceivedMessageDateTime = formatDateTime("%d %b %y %H:%M:%S");
  }

  //Only if a new alignment value
  if (pendingSettingsPost.alignmentProvided && alignment != pendingSettingsPost.alignment) {
    alignment = pendingSettingsPost.alignment;
    alignmentUpdated = true;

    saveAlignment();
    SerialPrintln("Alignment Updated: " + alignment);
  }

  //Only if a new flap speed value
  if (pendingSettingsPost.flapSpeedProvided && flapSpeed != pendingSettingsPost.flapSpeed) {
    flapSpeed = pendingSettingsPost.flapSpeed;

    saveFlapSpeed();
    SerialPrintln("Flap Speed Updated: " + flapSpeed);
  }

  //Only if device mode has changed
  if (pendingSettingsPost.deviceModeProvided && deviceMode != pendingSettingsPost.deviceMode) {
    deviceMode = pendingSettingsPost.deviceMode;

    saveDeviceMode();
    //Explicit mode switch trumps a running MQTT notification (#130):
    //ask loopMqtt() to cancel it so the new mode shows immediately
    //instead of after the dwell.
    cancelActiveNotification();
    SerialPrintln("Device Mode Set: " + deviceMode);
  }

  //Only if a new timezone value was submitted and it changed.
  //Re-apply configTime() so the clock picks up the new zone on
  //its next tick — no reboot required. Issue #48.
  if (pendingSettingsPost.timezoneProvided && timezonePosixSetting != pendingSettingsPost.timezone) {
    timezonePosixSetting = pendingSettingsPost.timezone;
    saveTimezone();
    applyTimezoneAndNtp();
    SerialPrintln("Timezone Updated: " + (timezonePosixSetting.length() ? timezonePosixSetting : String("(default)")));
  }

  //Device name change (#125). Saved to EEPROM now, applied to
  //mDNS/hostname/MQTT/AP SSIDs on the next reboot. While we still ARE the
  //old MQTT identity, ask loopMqtt() to clear the old retained HA discovery
  //configs so the rename doesn't leave an orphaned device in Home Assistant.
  if (pendingSettingsPost.deviceNameProvided && deviceNameSetting != pendingSettingsPost.deviceName) {
    mqttRequestDiscoveryClear();
    deviceNameSetting = pendingSettingsPost.deviceName;
    saveDeviceName();
    SerialPrintln("Device Name Updated (reboot to apply): " + (deviceNameSetting.length() ? deviceNameSetting : String("(chip-id default)")));
  }

  //MQTT broker config (#57). Persisted now, applied on the next reboot —
  //initMqtt() runs once at boot and holds its own stable copies, so the
  //running client never sees these change under it. Password is write-only:
  //it only changes when a non-empty value was submitted.
  bool mqttChanged = false;
  if (pendingSettingsPost.mqttHostProvided && mqttHostSetting != pendingSettingsPost.mqttHost) { mqttHostSetting = pendingSettingsPost.mqttHost; mqttChanged = true; }
  if (pendingSettingsPost.mqttPortProvided && mqttPortSetting != pendingSettingsPost.mqttPort) { mqttPortSetting = pendingSettingsPost.mqttPort; mqttChanged = true; }
  if (pendingSettingsPost.mqttUserProvided && mqttUserSetting != pendingSettingsPost.mqttUser) { mqttUserSetting = pendingSettingsPost.mqttUser; mqttChanged = true; }
  if (pendingSettingsPost.mqttPasswordProvided && mqttPasswordSetting != pendingSettingsPost.mqttPassword) { mqttPasswordSetting = pendingSettingsPost.mqttPassword; mqttChanged = true; }
  if (mqttChanged) {
    saveMqttSettings();
    SerialPrintln("MQTT settings updated (reboot to apply). Broker: " + (mqttHostSetting.length() ? mqttHostSetting : String("(disabled)")));
  }

  //Only if we are showing text. deviceMode was applied above, so a POST
  //that switches to text mode and sets the message in one go works.
  if (pendingSettingsPost.inputTextProvided && deviceMode == DEVICE_MODE_TEXT) {
    inputText = pendingSettingsPost.inputText;
    //An explicit message send trumps an active notification/calibration
    //pattern — same intent rule as the #130 mode-change cancel. Without
    //this a calibration dwell would block the new message for minutes.
    cancelActiveNotification();
  }

  //Transient text (#165/#176): calibration patterns and timed messages own
  //the display for their dwell via the notification show-then-revert state,
  //then the normal mode content re-flaps by itself. Nothing is persisted —
  //a clock display stays a clock display.
  if (pendingSettingsPost.transientTextProvided) {
    showTransientText(pendingSettingsPost.transientText, pendingSettingsPost.transientDwell);
    //Dwell 0 = "not provided" -> showTransientText applies the 600 s default.
    SerialPrintln("Transient text (dwell " + (pendingSettingsPost.transientDwell > 0 ? String(pendingSettingsPost.transientDwell) + " s" : String("default")) + "): " + pendingSettingsPost.transientText);
  }

  //Reset for the next post; release the staged String heap while at it
  //(the password in particular shouldn't linger).
  pendingSettingsPost.alignmentProvided    = false; pendingSettingsPost.alignment    = String();
  pendingSettingsPost.flapSpeedProvided    = false; pendingSettingsPost.flapSpeed    = String();
  pendingSettingsPost.deviceModeProvided   = false; pendingSettingsPost.deviceMode   = String();
  pendingSettingsPost.inputTextProvided    = false; pendingSettingsPost.inputText    = String();
  pendingSettingsPost.transientTextProvided = false; pendingSettingsPost.transientText = String(); pendingSettingsPost.transientDwell = 0;
  pendingSettingsPost.timezoneProvided     = false; pendingSettingsPost.timezone     = String();
  pendingSettingsPost.deviceNameProvided   = false; pendingSettingsPost.deviceName   = String();
  pendingSettingsPost.mqttHostProvided     = false; pendingSettingsPost.mqttHost     = String();
  pendingSettingsPost.mqttPortProvided     = false; pendingSettingsPost.mqttPort     = String();
  pendingSettingsPost.mqttUserProvided     = false; pendingSettingsPost.mqttUser     = String();
  pendingSettingsPost.mqttPasswordProvided = false; pendingSettingsPost.mqttPassword = String();
  pendingSettingsPost.pending = false;
}

// --- moved from ESPMaster.ino (#174): the /settings JSON builder ---

//Gets all the currently stored values from memory as a JSON string.
//Hand-rolled to avoid pulling in the ArduinoJson library (~30 KB) for a
//fixed-shape serializer that never needs to parse (issue #40).
String getCurrentSettingValues() {
  String out;
  //Three per-unit arrays (address, versionStatus, version string) grow with
  //UNITS_AMOUNT — scale the reservation so big displays don't realloc-churn
  //the ~40 KB heap (#95). One-shot allocation, freed when the response sends.
  out.reserve(512 + UNITS_AMOUNT * 24);
  out += '{';

  out += F("\"timezoneOffset\":");          out += getTimezoneOffsetMinutes();
  //Detected display width (#123), not the UNITS_AMOUNT ceiling — the web
  //UI's line-count math and repeating-string actions follow the real size.
  out += F(",\"unitCount\":");              out += displayWidth;
  out += F(",\"detectedUnitCount\":");      out += detectedUnitCount;

  out += F(",\"detectedUnitAddresses\":[");
  for (int i = 0; i < detectedUnitCount; i++) {
    if (i) out += ',';
    out += detectedUnitAddresses[i];
  }
  out += ']';

  //Per-unit firmware version, indexed by unit slot (0..UNITS_AMOUNT-1) so
  //the UI can show a badge per address even for silent units (issue #28).
  out += F(",\"detectedUnitVersionStatus\":[");
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    if (i) out += ',';
    out += detectedUnitVersionStatus[i];
  }
  out += F("],\"detectedUnitVersions\":[");
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    if (i) out += ',';
    appendJsonString(out, detectedUnitVersions[i]);
  }
  out += ']';

  out += F(",\"alignment\":");                       appendJsonString(out, alignment);
  out += F(",\"flapSpeed\":");                       appendJsonString(out, flapSpeed);
  out += F(",\"deviceMode\":");                      appendJsonString(out, deviceMode);
  out += F(",\"timezonePosix\":");                   appendJsonString(out, timezonePosixSetting);
  //Per-device identity (#125): deviceName is the raw EEPROM value ("" =
  //unset), effectiveDeviceName is what the device actually uses right now.
  out += F(",\"deviceName\":");                      appendJsonString(out, deviceNameSetting);
  out += F(",\"effectiveDeviceName\":");             appendJsonString(out, effectiveDeviceName);
  //MQTT broker config (#57). The password is write-only: never echoed to
  //the browser; only whether one is stored.
  out += F(",\"mqttHost\":");                        appendJsonString(out, mqttHostSetting);
  out += F(",\"mqttPort\":");                        appendJsonString(out, mqttPortSetting);
  out += F(",\"mqttUser\":");                        appendJsonString(out, mqttUserSetting);
  out += F(",\"mqttPasswordSet\":");                 out += (mqttPasswordSetting.length() ? F("true") : F("false"));
  out += F(",\"mqttConnected\":");                   out += (mqttIsConnected() ? F("true") : F("false"));
  out += F(",\"version\":");                         appendJsonString(out, String(espVersion));
  //OTA failure diagnostics (#52). These fields let a remote flasher tell a
  //genuine revert apart from a same-version false-alarm. `sketchMd5` is the
  //MD5 of the running sketch as read from flash (via ESP.getSketchMD5(),
  //cached by core on first call) — unambiguous identity check independent
  //of GIT_REV strings, added in #53.
  out += F(",\"sketchMd5\":");                       appendJsonString(out, ESP.getSketchMD5());
  out += F(",\"lastFlashResult\":");                 appendJsonString(out, lastFlashResult);
  out += F(",\"intendedVersion\":");                 appendJsonString(out, intendedVersionEeprom);
  out += F(",\"otaReverted\":");                     out += (otaReverted ? F("true") : F("false"));
  out += F(",\"lastResetReason\":");                 appendJsonString(out, lastResetReason);
  out += F(",\"bootCounter\":");                     out += String(readBootStateRtc().bootCounter);
  out += F(",\"recoveryMode\":");                    out += (isRecoveryMode ? F("true") : F("false"));
  //True when the running image's flash-size header exceeds the physical
  //chip — the state where Update.begin() rejects every OTA (#92/#94).
  out += F(",\"flashConfigMismatch\":");             out += (ESP.getFlashChipRealSize() < ESP.getFlashChipSize() ? F("true") : F("false"));
  out += F(",\"lastTimeReceivedMessageDateTime\":"); appendJsonString(out, lastReceivedMessageDateTime);
  out += F(",\"lastWrittenText\":");                 appendJsonString(out, lastWrittenText);

  out += F(",\"otaEnabled\":false");
  out += F(",\"isInOtaMode\":");                    out += (isOtaMode ? F("true") : F("false"));

  out += F(",\"wifiSettingsResettable\":true");

  out += '}';
  return out;
}
