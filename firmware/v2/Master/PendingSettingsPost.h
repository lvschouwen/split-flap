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
#define PARAM_TIMEZONE        "timezone"
#define PARAM_DEVICE_NAME     "deviceName"
#define PARAM_MQTT_HOST       "mqttHost"
#define PARAM_MQTT_PORT       "mqttPort"
#define PARAM_MQTT_USER       "mqttUser"
#define PARAM_MQTT_PASSWORD   "mqttPassword"

struct PendingSettingsPost {
  bool pending = false;

  String alignment;     bool alignmentProvided = false;
  String flapSpeed;     bool flapSpeedProvided = false;
  String deviceMode;    bool deviceModeProvided = false;
  String inputText;     bool inputTextProvided = false;
  String transientText; bool transientTextProvided = false;
  long transientDwell = 0;
  bool transientDwellProvided = false;
  String timezone;      bool timezoneProvided = false;
  String deviceName;    bool deviceNameProvided = false;
  String mqttHost;      bool mqttHostProvided = false;
  String mqttPort;      bool mqttPortProvided = false;
  String mqttUser;      bool mqttUserProvided = false;
  String mqttPassword;  bool mqttPasswordProvided = false;
};

enum class SettingsParamResult {
  Ignored,   // not a known settings field (or empty password = keep stored)
  Accepted,  // validated, normalized and staged
  Invalid,   // known field, failed validation — reject the whole POST
};

// Validate one POST parameter and stage it into `post`. Normalization
// mirrors v1: MQTT host/user/port and the dwell are trimmed before
// validation, the device name is lowercased via normalizeDeviceName().
inline SettingsParamResult stageSettingsParam(PendingSettingsPost& post,
                                              const String& name,
                                              const String& rawValue) {
  if (name == PARAM_ALIGNMENT) {
    if (!isValidAlignmentValue(rawValue)) return SettingsParamResult::Invalid;
    post.alignment = rawValue;
    post.alignmentProvided = true;
    return SettingsParamResult::Accepted;
  }

  if (name == PARAM_FLAP_SPEED) {
    if (!isValidFlapSpeedValue(rawValue)) return SettingsParamResult::Invalid;
    post.flapSpeed = rawValue;
    post.flapSpeedProvided = true;
    return SettingsParamResult::Accepted;
  }

  if (name == PARAM_DEVICEMODE) {
    if (!isValidDeviceModeValue(rawValue)) return SettingsParamResult::Invalid;
    post.deviceMode = rawValue;
    post.deviceModeProvided = true;
    return SettingsParamResult::Accepted;
  }

  if (name == PARAM_INPUT_TEXT) {
    post.inputText = rawValue;
    post.inputTextProvided = true;
    return SettingsParamResult::Accepted;
  }

  if (name == PARAM_TRANSIENT_TEXT) {
    post.transientText = rawValue;
    post.transientTextProvided = true;
    return SettingsParamResult::Accepted;
  }

  if (name == PARAM_TRANSIENT_DWELL) {
    String trimmed = rawValue;
    trimmed.trim();
    if (!isValidTransientDwellValue(trimmed)) return SettingsParamResult::Invalid;
    post.transientDwell = trimmed.toInt();
    post.transientDwellProvided = true;
    return SettingsParamResult::Accepted;
  }

  if (name == PARAM_TIMEZONE) {
    if (!isValidTimezoneValue(rawValue, LEN_TIMEZONE)) {
      return SettingsParamResult::Invalid;
    }
    post.timezone = rawValue;
    post.timezoneProvided = true;
    return SettingsParamResult::Accepted;
  }

  if (name == PARAM_DEVICE_NAME) {
    String normalized = normalizeDeviceName(rawValue);
    // Empty is valid and means "reset to the chip-id default" (#125).
    if (normalized.length() > 0 && !isValidDeviceName(normalized)) {
      return SettingsParamResult::Invalid;
    }
    post.deviceName = normalized;
    post.deviceNameProvided = true;
    return SettingsParamResult::Accepted;
  }

  if (name == PARAM_MQTT_HOST || name == PARAM_MQTT_USER) {
    bool isHost = name == PARAM_MQTT_HOST;
    String trimmed = rawValue;
    trimmed.trim();
    if (!isValidMqttHostOrUserValue(trimmed,
                                    isHost ? LEN_MQTT_HOST : LEN_MQTT_USER)) {
      return SettingsParamResult::Invalid;
    }
    if (isHost) { post.mqttHost = trimmed; post.mqttHostProvided = true; }
    else        { post.mqttUser = trimmed; post.mqttUserProvided = true; }
    return SettingsParamResult::Accepted;
  }

  if (name == PARAM_MQTT_PORT) {
    String trimmed = rawValue;
    trimmed.trim();
    if (!isValidMqttPortValue(trimmed, LEN_MQTT_PORT)) {
      return SettingsParamResult::Invalid;
    }
    post.mqttPort = trimmed;
    post.mqttPortProvided = true;
    return SettingsParamResult::Accepted;
  }

  if (name == PARAM_MQTT_PASSWORD) {
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

  return SettingsParamResult::Ignored;
}

// Cross-field rule (#176): a dwell belongs to a transientText in the SAME
// post — accepting a text-less dwell and silently dropping it would report
// a false ok.
inline bool settingsPostConsistent(const PendingSettingsPost& post) {
  return !(post.transientDwellProvided && !post.transientTextProvided);
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
  }
  if (accepted.timezoneProvided)   { shared.timezone   = accepted.timezone;   shared.timezoneProvided   = true; }
  if (accepted.deviceNameProvided) { shared.deviceName = accepted.deviceName; shared.deviceNameProvided = true; }
  if (accepted.mqttHostProvided)   { shared.mqttHost   = accepted.mqttHost;   shared.mqttHostProvided   = true; }
  if (accepted.mqttPortProvided)   { shared.mqttPort   = accepted.mqttPort;   shared.mqttPortProvided   = true; }
  if (accepted.mqttUserProvided)   { shared.mqttUser   = accepted.mqttUser;   shared.mqttUserProvided   = true; }
  if (accepted.mqttPasswordProvided) { shared.mqttPassword = accepted.mqttPassword; shared.mqttPasswordProvided = true; }
  shared.pending = true;
}

// Reset for the next post; release the staged String heap while at it (the
// password in particular shouldn't linger).
inline void resetSettingsPost(PendingSettingsPost& post) {
  post = PendingSettingsPost();
}

// Drain the staged post into the live settings + store. Runs from loop()
// context only. Persistence mirrors v1's applyPendingSettingsPost(): each
// provided-and-changed field updates the live struct and its NVS key;
// unchanged values don't touch flash. Display-bound fields (inputText,
// transientText) are NOT consumed here — the caller reads them off the post
// before calling. Clears the post afterwards, secrets included.
inline void applySettingsPost(PendingSettingsPost& post,
                              MasterSettings& settings, SettingsStore& store) {
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
  if (mqttChanged) {
    // Write-only password rule lives in saveMqttConfig(): it only writes the
    // password key when a non-empty one is passed.
    saveMqttConfig(store, settings.mqttHost, settings.mqttPort,
                   settings.mqttUser,
                   post.mqttPasswordProvided ? settings.mqttPassword : String());
  }

  resetSettingsPost(post);
}
