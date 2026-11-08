#pragma once
// PendingSettingsPost.h — the POST / staging pipeline (#186), the pure core
// of v1's ServiceWebEndpoints.ino form handler + applyPendingSettingsPost().
//
// v1 semantics, natively tested:
//  - every field optional (#128 per-card saves); a field that wasn't
//    provided must never be applied,
//  - all-or-nothing per POST: one invalid field rejects the submission
//    (the handler parses into a local post and only merges on success),
//  - overlay across POSTs: a second card's post before the drain adds its
//    own fields (mergeSettingsPost),
//  - MQTT password is write-only: an empty submission means "keep stored",
//  - persistence is only-if-changed (applySettingsPost).
//
// The async handler stages; applySettingsPost() runs from loop() context —
// the v1 async-context rule (#150) carries over: handlers never write the
// store. Display-bound fields (inputText, transientText/Dwell) are parsed
// and staged for wire-contract parity, but consuming them is the flap
// service's job — the loop drain reads them off the post before apply.

#include <Arduino.h>

#include "DeviceIdentity.h"
#include "Settings.h"
#include "SettingsLimits.h"
#include "SettingsValidation.h"

// POST field names — the v1 wire contract (ESPMaster.ino).
#define PARAM_ALIGNMENT       "alignment"
#define PARAM_FLAP_SPEED      "flapSpeed"
#define PARAM_DEVICEMODE      "deviceMode"
#define PARAM_INPUT_TEXT      "inputText"
#define PARAM_TRANSIENT_TEXT  "transientText"
#define PARAM_TRANSIENT_DWELL "transientDwell"
#define PARAM_TRANSIENT_HOLD  "transientHold"   // #616: until the next text or a clear
#define PARAM_TEXT_PRIORITY   "textPriority"    // #625: 0..TEXT_PRIORITY_MAX
#define PARAM_TIMEZONE        "timezone"
#define PARAM_DEVICE_NAME     "deviceName"
#define PARAM_MQTT_HOST       "mqttHost"
#define PARAM_MQTT_PORT       "mqttPort"
#define PARAM_MQTT_USER       "mqttUser"
#define PARAM_MQTT_PASSWORD   "mqttPassword"
#define PARAM_UNIT_COUNT      "unitCount"
#define PARAM_REFLASH_ON_BOOT "reflashOnBoot"
#define PARAM_WIFI_POWER_SAVE "wifiPowerSave"
#define PARAM_QUIET           "quiet"
#define PARAM_RELEASE_CHECK   "releaseCheck"
#define PARAM_RELEASE_CHANNEL "releaseChannel"

struct PendingSettingsPost {
  bool pending = false;

  String alignment;     bool alignmentProvided = false;
  String flapSpeed;     bool flapSpeedProvided = false;
  String deviceMode;    bool deviceModeProvided = false;
  String inputText;     bool inputTextProvided = false;
  String transientText; bool transientTextProvided = false;
  long transientDwell = 0;
  bool transientDwellProvided = false;
  bool transientHold = false;  // #616
  bool transientHoldProvided = false;
  long textPriority = 0;       // #625
  bool textPriorityProvided = false;
  String timezone;      bool timezoneProvided = false;
  String deviceName;    bool deviceNameProvided = false;
  String mqttHost;      bool mqttHostProvided = false;
  String mqttPort;      bool mqttPortProvided = false;
  String mqttUser;      bool mqttUserProvided = false;
  String mqttPassword;  bool mqttPasswordProvided = false;
  String unitCount;     bool unitCountProvided = false;
  String reflashOnBoot; bool reflashOnBootProvided = false;  // #412
  String quiet;         bool quietProvided = false;          // #227
  String wifiPowerSave; bool wifiPowerSaveProvided = false;  // #609
  String releaseCheck;   bool releaseCheckProvided = false;   // #583
  String releaseChannel; bool releaseChannelProvided = false; // #583
};

enum class SettingsParamResult {
  Ignored,   // not a known settings field (or empty password = keep stored)
  Accepted,  // validated, normalized and staged
  Invalid,   // known field, failed validation — reject the whole POST
};

// The fields that stage as a String: the name, where the value and its
// provided flag live, whether the value is trimmed first, and its validator
// (nullptr = any value).
struct SettingsStringField {
  const char* name;
  String PendingSettingsPost::*value;
  bool PendingSettingsPost::*provided;
  bool trim;
  bool (*valid)(const String&);
};

// #412/#227/#583/#609: strictly "true"/"false" — a bare presence check or a
// truthy-string rule would let a typo silently disarm the unattended
// recovery path (#412), silence or wake the wall (#227), or flip the daily
// look (#583).
inline bool isStrictSettingsFlag(const String& v) { return v == "true" || v == "false"; }
inline bool isValidTimezoneParam(const String& v) { return isValidTimezoneValue(v, LEN_TIMEZONE); }
inline bool isValidMqttHostParam(const String& v) { return isValidMqttHostOrUserValue(v, LEN_MQTT_HOST); }
inline bool isValidMqttUserParam(const String& v) { return isValidMqttHostOrUserValue(v, LEN_MQTT_USER); }
inline bool isValidMqttPortParam(const String& v) { return isValidMqttPortValue(v, LEN_MQTT_PORT); }
inline bool isValidAlignmentParam(const String& v) { return isValidAlignmentValue(v); }
inline bool isValidFlapSpeedParam(const String& v) { return isValidFlapSpeedValue(v); }
inline bool isValidDeviceModeParam(const String& v) { return isValidDeviceModeValue(v); }
inline bool isValidReleaseChannelParam(const String& v) { return isValidReleaseChannelValue(v); }
inline bool isValidUnitCountParam(const String& v) { return isValidUnitCountOverrideValue(v); }

inline const SettingsStringField* findSettingsStringField(const String& name) {
  using P = PendingSettingsPost;
  static const SettingsStringField fields[] = {
      {PARAM_ALIGNMENT, &P::alignment, &P::alignmentProvided, false, isValidAlignmentParam},
      {PARAM_FLAP_SPEED, &P::flapSpeed, &P::flapSpeedProvided, false, isValidFlapSpeedParam},
      {PARAM_DEVICEMODE, &P::deviceMode, &P::deviceModeProvided, false, isValidDeviceModeParam},
      {PARAM_INPUT_TEXT, &P::inputText, &P::inputTextProvided, false, nullptr},
      {PARAM_TRANSIENT_TEXT, &P::transientText, &P::transientTextProvided, false, nullptr},
      {PARAM_TIMEZONE, &P::timezone, &P::timezoneProvided, false, isValidTimezoneParam},
      {PARAM_MQTT_HOST, &P::mqttHost, &P::mqttHostProvided, true, isValidMqttHostParam},
      {PARAM_MQTT_USER, &P::mqttUser, &P::mqttUserProvided, true, isValidMqttUserParam},
      {PARAM_MQTT_PORT, &P::mqttPort, &P::mqttPortProvided, true, isValidMqttPortParam},
      {PARAM_WIFI_POWER_SAVE, &P::wifiPowerSave, &P::wifiPowerSaveProvided, true, isStrictSettingsFlag},
      {PARAM_REFLASH_ON_BOOT, &P::reflashOnBoot, &P::reflashOnBootProvided, true, isStrictSettingsFlag},
      {PARAM_QUIET, &P::quiet, &P::quietProvided, true, isStrictSettingsFlag},
      {PARAM_RELEASE_CHECK, &P::releaseCheck, &P::releaseCheckProvided, true, isStrictSettingsFlag},
      {PARAM_RELEASE_CHANNEL, &P::releaseChannel, &P::releaseChannelProvided, false, isValidReleaseChannelParam},
      {PARAM_UNIT_COUNT, &P::unitCount, &P::unitCountProvided, true, isValidUnitCountParam},
  };
  for (const SettingsStringField& field : fields) {
    if (name == field.name) return &field;
  }
  return nullptr;
}

inline SettingsParamResult stageSettingsStringField(PendingSettingsPost& post,
                                                    const SettingsStringField& field,
                                                    const String& rawValue) {
  String value = rawValue;
  if (field.trim) value.trim();
  if (field.valid != nullptr && !field.valid(value)) return SettingsParamResult::Invalid;
  post.*field.value = value;
  post.*field.provided = true;
  return SettingsParamResult::Accepted;
}

inline SettingsParamResult stageTransientDwell(PendingSettingsPost& post, const String& rawValue) {
  String trimmed = rawValue;
  trimmed.trim();
  if (!isValidTransientDwellValue(trimmed)) return SettingsParamResult::Invalid;
  post.transientDwell = trimmed.toInt();
  post.transientDwellProvided = true;
  return SettingsParamResult::Accepted;
}

// #616: strictly "true"/"false", like every flag.
inline SettingsParamResult stageTransientHold(PendingSettingsPost& post, const String& rawValue) {
  String trimmed = rawValue;
  trimmed.trim();
  if (!isStrictSettingsFlag(trimmed)) return SettingsParamResult::Invalid;
  post.transientHold = trimmed == "true";
  post.transientHoldProvided = true;
  return SettingsParamResult::Accepted;
}

// #625: a whole number 0..TEXT_PRIORITY_MAX, digits only.
inline SettingsParamResult stageTextPriority(PendingSettingsPost& post, const String& rawValue) {
  String trimmed = rawValue;
  trimmed.trim();
  if (trimmed.length() == 0 || trimmed.length() > 3) return SettingsParamResult::Invalid;
  for (unsigned int i = 0; i < trimmed.length(); i++) {
    if (trimmed[i] < '0' || trimmed[i] > '9') return SettingsParamResult::Invalid;
  }
  const long value = trimmed.toInt();
  if (value > TEXT_PRIORITY_MAX) return SettingsParamResult::Invalid;
  post.textPriority = value;
  post.textPriorityProvided = true;
  return SettingsParamResult::Accepted;
}

inline SettingsParamResult stageDeviceName(PendingSettingsPost& post, const String& rawValue) {
  String normalized = normalizeDeviceName(rawValue);
  // Empty is valid and means "reset to the chip-id default" (#125).
  if (normalized.length() > 0 && !isValidDeviceName(normalized)) {
    return SettingsParamResult::Invalid;
  }
  post.deviceName = normalized;
  post.deviceNameProvided = true;
  return SettingsParamResult::Accepted;
}

inline SettingsParamResult stageMqttPassword(PendingSettingsPost& post, const String& rawValue) {
  // Write-only (#57): an empty field means "keep the stored one" and is
  // deliberately NOT treated as provided.
  if (rawValue.length() == 0) return SettingsParamResult::Ignored;
  if (!isValidMqttPasswordValue(rawValue, LEN_MQTT_PASSWORD)) {
    return SettingsParamResult::Invalid;
  }
  post.mqttPassword = rawValue;
  post.mqttPasswordProvided = true;
  return SettingsParamResult::Accepted;
}

// Validate one POST parameter and stage it into `post`. Normalization
// mirrors v1: MQTT host/user/port, the unit count, the flags and the dwell
// are trimmed before validation, the device name is lowercased via
// normalizeDeviceName().
inline SettingsParamResult stageSettingsParam(PendingSettingsPost& post,
                                              const String& name,
                                              const String& rawValue) {
  const SettingsStringField* field = findSettingsStringField(name);
  if (field != nullptr) return stageSettingsStringField(post, *field, rawValue);
  if (name == PARAM_TRANSIENT_DWELL) return stageTransientDwell(post, rawValue);
  if (name == PARAM_TRANSIENT_HOLD) return stageTransientHold(post, rawValue);
  if (name == PARAM_TEXT_PRIORITY) return stageTextPriority(post, rawValue);
  if (name == PARAM_DEVICE_NAME) return stageDeviceName(post, rawValue);
  if (name == PARAM_MQTT_PASSWORD) return stageMqttPassword(post, rawValue);
  return SettingsParamResult::Ignored;
}

// Cross-field rule (#176): a dwell belongs to a transientText in the SAME
// post — accepting a text-less dwell and silently dropping it would report
// a false ok.
inline bool settingsPostConsistent(const PendingSettingsPost& post) {
  if (post.transientDwellProvided && !post.transientTextProvided) return false;
  // #616: a hold belongs to a text shown over the mode, and not beside a dwell.
  if (post.transientHoldProvided && post.transientHold &&
      (!post.transientTextProvided || post.transientDwellProvided)) {
    return false;
  }
  // #625: a priority belongs to a text.
  if (post.textPriorityProvided && !post.transientTextProvided && !post.inputTextProvided) {
    return false;
  }
  return true;
}

// Verdict for the "ok-reboot" response (#128): identity and MQTT changes
// apply on the next reboot, everything else is live.
inline bool settingsPostNeedsReboot(const PendingSettingsPost& post,
                                    const MasterSettings& live) {
  if (post.deviceNameProvided && post.deviceName != live.deviceName) return true;
  if (post.mqttHostProvided && post.mqttHost != live.mqttHost) return true;
  if (post.mqttPortProvided && post.mqttPort != String(live.mqttPort)) return true;
  if (post.mqttUserProvided && post.mqttUser != live.mqttUser) return true;
  if (post.mqttPasswordProvided && post.mqttPassword != live.mqttPassword) return true;
  return false;
}

// Overlay `accepted` (a fully validated local post) onto the shared pending
// post. Later posts win on the same field; untouched fields survive.
inline void mergeSettingsPost(PendingSettingsPost& shared,
                              const PendingSettingsPost& accepted) {
  if (accepted.alignmentProvided)  { shared.alignment  = accepted.alignment;  shared.alignmentProvided  = true; }
  if (accepted.flapSpeedProvided)  { shared.flapSpeed  = accepted.flapSpeed;  shared.flapSpeedProvided  = true; }
  if (accepted.deviceModeProvided) { shared.deviceMode = accepted.deviceMode; shared.deviceModeProvided = true; }
  if (accepted.inputTextProvided)  { shared.inputText  = accepted.inputText;  shared.inputTextProvided  = true; }
  if (accepted.transientTextProvided) {
    shared.transientText = accepted.transientText;
    shared.transientDwell = accepted.transientDwell;
    shared.transientTextProvided = true;
    shared.transientDwellProvided = accepted.transientDwellProvided;
    shared.transientHold = accepted.transientHold;
    shared.transientHoldProvided = accepted.transientHoldProvided;
  }
  if (accepted.textPriorityProvided) { shared.textPriority = accepted.textPriority; shared.textPriorityProvided = true; }
  if (accepted.timezoneProvided)   { shared.timezone   = accepted.timezone;   shared.timezoneProvided   = true; }
  if (accepted.deviceNameProvided) { shared.deviceName = accepted.deviceName; shared.deviceNameProvided = true; }
  if (accepted.mqttHostProvided)   { shared.mqttHost   = accepted.mqttHost;   shared.mqttHostProvided   = true; }
  if (accepted.mqttPortProvided)   { shared.mqttPort   = accepted.mqttPort;   shared.mqttPortProvided   = true; }
  if (accepted.mqttUserProvided)   { shared.mqttUser   = accepted.mqttUser;   shared.mqttUserProvided   = true; }
  if (accepted.mqttPasswordProvided) { shared.mqttPassword = accepted.mqttPassword; shared.mqttPasswordProvided = true; }
  if (accepted.unitCountProvided)  { shared.unitCount  = accepted.unitCount;  shared.unitCountProvided  = true; }
  if (accepted.reflashOnBootProvided) { shared.reflashOnBoot = accepted.reflashOnBoot; shared.reflashOnBootProvided = true; }
  if (accepted.quietProvided)      { shared.quiet      = accepted.quiet;      shared.quietProvided      = true; }
  if (accepted.wifiPowerSaveProvided) { shared.wifiPowerSave = accepted.wifiPowerSave; shared.wifiPowerSaveProvided = true; }
  if (accepted.releaseCheckProvided) { shared.releaseCheck = accepted.releaseCheck; shared.releaseCheckProvided = true; }
  if (accepted.releaseChannelProvided) { shared.releaseChannel = accepted.releaseChannel; shared.releaseChannelProvided = true; }
  shared.pending = true;
}

// Reset for the next post; release the staged String heap while at it (the
// password in particular shouldn't linger).
inline void resetSettingsPost(PendingSettingsPost& post) {
  post = PendingSettingsPost();
}

// A "true"/"false" flag of the post: provided and changed updates the live
// bool and its NVS key.
inline void applySettingsFlag(bool provided, const String& text, bool& live,
                              SettingsStore& store, void (*save)(SettingsStore&, bool)) {
  if (!provided) return;
  const bool want = (text == "true");
  if (live == want) return;
  live = want;
  save(store, live);
}

inline void applySettingsStrings(const PendingSettingsPost& post, MasterSettings& settings,
                                 SettingsStore& store) {
  if (post.alignmentProvided && settings.alignment != post.alignment) {
    settings.alignment = post.alignment;
    saveAlignment(store, settings.alignment);
  }
  if (post.flapSpeedProvided && settings.flapSpeed != post.flapSpeed.toInt()) {
    settings.flapSpeed = post.flapSpeed.toInt();
    saveFlapSpeed(store, settings.flapSpeed);
  }
  if (post.deviceModeProvided && settings.deviceMode != post.deviceMode) {
    settings.deviceMode = post.deviceMode;
    saveDeviceMode(store, settings.deviceMode);
  }
  if (post.timezoneProvided && settings.timezonePosix != post.timezone) {
    settings.timezonePosix = post.timezone;
    saveTimezone(store, settings.timezonePosix);
  }
  if (post.deviceNameProvided && settings.deviceName != post.deviceName) {
    settings.deviceName = post.deviceName;
    saveDeviceName(store, settings.deviceName);
  }
  if (post.unitCountProvided &&
      settings.unitCountOverride != post.unitCount.toInt()) {
    settings.unitCountOverride = post.unitCount.toInt();
    saveUnitCountOverride(store, settings.unitCountOverride);
  }
}

inline void applySettingsMqtt(const PendingSettingsPost& post, MasterSettings& settings,
                              SettingsStore& store) {
  bool mqttChanged = false;
  if (post.mqttHostProvided && settings.mqttHost != post.mqttHost) {
    settings.mqttHost = post.mqttHost;
    mqttChanged = true;
  }
  if (post.mqttPortProvided && settings.mqttPort != post.mqttPort.toInt()) {
    settings.mqttPort = post.mqttPort.toInt();
    mqttChanged = true;
  }
  if (post.mqttUserProvided && settings.mqttUser != post.mqttUser) {
    settings.mqttUser = post.mqttUser;
    mqttChanged = true;
  }
  if (post.mqttPasswordProvided && settings.mqttPassword != post.mqttPassword) {
    settings.mqttPassword = post.mqttPassword;
    mqttChanged = true;
  }
  if (!mqttChanged) return;
  // Write-only password rule lives in saveMqttConfig(): it only writes the
  // password key when a non-empty one is passed.
  saveMqttConfig(store, settings.mqttHost, settings.mqttPort,
                 settings.mqttUser,
                 post.mqttPasswordProvided ? settings.mqttPassword : String());
}

// Drain the staged post into the live settings + store. Runs from loop()
// context only. Persistence mirrors v1's applyPendingSettingsPost(): each
// provided-and-changed field updates the live struct and its NVS key;
// unchanged values don't touch flash. Display-bound fields (inputText,
// transientText) are NOT consumed here — the caller reads them off the post
// before calling. Clears the post afterwards, secrets included.
inline void applySettingsPost(PendingSettingsPost& post,
                              MasterSettings& settings, SettingsStore& store) {
  applySettingsStrings(post, settings, store);
  applySettingsFlag(post.reflashOnBootProvided, post.reflashOnBoot, settings.reflashOnBoot,
                    store, saveReflashOnBoot);
  applySettingsFlag(post.wifiPowerSaveProvided, post.wifiPowerSave, settings.wifiPowerSave,
                    store, saveWifiPowerSave);
  applySettingsFlag(post.quietProvided, post.quiet, settings.quiet, store, saveQuiet);
  applySettingsFlag(post.releaseCheckProvided, post.releaseCheck, settings.releaseCheck,
                    store, saveReleaseCheck);
  if (post.releaseChannelProvided && settings.releaseChannel != post.releaseChannel) {
    settings.releaseChannel = post.releaseChannel;
    saveReleaseChannel(store, settings.releaseChannel);
  }
  applySettingsMqtt(post, settings, store);
  resetSettingsPost(post);
}
