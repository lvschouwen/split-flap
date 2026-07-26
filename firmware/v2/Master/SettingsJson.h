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

#include "HeadlessPolicy.h"

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
  // #329 headless mode: the stored role + the auto-detect nudge. "display"
  // (default) keeps old UIs unaffected; headlessSuggested drives the "you
  // look unit-less — pick a role" banner (detection only ever suggests).
  String deviceRole = DEVICE_ROLE_DISPLAY;
  bool headlessSuggested = false;
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
  // #391 rescue slot: what the factory partition actually holds. rescueRev
  // is "" whenever the image cannot be identified — never a guess.
  String rescueRev;
  String rescueSlot;  // ok | absent | empty | unidentified | stale
  bool rescueSlotWarn = false;
  String lastResetReason;
  uint32_t bootCounter = 0;
  bool recoveryMode = false;
  bool flashConfigMismatch = false;

  String lastTimeReceivedMessageDateTime;
  String lastWrittenText;
  bool isInOtaMode = false;
  bool wifiSettingsResettable = false;

  // #289 dummy mode: the stored override (0 = auto), distinct from the
  // effective width already carried by unitCount.
  int unitCountOverride = 0;

  // Per-board vitals (#335) so cluster members surface heap/rssi/uptime in
  // the System-tab panel — same keys/units as the ESP-01 follower's #297
  // block. `plat` is this board's platform tag (esp32s3), the S3 counterpart
  // to the ESP-01's "esp01"; the member UI keys the model label off it.
  uint32_t heapBytes = 0;
  int rssiDbm = 0;
  uint32_t upSeconds = 0;
  String plat = "esp32s3";  // mirrors ClusterLeaderPolicy.h CLUSTER_LEADER_PLAT

  // Cluster membership (#272): drives the follower banner + card gating.
  // "standalone" = not clustered (the cluster-disabled default).
  String clusterState = "standalone";
  String clusterLeaderName;
  String clusterLeaderHost;
  int clusterRow = 0;
  // Leader side (#277): the browser's wall mirror collapses on the poll
  // when this drops and the SSE stream missed the transition.
  bool clusterLeading = false;
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
  out += ",\"deviceRole\":";          appendJsonString(out, f.deviceRole);
  out += ",\"headlessSuggested\":";   appendJsonBool(out, f.headlessSuggested);
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
  out += ",\"rescueRev\":";           appendJsonString(out, f.rescueRev);
  out += ",\"rescueSlot\":";          appendJsonString(out, f.rescueSlot);
  out += ",\"rescueSlotWarn\":";      appendJsonBool(out, f.rescueSlotWarn);
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
  out += ",\"unitCountOverride\":"; out += f.unitCountOverride;
  out += ",\"clusterState\":";      appendJsonString(out, f.clusterState);
  out += ",\"clusterLeaderName\":"; appendJsonString(out, f.clusterLeaderName);
  out += ",\"clusterLeaderHost\":"; appendJsonString(out, f.clusterLeaderHost);
  out += ",\"clusterRow\":";        out += f.clusterRow;
  out += ",\"clusterLeading\":";    appendJsonBool(out, f.clusterLeading);
  // #335 per-board vitals — same keys/units as the ESP-01 follower so the
  // cluster member panel renders S3 rows identically.
  out += ",\"heap\":";              out += String(f.heapBytes);
  out += ",\"rssi\":";              out += f.rssiDbm;
  out += ",\"up\":";                out += String(f.upSeconds);
  out += ",\"plat\":";              appendJsonString(out, f.plat);

  out += '}';
  return out;
}
