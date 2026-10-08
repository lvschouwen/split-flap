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

#include "JsonEscape.h"  // appendJsonString
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
  // #391 rescue slot: what the factory partition actually holds. rescueRev
  // is "" whenever the image cannot be identified — never a guess.
  String rescueRev;
  String rescueSlot;  // ok | absent | empty | unidentified | stale
  bool rescueSlotWarn = false;
  String lastResetReason;
  String lastRebootCause;  // #432: "" unless the last reset was a stamped deliberate reboot
  String bootTrace = "[]";  // #504: raw JSON array, oldest boot first
  String netLiveness = "{}";  // #501: raw JSON object, probe results + strikes
  String crashContext = "{}";  // #504: raw JSON, tasks at the last crash
  String bootGuard = "{}";  // #281: raw JSON, crashes in a row + rescue trips
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
  bool reflashOnBoot = true;  // #412 boot auto-install brake
  bool quiet = false;         // #227 quiet mode

  // Per-board vitals (#335), same keys/units as the ESP-01 row board's
  // /settings. `plat` is this board's platform tag (esp32s3), the S3
  // counterpart to the ESP-01's "esp01"; ota-flash.sh picks the image by it.
  uint32_t heapBytes = 0;
  int rssiDbm = 0;
  uint32_t upSeconds = 0;
  String plat = "esp32s3";
};


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
  out += ",\"rescueRev\":";           appendJsonString(out, f.rescueRev);
  out += ",\"rescueSlot\":";          appendJsonString(out, f.rescueSlot);
  out += ",\"rescueSlotWarn\":";      appendJsonBool(out, f.rescueSlotWarn);
  out += ",\"lastResetReason\":";     appendJsonString(out, f.lastResetReason);
  out += ",\"lastRebootCause\":";     appendJsonString(out, f.lastRebootCause);
  out += ",\"bootTrace\":";           out += f.bootTrace;
  out += ",\"netLiveness\":";         out += f.netLiveness;
  out += ",\"crashContext\":";        out += f.crashContext;
  out += ",\"bootGuard\":";           out += f.bootGuard;
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
  out += ",\"reflashOnBoot\":";     out += f.reflashOnBoot ? "true" : "false";
  out += ",\"quiet\":";             out += f.quiet ? "true" : "false";
  // #335 per-board vitals — same keys/units as the ESP-01 row board.
  out += ",\"heap\":";              out += String(f.heapBytes);
  out += ",\"rssi\":";              out += f.rssiDbm;
  out += ",\"up\":";                out += String(f.upSeconds);
  out += ",\"plat\":";              appendJsonString(out, f.plat);

  out += '}';
  return out;
}
