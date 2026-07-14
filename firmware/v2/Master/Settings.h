#pragma once
// Settings.h — the v2 master's runtime settings over a SettingsStore (#185).
//
// Storage is NVS (namespace "splitflap"); there is no version byte, CRC, or
// migration ladder — NVS guards integrity and a missing key simply yields
// its default, so "adding a setting" is one getString/getInt call with a
// default. What NVS does NOT guard is semantics: a stored value can be
// stale-invalid (written by buggier firmware) — loadSettings() therefore
// sanitizes every field through the same validators the web boundary uses,
// falling back to the field's default rather than propagating garbage.
//
// Save helpers are deliberately write-through (no validation): validation
// belongs at the input boundary (web handler), sanitation at load. The one
// piece of policy here is saveMqttConfig()'s write-only password — an empty
// submission keeps the stored secret (v1 #57 semantics).

#include <Arduino.h>

#include "DeviceIdentity.h"
#include "SettingsLimits.h"
#include "SettingsStore.h"
#include "SettingsValidation.h"

// v1 fresh-init parity (resetSettingsToDefaults in ServiceSettingsFunctions).
#define SETTINGS_DEFAULT_ALIGNMENT   "left"
#define SETTINGS_DEFAULT_FLAP_SPEED  80
#define SETTINGS_DEFAULT_DEVICE_MODE "text"
#define SETTINGS_DEFAULT_TIMEZONE    "CET-1CEST,M3.5.0,M10.5.0/3"
#define SETTINGS_DEFAULT_MQTT_PORT   1883

struct MasterSettings {
  String alignment;
  int flapSpeed;
  String deviceMode;
  String timezonePosix;
  String deviceName;       // "" = chip-id default via resolveDeviceName()
  String wifiSsid;         // "" = unprovisioned (boot lands in the portal)
  String wifiPass;         // "" = open network; never serialized to /settings
  String mqttHost;         // "" = MQTT disabled
  int mqttPort;
  String mqttUser;
  String mqttPassword;
  String intendedVersion;  // ?v= diagnostic from /firmware/master (#190);
                           // the flash VERDICT is synthesized from esp_ota
                           // state (OtaService), never persisted here
  int unitCountOverride;   // #289 dummy mode: 0 = auto (probe-derived
                           // width), 1..UNITS_AMOUNT pins the width
};

// NVS keys (hard 15-char limit).
#define SETTINGS_KEY_ALIGNMENT    "alignment"
#define SETTINGS_KEY_FLAP_SPEED   "flapSpeed"
#define SETTINGS_KEY_DEVICE_MODE  "deviceMode"
#define SETTINGS_KEY_TIMEZONE     "tzPosix"
#define SETTINGS_KEY_DEVICE_NAME  "deviceName"
#define SETTINGS_KEY_WIFI_SSID    "wifiSsid"
#define SETTINGS_KEY_WIFI_PASS    "wifiPass"
#define SETTINGS_KEY_MQTT_HOST    "mqttHost"
#define SETTINGS_KEY_MQTT_PORT    "mqttPort"
#define SETTINGS_KEY_MQTT_USER    "mqttUser"
#define SETTINGS_KEY_MQTT_PASS    "mqttPass"
#define SETTINGS_KEY_INTENDED_VER "intendedVer"
#define SETTINGS_KEY_UNIT_COUNT   "unitCount"

// Bounded free-text sanitation: printable ASCII and shorter than the
// limit, else default.
static inline String sanitizeBoundedText(const String& v, int limit,
                                         const char* def) {
  if ((int)v.length() < limit && settingsIsPrintableAscii(v, 0x20)) {
    return v;
  }
  return String(def);
}

// Shared rule for the ?v= flash diagnostic (#191): the /firmware/master
// drain and the boot load path must sanitize identically.
static inline String sanitizeIntendedVersion(const String& v) {
  return sanitizeBoundedText(v, LEN_INTENDED_VERSION, "");
}

inline MasterSettings loadSettings(SettingsStore& store) {
  MasterSettings s;

  s.alignment = store.getString(SETTINGS_KEY_ALIGNMENT, SETTINGS_DEFAULT_ALIGNMENT);
  if (!isValidAlignmentValue(s.alignment)) s.alignment = SETTINGS_DEFAULT_ALIGNMENT;

  s.flapSpeed = store.getInt(SETTINGS_KEY_FLAP_SPEED, SETTINGS_DEFAULT_FLAP_SPEED);
  if (s.flapSpeed < 1 || s.flapSpeed > 100) s.flapSpeed = SETTINGS_DEFAULT_FLAP_SPEED;

  s.deviceMode = store.getString(SETTINGS_KEY_DEVICE_MODE, SETTINGS_DEFAULT_DEVICE_MODE);
  if (!isValidDeviceModeValue(s.deviceMode)) s.deviceMode = SETTINGS_DEFAULT_DEVICE_MODE;

  s.timezonePosix = store.getString(SETTINGS_KEY_TIMEZONE, SETTINGS_DEFAULT_TIMEZONE);
  if (!isValidTimezoneValue(s.timezonePosix, LEN_TIMEZONE)) {
    s.timezonePosix = SETTINGS_DEFAULT_TIMEZONE;
  }

  s.deviceName = store.getString(SETTINGS_KEY_DEVICE_NAME, "");
  if (s.deviceName.length() > 0 && !isValidDeviceName(s.deviceName)) {
    s.deviceName = "";  // sentinel: identity falls back to chip-id default
  }

  // WiFi credentials (#188) sanitize as a pair: a password without a usable
  // ssid is dead weight (-> both cleared, boot lands in the portal), while a
  // corrupt password alone keeps the ssid so the join fails fast instead of
  // erasing a recoverable network name.
  s.wifiSsid = store.getString(SETTINGS_KEY_WIFI_SSID, "");
  s.wifiPass = store.getString(SETTINGS_KEY_WIFI_PASS, "");
  if (s.wifiSsid.length() > 0 && !isValidWifiSsidValue(s.wifiSsid, LEN_WIFI_SSID)) {
    s.wifiSsid = "";
  }
  if (s.wifiSsid.length() == 0 ||
      !isValidWifiPasswordValue(s.wifiPass, LEN_WIFI_PASSWORD)) {
    s.wifiPass = "";
  }

  s.mqttHost = store.getString(SETTINGS_KEY_MQTT_HOST, "");
  if (!isValidMqttHostOrUserValue(s.mqttHost, LEN_MQTT_HOST)) s.mqttHost = "";

  s.mqttPort = store.getInt(SETTINGS_KEY_MQTT_PORT, SETTINGS_DEFAULT_MQTT_PORT);
  if (s.mqttPort < 1 || s.mqttPort > 65535) s.mqttPort = SETTINGS_DEFAULT_MQTT_PORT;

  s.mqttUser = store.getString(SETTINGS_KEY_MQTT_USER, "");
  if (!isValidMqttHostOrUserValue(s.mqttUser, LEN_MQTT_USER)) s.mqttUser = "";

  s.mqttPassword = store.getString(SETTINGS_KEY_MQTT_PASS, "");
  if (!isValidMqttPasswordValue(s.mqttPassword, LEN_MQTT_PASSWORD)) {
    s.mqttPassword = "";
  }

  s.intendedVersion =
      sanitizeIntendedVersion(store.getString(SETTINGS_KEY_INTENDED_VER, ""));

  s.unitCountOverride = store.getInt(SETTINGS_KEY_UNIT_COUNT, 0);
  if (s.unitCountOverride < 0 || s.unitCountOverride > UNITS_AMOUNT) {
    s.unitCountOverride = 0;  // auto
  }

  return s;
}

inline void saveAlignment(SettingsStore& store, const String& v) {
  store.putString(SETTINGS_KEY_ALIGNMENT, v);
}

inline void saveFlapSpeed(SettingsStore& store, int v) {
  store.putInt(SETTINGS_KEY_FLAP_SPEED, v);
}

inline void saveDeviceMode(SettingsStore& store, const String& v) {
  store.putString(SETTINGS_KEY_DEVICE_MODE, v);
}

inline void saveTimezone(SettingsStore& store, const String& v) {
  store.putString(SETTINGS_KEY_TIMEZONE, v);
}

inline void saveDeviceName(SettingsStore& store, const String& v) {
  store.putString(SETTINGS_KEY_DEVICE_NAME, v);
}

inline void saveIntendedVersion(SettingsStore& store, const String& v) {
  store.putString(SETTINGS_KEY_INTENDED_VER, v);
}

inline void saveUnitCountOverride(SettingsStore& store, int v) {
  store.putInt(SETTINGS_KEY_UNIT_COUNT, v);
}

// WiFi credentials (#188): always written as a pair — the portal submits
// both fields together (an empty password is a real value: open network),
// so there is no keep-the-stored-secret rule here, unlike MQTT below.
inline void saveWifiCredentials(SettingsStore& store, const String& ssid,
                                const String& password) {
  store.putString(SETTINGS_KEY_WIFI_SSID, ssid);
  store.putString(SETTINGS_KEY_WIFI_PASS, password);
}

// /reset-wifi: our NVS namespace is the single credential store (esp_wifi
// persistence is off), so two key deletes ARE the factory-fresh WiFi state.
inline void clearWifiCredentials(SettingsStore& store) {
  store.remove(SETTINGS_KEY_WIFI_SSID);
  store.remove(SETTINGS_KEY_WIFI_PASS);
}

// Write-only password: an empty `password` means "keep the stored secret".
inline void saveMqttConfig(SettingsStore& store, const String& host, int port,
                           const String& user, const String& password) {
  store.putString(SETTINGS_KEY_MQTT_HOST, host);
  store.putInt(SETTINGS_KEY_MQTT_PORT, port);
  store.putString(SETTINGS_KEY_MQTT_USER, user);
  if (password.length() > 0) {
    store.putString(SETTINGS_KEY_MQTT_PASS, password);
  }
}
