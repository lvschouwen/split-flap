#pragma once
// SettingsJson.h — the /settings JSON builder (#186), v2 counterpart of
// v1's getCurrentSettingValues() (ServiceSettingsFunctions.ino).
//
// Same key set and value typing as v1 — one wire contract for /settings
// across both firmware generations (flapSpeed stays string-typed, the MQTT
// password is never present, only mqttPasswordSet). Pure: every value
// arrives through SettingsJsonFields, so the shape is natively testable and
// the endpoint layer owns the ESP.* / boot-state lookups.
//
// Hand-rolled to avoid an ArduinoJson dependency for a fixed-shape
// serializer that never needs to parse (v1 issue #40).

#include <Arduino.h>

struct SettingsJsonFields {
  // Bus/probe results. unitsAmount is the per-unit array length (the
  // UNITS_AMOUNT ceiling); null array pointers emit per-slot defaults
  // (status 0, empty version) until the I2C slice lands.
  int unitCount = 0;
  int detectedUnitCount = 0;
  const int* detectedUnitAddresses = nullptr;   // len detectedUnitCount
  int unitsAmount = 0;
  const int* detectedUnitVersionStatus = nullptr;  // len unitsAmount
  const String* detectedUnitVersions = nullptr;    // len unitsAmount

  String alignment;
  String flapSpeed;  // string-typed on the wire, v1 parity
  String deviceMode;
  String timezonePosix;
  String deviceName;           // raw stored value ("" = unset)
  String effectiveDeviceName;  // what the device actually uses right now

  String mqttHost;
  String mqttPort;
  String mqttUser;
  bool mqttPasswordSet = false;
  bool mqttConnected = false;

  String version;
  String sketchMd5;
  String lastFlashResult;
  String intendedVersion;
  bool otaReverted = false;
  String lastResetReason;
  uint32_t bootCounter = 0;
  bool recoveryMode = false;
  bool flashConfigMismatch = false;

  String lastTimeReceivedMessageDateTime;
  String lastWrittenText;
  bool isInOtaMode = false;
  bool wifiSettingsResettable = false;

  // Cluster membership (#272): drives the follower banner + card gating.
  // "standalone" = not clustered (the cluster-disabled default).
  String clusterState = "standalone";
  String clusterLeaderName;
  String clusterLeaderHost;
  int clusterRow = 0;
};

// JSON string escaper (v2 copy of v1's HelpersStringHandling.ino version).
static inline void appendJsonString(String& out, const String& value) {
  out += '"';
  for (unsigned int i = 0; i < value.length(); i++) {
    char c = value[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

static inline void appendJsonBool(String& out, bool value) {
  out += value ? "true" : "false";
}

inline String buildSettingsJson(const SettingsJsonFields& f) {
  String out;
  // Three per-unit arrays grow with unitsAmount — scale the reservation so
  // the response builds in one allocation (v1 #95).
  out.reserve(512 + f.unitsAmount * 24);
  out += '{';

  out += "\"unitCount\":";          out += f.unitCount;
  out += ",\"detectedUnitCount\":"; out += f.detectedUnitCount;

  out += ",\"detectedUnitAddresses\":[";
  for (int i = 0; i < f.detectedUnitCount; i++) {
    if (i) out += ',';
    out += f.detectedUnitAddresses ? f.detectedUnitAddresses[i] : 0;
  }
  out += ']';

  out += ",\"detectedUnitVersionStatus\":[";
  for (int i = 0; i < f.unitsAmount; i++) {
    if (i) out += ',';
    out += f.detectedUnitVersionStatus ? f.detectedUnitVersionStatus[i] : 0;
  }
  out += "],\"detectedUnitVersions\":[";
  for (int i = 0; i < f.unitsAmount; i++) {
    if (i) out += ',';
    appendJsonString(out, f.detectedUnitVersions ? f.detectedUnitVersions[i]
                                                 : String());
  }
  out += ']';

  out += ",\"alignment\":";           appendJsonString(out, f.alignment);
  out += ",\"flapSpeed\":";           appendJsonString(out, f.flapSpeed);
  out += ",\"deviceMode\":";          appendJsonString(out, f.deviceMode);
  out += ",\"timezonePosix\":";       appendJsonString(out, f.timezonePosix);
  out += ",\"deviceName\":";          appendJsonString(out, f.deviceName);
  out += ",\"effectiveDeviceName\":"; appendJsonString(out, f.effectiveDeviceName);
  out += ",\"mqttHost\":";            appendJsonString(out, f.mqttHost);
  out += ",\"mqttPort\":";            appendJsonString(out, f.mqttPort);
  out += ",\"mqttUser\":";            appendJsonString(out, f.mqttUser);
  out += ",\"mqttPasswordSet\":";     appendJsonBool(out, f.mqttPasswordSet);
  out += ",\"mqttConnected\":";       appendJsonBool(out, f.mqttConnected);
  out += ",\"version\":";             appendJsonString(out, f.version);
  out += ",\"sketchMd5\":";           appendJsonString(out, f.sketchMd5);
  out += ",\"lastFlashResult\":";     appendJsonString(out, f.lastFlashResult);
  out += ",\"intendedVersion\":";     appendJsonString(out, f.intendedVersion);
  out += ",\"otaReverted\":";         appendJsonBool(out, f.otaReverted);
  out += ",\"lastResetReason\":";     appendJsonString(out, f.lastResetReason);
  out += ",\"bootCounter\":";         out += String(f.bootCounter);
  out += ",\"recoveryMode\":";        appendJsonBool(out, f.recoveryMode);
  out += ",\"flashConfigMismatch\":"; appendJsonBool(out, f.flashConfigMismatch);
  out += ",\"lastTimeReceivedMessageDateTime\":";
  appendJsonString(out, f.lastTimeReceivedMessageDateTime);
  out += ",\"lastWrittenText\":";     appendJsonString(out, f.lastWrittenText);
  out += ",\"isInOtaMode\":";         appendJsonBool(out, f.isInOtaMode);
  out += ",\"wifiSettingsResettable\":";
  appendJsonBool(out, f.wifiSettingsResettable);
  out += ",\"clusterState\":";      appendJsonString(out, f.clusterState);
  out += ",\"clusterLeaderName\":"; appendJsonString(out, f.clusterLeaderName);
  out += ",\"clusterLeaderHost\":"; appendJsonString(out, f.clusterLeaderHost);
  out += ",\"clusterRow\":";        out += f.clusterRow;

  out += '}';
  return out;
}
