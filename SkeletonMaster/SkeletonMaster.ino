// SkeletonMaster — minimal OTA bootstrap firmware for the ESP-01 master.
// See issue #61.
//
// Staged-OTA strategy: flash this tiny firmware first, use it as a base
// to OTA the full firmware. Isolates "does OTA work on this device?"
// from "does the full image OTA reliably?" and provides a known-bootable
// fallback for when the full image silent-reverts.
//
// Contract with the full firmware:
//   - Same flash layout (eagle.flash.1m.ld, ESP-01 1 MB, no FS) so eboot
//     run/staging slots overlap identically.
//   - Same RTC boot-state struct and MAGIC, so a pre-flash cookie written
//     by either firmware is readable by the other. This means skeleton
//     can resolve a silent revert caused by a failed full-firmware flash,
//     and vice versa.
//   - Same EEPROM layout (SettingsEepromLayout.h), so hopping through
//     the skeleton doesn't wipe user settings (alignment, flapSpeed,
//     deviceMode, timezonePosix). Skeleton only touches the OTA-related
//     slots (intendedVersion, lastFlashResult); all others pass through.
//   - Same /settings JSON field names so ota-master.sh's verdict decoder
//     works unchanged when pointed at the skeleton.
//
// What's deliberately NOT here:
//   - I2C / Wire / unit management
//   - Bundled unit firmware PROGMEM
//   - Web UI assets, GET /
//   - NTP / clock / text / alignment / flapSpeed
//   - mDNS, Arduino OTA (separate from web OTA), WiFiManager
//   - WebLog ring buffer (plain Serial; no I2C conflict here)
//   - SoftAP fallback — always fixed to the hardcoded WiFi. If that WiFi
//     is down, the device can't serve OTA. That's by design: skeleton is
//     for known-WiFi recovery, not zero-infrastructure recovery.

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include "SettingsEepromLayout.h"
#include "BuildVersion.h"

#if __has_include("WifiCredentials.h")
  #include "WifiCredentials.h"
#else
  #warning "WifiCredentials.h missing — copy ESPMaster/WifiCredentials.h.example to ESPMaster/WifiCredentials.h"
  const char* wifiDirectSsid = "";
  const char* wifiDirectPassword = "";
#endif

// How long to wait for WiFi before giving up on a connect attempt.
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000;

// Clean uptime before we reset the RTC boot counter. Matches ESPMaster's
// HEALTHY_BOOT_MS so the recovery trip semantics are identical.
static const unsigned long HEALTHY_BOOT_MS = 30000;

static const uint16_t WEB_PORT = 80;

// --- RTC boot state ---
// DUPLICATED from ESPMaster.ino (not a header there). Must stay in sync —
// if the layout or MAGIC diverges, pre-flash cookies won't round-trip
// between skeleton and full firmware.
#define PRE_FLASH_MD5_LEN 36
struct RtcBootState {
  uint32_t magic;
  uint32_t bootCounter;
  char     preFlashSketchMd5[PRE_FLASH_MD5_LEN];
};
static const uint32_t RTC_BOOT_MAGIC = 0xC0FFEE43UL;
static const uint32_t RTC_BOOT_OFFSET_BLOCKS = 0;
static const uint32_t RECOVERY_BOOT_THRESHOLD = 3;

static RtcBootState readBootStateRtc() {
  RtcBootState state;
  ESP.rtcUserMemoryRead(RTC_BOOT_OFFSET_BLOCKS,
                        reinterpret_cast<uint32_t*>(&state),
                        sizeof(state));
  if (state.magic != RTC_BOOT_MAGIC) {
    memset(&state, 0, sizeof(state));
    state.magic = RTC_BOOT_MAGIC;
  }
  return state;
}

static void writeBootStateRtc(RtcBootState state) {
  ESP.rtcUserMemoryWrite(RTC_BOOT_OFFSET_BLOCKS,
                         reinterpret_cast<uint32_t*>(&state),
                         sizeof(state));
}

static void clearBootCounterRtc() {
  RtcBootState state = readBootStateRtc();
  state.bootCounter = 0;
  writeBootStateRtc(state);
}

static void setPreFlashCookieRtc(const String& sketchMd5) {
  RtcBootState state = readBootStateRtc();
  size_t copy = sketchMd5.length();
  if (copy >= PRE_FLASH_MD5_LEN) copy = PRE_FLASH_MD5_LEN - 1;
  memcpy(state.preFlashSketchMd5, sketchMd5.c_str(), copy);
  state.preFlashSketchMd5[copy] = '\0';
  writeBootStateRtc(state);
}

static void clearPreFlashCookieRtc() {
  RtcBootState state = readBootStateRtc();
  memset(state.preFlashSketchMd5, 0, PRE_FLASH_MD5_LEN);
  writeBootStateRtc(state);
}

// --- State ---
static ESP8266WebServer webServer(WEB_PORT);
static bool isPendingReboot = false;
static bool recoveryTripped = false;
static bool otaReverted = false;
static String lastFlashResult;
static String intendedVersion;
static String lastResetReason;
static uint32_t bootCounter = 0;
static unsigned long setupCompleteMs = 0;
static bool healthyBootResolved = false;

// WiFi TX power management during upload — mirrors ESPMaster (#60).
static bool otaTxPowerReduced = false;
static constexpr float OTA_TX_POWER_DBM = 10.0f;
static constexpr float DEFAULT_TX_POWER_DBM = 20.5f;

// --- EEPROM helpers (only OTA-related slots; we do NOT touch user
// settings like alignment / flapSpeed / deviceMode / timezonePosix) ---
static String readLastFlashResult() {
  return readSettingString(OFF_LAST_FLASH_RESULT, LEN_LAST_FLASH_RESULT);
}

static void writeLastFlashResult(const String& v) {
  writeSettingString(OFF_LAST_FLASH_RESULT, LEN_LAST_FLASH_RESULT, v);
  EEPROM.commit();
}

static String readIntendedVersion() {
  return readSettingString(OFF_INTENDED_VERSION, LEN_INTENDED_VERSION);
}

static void saveIntendedVersion(const String& v) {
  writeSettingString(OFF_INTENDED_VERSION, LEN_INTENDED_VERSION, v);
  EEPROM.commit();
}

static void resolvePreFlashCookie() {
  RtcBootState state = readBootStateRtc();
  if (state.preFlashSketchMd5[0] == '\0') return;

  String runningMd5 = ESP.getSketchMD5();
  bool reverted = (runningMd5 == state.preFlashSketchMd5);
  Serial.print(F("Pre-flash cookie resolved: "));
  Serial.print(reverted ? F("reverted") : F("ok"));
  Serial.print(F(" (cookie="));
  Serial.print(state.preFlashSketchMd5);
  Serial.print(F(", running="));
  Serial.print(runningMd5);
  Serial.println(F(")"));

  writeLastFlashResult(reverted ? "reverted" : "ok");
  clearPreFlashCookieRtc();
}

// --- WiFi ---
static bool connectWifi() {
  if (wifiDirectSsid[0] == '\0') {
    Serial.println(F("WiFi creds missing — cannot connect"));
    return false;
  }
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiDirectSsid, wifiDirectPassword);

  Serial.print(F("Connecting to "));
  Serial.println(wifiDirectSsid);

  unsigned long deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("WiFi connect failed within timeout"));
    return false;
  }

  Serial.print(F("WiFi connected: "));
  Serial.println(WiFi.localIP().toString());
  return true;
}

// --- Handlers ---
static void handleSettings() {
  // JSON field names mirror ESPMaster so ota-master.sh parses either
  // firmware identically. Only the subset we can actually report is
  // included; fields the skeleton doesn't own (alignment, flapSpeed,
  // etc.) are deliberately omitted — the flasher never reads them.
  String body;
  body.reserve(384);
  body += F("{");
  body += F("\"version\":\"");         body += GIT_REV;                          body += F("\",");
  body += F("\"sketchMd5\":\"");       body += ESP.getSketchMD5();               body += F("\",");
  body += F("\"lastFlashResult\":\""); body += lastFlashResult;                  body += F("\",");
  body += F("\"intendedVersion\":\""); body += intendedVersion;                  body += F("\",");
  body += F("\"otaReverted\":");       body += (otaReverted ? F("true") : F("false")); body += F(",");
  body += F("\"lastResetReason\":\""); body += lastResetReason;                  body += F("\",");
  body += F("\"bootCounter\":");       body += bootCounter;                      body += F(",");
  body += F("\"recoveryMode\":");      body += (recoveryTripped ? F("true") : F("false")); body += F(",");
  body += F("\"skeletonBuild\":true");
  body += F("}");
  webServer.send(200, "application/json", body);
}

static void handleFirmwareUploadFinish() {
  if (Update.hasError()) {
    webServer.send(500, "text/plain", String(F("Update failed: ")) + Update.getErrorString());
    return;
  }
  if (!Update.isFinished()) {
    webServer.send(500, "text/plain", F("Update incomplete"));
    return;
  }
  webServer.send(200, "text/plain", F("Master firmware flashed; rebooting…"));
  setPreFlashCookieRtc(ESP.getSketchMD5());
  clearBootCounterRtc();
  isPendingReboot = true;
}

static void handleFirmwareUpload() {
  HTTPUpload& upload = webServer.upload();
  switch (upload.status) {
    case UPLOAD_FILE_START: {
      Serial.print(F("OTA upload start: "));
      Serial.println(upload.filename);

      // Drop TX power for the upload write storm (#60). Restored on
      // UPLOAD_FILE_END / UPLOAD_FILE_ABORTED, or in setup's loop if the
      // reboot triggers before a clean end.
      WiFi.setOutputPower(OTA_TX_POWER_DBM);
      otaTxPowerReduced = true;
      Serial.print(F("WiFi TX reduced to "));
      Serial.print(OTA_TX_POWER_DBM);
      Serial.println(F(" dBm for OTA upload"));

      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      if (!Update.begin(maxSketchSpace)) {
        Serial.print(F("Update.begin failed: "));
        Serial.println(Update.getErrorString());
        return;
      }
      if (webServer.hasArg("md5")) {
        String md5 = webServer.arg("md5");
        md5.toLowerCase();
        if (md5.length() != 32 || !Update.setMD5(md5.c_str())) {
          Serial.println(F("Bad md5 param — upload rejected"));
          Update.end(false);
          return;
        }
        Serial.print(F("MD5 expected: "));
        Serial.println(md5);
      }
      // Record intended version unconditionally (empty string if the
      // caller didn't pass ?v=) so stale values from a prior flash don't
      // linger and trip a false-positive otaReverted on the next boot.
      String intended;
      if (webServer.hasArg("v")) {
        intended = webServer.arg("v");
      }
      saveIntendedVersion(intended);
      Serial.print(F("Intended version recorded: \""));
      Serial.print(intended);
      Serial.println(F("\""));
      break;
    }
    case UPLOAD_FILE_WRITE: {
      if (Update.hasError()) return;
      size_t written = Update.write(upload.buf, upload.currentSize);
      if (written != upload.currentSize) {
        Serial.print(F("Update.write short: "));
        Serial.println(Update.getErrorString());
      }
      break;
    }
    case UPLOAD_FILE_END: {
      if (Update.end(true)) {
        Serial.print(F("OTA complete, "));
        Serial.print(upload.totalSize);
        Serial.println(F(" bytes"));
      } else {
        Serial.print(F("Update.end failed: "));
        Serial.println(Update.getErrorString());
      }
      if (otaTxPowerReduced) {
        WiFi.setOutputPower(DEFAULT_TX_POWER_DBM);
        otaTxPowerReduced = false;
      }
      break;
    }
    case UPLOAD_FILE_ABORTED: {
      Update.end(false);
      if (otaTxPowerReduced) {
        WiFi.setOutputPower(DEFAULT_TX_POWER_DBM);
        otaTxPowerReduced = false;
      }
      Serial.println(F("OTA aborted"));
      break;
    }
  }
}

static void handleRecoverMark() {
  // Remote trigger to force recovery on next boot — same semantics as
  // ESPMaster's endpoint. Only matters if the operator wants to bounce
  // through recovery deliberately; skeleton itself doesn't behave any
  // differently in recovery, but the flag is reflected in /settings.
  RtcBootState state = readBootStateRtc();
  state.bootCounter = RECOVERY_BOOT_THRESHOLD;
  writeBootStateRtc(state);
  webServer.send(200, "text/plain", F("Recovery mark written; rebooting"));
  isPendingReboot = true;
}

static void registerRoutes() {
  webServer.on("/settings", HTTP_GET, handleSettings);
  webServer.on("/firmware/master", HTTP_POST, handleFirmwareUploadFinish, handleFirmwareUpload);
  webServer.on("/firmware/recover-mark", HTTP_POST, handleRecoverMark);
  webServer.onNotFound([]() { webServer.send(404, "text/plain", F("skeleton: not found")); });
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println();
  Serial.print(F("Split-Flap Skeleton "));
  Serial.println(GIT_REV);

  lastResetReason = ESP.getResetReason();
  Serial.print(F("Reset reason: "));
  Serial.println(lastResetReason);

  EEPROM.begin(SETTINGS_EEPROM_SIZE);
  // Intentional: don't initialize magic/version here. Skeleton reads
  // slots that might already be populated by the full firmware and
  // writes the OTA-related ones. Fresh EEPROM (no magic) just yields
  // empty strings — that's correct for a first boot and the full
  // firmware will initialize settings on its next boot.
  lastFlashResult = readLastFlashResult();
  intendedVersion = readIntendedVersion();
  otaReverted = (intendedVersion.length() > 0 && intendedVersion != String(GIT_REV));

  // Resolve a pending pre-flash cookie (may update lastFlashResult on
  // disk). This is the diagnostic path that turns a silent eboot revert
  // into a surfaced verdict via /settings.
  resolvePreFlashCookie();
  lastFlashResult = readLastFlashResult();

  RtcBootState bootState = readBootStateRtc();
  bootState.bootCounter++;
  writeBootStateRtc(bootState);
  bootCounter = bootState.bootCounter;
  recoveryTripped = (bootCounter >= RECOVERY_BOOT_THRESHOLD);
  Serial.print(F("RTC boot counter: "));
  Serial.print(bootCounter);
  if (recoveryTripped) Serial.print(F(" (recovery tripped)"));
  Serial.println();

  // No SoftAP fallback — always fix to the hardcoded WiFi. If WiFi is
  // down, we can't serve OTA and the device is useless remotely, which
  // is the whole point: we're not trying to solve zero-infrastructure
  // recovery here.
  while (!connectWifi()) {
    Serial.println(F("WiFi retry in 10 s..."));
    delay(10000);
  }

  registerRoutes();
  webServer.begin();
  Serial.print(F("Skeleton ready on "));
  Serial.println(WiFi.localIP().toString());
  setupCompleteMs = millis();
}

void loop() {
  webServer.handleClient();

  if (!healthyBootResolved && millis() - setupCompleteMs >= HEALTHY_BOOT_MS) {
    clearBootCounterRtc();
    healthyBootResolved = true;
    Serial.println(F("Healthy boot — RTC boot counter cleared"));
  }

  if (isPendingReboot) {
    delay(100);
    ESP.restart();
  }
}
