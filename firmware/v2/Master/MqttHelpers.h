#pragma once

#include <Arduino.h>

#include "UnitHealth.h"  // UnitFacts/UnitStatus for unitFleetRebootTotal (#368)

// Pure MQTT logic for the Home Assistant integration (issue #121): payload
// parsing, show-then-revert notification state, and topic/discovery/telemetry
// JSON assembly. No networking and no AsyncMqttClient types — everything in
// this header is exercised by `pio test -e native` (test_mqtt_helpers).
//
// Format strings live in PROGMEM on the device (PSTR/snprintf_P). The native
// test env has no pgmspace, so UNIT_TEST builds fall back to plain snprintf.
#ifdef UNIT_TEST
  #include <cstdio>
  #define MQTT_FMT(s) (s)
  #define mqttSnprintf snprintf
#else
  #define MQTT_FMT(s) PSTR(s)
  #define mqttSnprintf snprintf_P
#endif

// Default dwell for a notification without a JSON "dwell" override. The
// sketch can override via -D before including this header.
#ifndef MQTT_TEXT_DWELL_S
#define MQTT_TEXT_DWELL_S 60
#endif
#define MQTT_DWELL_MIN_S  5
#define MQTT_DWELL_MAX_S  3600

inline long clampDwellSeconds(long dwellSeconds) {
  if (dwellSeconds < MQTT_DWELL_MIN_S) return MQTT_DWELL_MIN_S;
  if (dwellSeconds > MQTT_DWELL_MAX_S) return MQTT_DWELL_MAX_S;
  return dwellSeconds;
}

// Minimal parser for {"text":"…","dwell":N}. Hand-rolled on purpose — the
// design doc rejects ArduinoJson (~30 KB flash) for a two-field object.
// Returns true iff `payload` is a JSON object with a string "text" member;
// textOut/dwellSecondsOut are written only on success. "dwell" is optional
// (default MQTT_TEXT_DWELL_S) and clamped. Escapes: \" \\ \n are decoded;
// any other \x yields the literal x. On false the caller treats the whole
// payload as plain text.
inline bool parseMqttTextPayload(const String& payload, String& textOut, long& dwellSecondsOut) {
  String trimmed = payload;
  trimmed.trim();
  if (!trimmed.startsWith("{") || !trimmed.endsWith("}")) return false;

  int keyIndex = trimmed.indexOf("\"text\"");
  if (keyIndex < 0) return false;
  int colonIndex = trimmed.indexOf(':', keyIndex + 6);
  if (colonIndex < 0) return false;
  unsigned int cursor = colonIndex + 1;
  while (cursor < trimmed.length() && (trimmed[cursor] == ' ' || trimmed[cursor] == '\t')) cursor++;
  if (cursor >= trimmed.length() || trimmed[cursor] != '"') return false;

  String text;
  // One-shot reservation: worst case the value runs to the end of the
  // payload. Avoids per-character realloc churn on the ~42 KB ESP-01 heap.
  text.reserve(trimmed.length() - cursor);
  bool closed = false;
  for (unsigned int i = cursor + 1; i < trimmed.length(); i++) {
    char c = trimmed[i];
    if (c == '\\' && i + 1 < trimmed.length()) {
      char next = trimmed[++i];
      if (next == 'n') text += '\n';
      else text += next;
      continue;
    }
    if (c == '"') { closed = true; break; }
    text += c;
  }
  if (!closed) return false;

  long dwellSeconds = MQTT_TEXT_DWELL_S;
  int dwellIndex = trimmed.indexOf("\"dwell\"");
  if (dwellIndex >= 0) {
    int dwellColon = trimmed.indexOf(':', dwellIndex + 7);
    if (dwellColon >= 0) {
      // toInt() skips leading whitespace and stops at the first non-digit.
      long parsed = trimmed.substring(dwellColon + 1).toInt();
      if (parsed > 0) dwellSeconds = parsed;
    }
  }

  textOut = text;
  dwellSecondsOut = clampDwellSeconds(dwellSeconds);
  return true;
}

// Show-then-revert notification state. The main loop stays declarative: it
// asks notificationTick() each pass — true means "the notification owns the
// display, show n.text"; false means normal deviceMode content. Expiry
// clears `active` in place, and the loop's existing lastWrittenText
// comparison re-flaps the previous clock/text content by itself — no saved
// mode, no EEPROM, no RTC.
//
// Times are uint32_t on purpose: millis() wraps at 2^32 on the ESP8266
// (32-bit unsigned long), while the native test host has a 64-bit unsigned
// long — fixed-width types make the wraparound arithmetic identical in
// both environments.
struct MqttNotification {
  bool active = false;
  String text;
  uint32_t deadlineMs = 0;
};

inline void notificationStart(MqttNotification& n, const String& text, long dwellSeconds, uint32_t nowMs) {
  n.active = true;
  n.text = text;
  n.deadlineMs = nowMs + (uint32_t)clampDwellSeconds(dwellSeconds) * 1000UL;
}

// Explicit mode change cancels an active notification (#130): a user who
// just switched text/clock wants that NOW, not after the dwell runs out.
inline void notificationCancel(MqttNotification& n) {
  n.active = false;
  n.text = "";
}

// Web transient text (#165/#176): calibration patterns and timed messages
// ride the same show-then-revert state instead of persisting a mode change,
// so a clock display resumes clocking by itself and nothing hits EEPROM.
// dwellSeconds <= 0 means "not provided" -> the 600 s default (a full
// calibration expect/reality/apply round). An explicit mode change (web UI
// or HA) cancels early via the #130 path.
#define TRANSIENT_TEXT_DEFAULT_DWELL_SECONDS 600L

inline void transientTextStart(MqttNotification& n, const String& text, long dwellSeconds, uint32_t nowMs) {
  notificationStart(n, text, dwellSeconds > 0 ? dwellSeconds : TRANSIENT_TEXT_DEFAULT_DWELL_SECONDS, nowMs);
}

// millis()-wraparound-safe via signed difference of fixed-width unsigned.
inline bool notificationTick(MqttNotification& n, uint32_t nowMs) {
  if (!n.active) return false;
  if ((int32_t)(nowMs - n.deadlineMs) >= 0) {
    n.active = false;
    n.text = "";
    return false;
  }
  return true;
}

// ---- Mode command (#130) ----
// HA's select publishes its option strings verbatim; accept exactly those
// (after a whitespace trim) and return "" for anything else — invalid
// payloads are ignored, never coerced.
inline String parseModeCommand(const String& payload) {
  String trimmed = payload;
  trimmed.trim();
  if (trimmed == "text" || trimmed == "clock") return trimmed;
  return String("");
}

// ---- Speed / alignment / restart commands (#132 Stage B) ----
// Flap-speed number: HA publishes the integer as a string. Accept 1..100 (the
// stored flapSpeed range, matching the web slider); -1 means ignore. Digit-only
// so a float ("80.0") is rejected, same as the web handler's isNumber() gate.
inline int parseSpeedCommand(const String& payload) {
  String trimmed = payload;
  trimmed.trim();
  if (trimmed.length() == 0) return -1;
  for (unsigned int i = 0; i < trimmed.length(); i++) {
    if (trimmed[i] < '0' || trimmed[i] > '9') return -1;
  }
  long v = trimmed.toInt();
  if (v < 1 || v > 100) return -1;
  return (int)v;
}

// Alignment select: accept exactly the option strings (post-trim), "" = ignore.
inline String parseAlignmentCommand(const String& payload) {
  String trimmed = payload;
  trimmed.trim();
  if (trimmed == "left" || trimmed == "center" || trimmed == "right") return trimmed;
  return String("");
}

// Restart button: HA publishes its payload_press verbatim. Only "PRESS" fires.
inline bool parseRestartCommand(const String& payload) {
  String trimmed = payload;
  trimmed.trim();
  return trimmed == "PRESS";
}

// ---- Topics ----
inline String mqttTopic(const String& deviceId, const char* suffix) {
  return String("splitflap/") + deviceId + "/" + suffix;
}

// ---- Telemetry ----
// One JSON packet feeds all telemetry-backed HA sensors via value_json
// templates. CONTINUOUS health metrics only — discrete/interactive state
// (current text, mode, alignment, speed, notification, display width) and the
// static diagnostics (ip/ssid/reset/boots/tz/otaReverted) ride their own
// retained topics, published on-change / on-connect (ServiceMqttFunctions.ino)
// so HA sees them instantly instead of on this periodic tick (#132).
// Fleet-wide reboot odometer (#368): sum of lifetime brownout + watchdog
// reset counts across every unit we hold a valid CMD_GET_STATUS read for —
// mirrors unitFleetVccMin's statusValid gate (UnitHealth.h). Unlike vccMin,
// 0 is a real "no reboots since boot" reading, not a sentinel — the caller
// always emits it. MQTT-only derived stat (no /units/health consumer), so it
// lives here rather than in the copied UnitHealth.h.
inline uint32_t unitFleetRebootTotal(const UnitFacts* units, int width) {
  uint32_t total = 0;
  for (int i = 0; i < width; i++) {
    if (units[i].statusValid) {
      total += (uint32_t)units[i].status.lifetimeBrownoutCount +
               (uint32_t)units[i].status.lifetimeWatchdogCount;
    }
  }
  return total;
}

// Fleet jam/stall problem (#365): true when ANY unit we hold a
// valid CMD_GET_EXT_DIAG read for flagged EXT_DIAG_STATUS_STALL on its last
// move. Gates on extDiagValid like unitFleetVccMin gates on vitalsValid — a
// unit that hasn't reported ext-diag (old firmware, or a read this epoch
// failed) contributes no jam. MQTT-only derived stat (no /units/health
// consumer — the per-unit "sb" field already carries this), so it lives
// here rather than in the copied UnitHealth.h, same as unitFleetRebootTotal.
inline bool unitFleetAnyJam(const UnitFacts* units, int width) {
  for (int i = 0; i < width; i++) {
    if (units[i].extDiagValid &&
        (units[i].extDiag.statusBits & EXT_DIAG_STATUS_STALL)) {
      return true;
    }
  }
  return false;
}

// Fleet worst-seen home-step excess (#365): the largest since-boot
// stepExcessMax (the drag/binding alarm) across every extDiagValid unit, or 0
// when none report ext-diag. Unlike vccMin, 0 is also the real "no excess
// seen" reading for a reporting unit — no sentinel ambiguity, so the caller
// always emits it (same contract as unitFleetRebootTotal).
inline uint16_t unitFleetMaxExcess(const UnitFacts* units, int width) {
  uint16_t worst = 0;
  for (int i = 0; i < width; i++) {
    if (units[i].extDiagValid && units[i].extDiag.stepExcessMax > worst) {
      worst = units[i].extDiag.stepExcessMax;
    }
  }
  return worst;
}

inline size_t buildTelemetryPayload(char* buf, size_t bufLen, uint32_t freeHeap, int heapFragPct,
                                    long rssi, int unitErrors, unsigned long uptimeSec,
                                    bool ntpSynced, uint16_t vccMin_mV, uint32_t rebootTotal,
                                    bool anyJam, uint16_t maxExcess) {
  size_t o = (size_t)mqttSnprintf(buf, bufLen,
      MQTT_FMT("{\"heap\":%lu,\"heapFrag\":%d,\"rssi\":%ld,\"unitErrors\":%d,\"uptime\":%lu,\"ntp\":%d"),
      (unsigned long)freeHeap, heapFragPct, rssi, unitErrors, uptimeSec, ntpSynced ? 1 : 0);
  // Fleet-min supply Vcc (#366): omitted when no unit reports vitals (value 0)
  // so the HA sensor reads unavailable instead of a phantom 0 mV.
  if (vccMin_mV > 0 && o < bufLen)
    o += (size_t)mqttSnprintf(buf + o, bufLen - o, MQTT_FMT(",\"vccMin\":%u"), (unsigned)vccMin_mV);
  // Fleet reboot odometer (#368): always emitted, see unitFleetRebootTotal.
  if (o < bufLen)
    o += (size_t)mqttSnprintf(buf + o, bufLen - o, MQTT_FMT(",\"reboots\":%u"), (unsigned)rebootTotal);
  // Fleet jam/stall problem (#365): always emitted — OFF is a real
  // "no stall seen" reading, not a sentinel. Quoted string so the HA
  // binary_sensor's val_tpl renders the same "ON"/"OFF" token its pl_on/pl_off
  // expect (matching DISCOVERY_UNIT_WEAR's convention).
  if (o < bufLen)
    o += (size_t)mqttSnprintf(buf + o, bufLen - o, MQTT_FMT(",\"jam\":\"%s\""), anyJam ? "ON" : "OFF");
  // Fleet worst-seen home-step excess (#365): always emitted, see
  // unitFleetMaxExcess.
  if (o < bufLen)
    o += (size_t)mqttSnprintf(buf + o, bufLen - o, MQTT_FMT(",\"stepsExcess\":%u"), (unsigned)maxExcess);
  if (o < bufLen) o += (size_t)mqttSnprintf(buf + o, bufLen - o, MQTT_FMT("}"));
  return o;
}

// ---- HA MQTT Discovery ----
// A Text + Mode-select command entity plus a fleet of read-only sensors, all
// sharing one device block so HA groups them under one device. Abbreviated
// keys (cmd_t, avty_t, uniq_id, stat_t, val_tpl, dev_cla, unit_of_meas,
// ent_cat, pl_on, pl_off, dev, ids, mf, mdl, sw) are the documented HA
// discovery short names — they keep payloads well inside the 512-byte buffer.
//
// Enum values are append-only: HA keys retained configs by the topic's
// object_id, so reordering would orphan entities. Keep DISCOVERY_ENTITY_COUNT
// last.
enum MqttDiscoveryEntity {
  DISCOVERY_TEXT = 0,
  DISCOVERY_HEAP,
  DISCOVERY_RSSI,
  DISCOVERY_UNIT_ERRORS,
  DISCOVERY_MODE,          // HA select, text/clock (#130)
  // Stage A read-only sensors (#132)
  DISCOVERY_HEAP_FRAG,     // telemetry-backed
  DISCOVERY_UPTIME,        // telemetry-backed
  DISCOVERY_NTP,           // telemetry-backed, binary
  DISCOVERY_CURRENT_TEXT,  // own retained topic
  DISCOVERY_NOTIFICATION,  // own retained topic, binary
  DISCOVERY_WIDTH,         // own retained topic
  DISCOVERY_UNITS,         // own retained topic
  DISCOVERY_IP,            // diagnostics topic
  DISCOVERY_SSID,          // diagnostics topic
  DISCOVERY_RESET,         // diagnostics topic
  DISCOVERY_BOOTS,         // diagnostics topic
  DISCOVERY_OTA_REVERTED,  // diagnostics topic, binary problem
  DISCOVERY_TIMEZONE,      // diagnostics topic
  // Stage B controls (#132) — command/write entities
  DISCOVERY_SPEED,         // HA number, speed/set + speed
  DISCOVERY_ALIGNMENT,     // HA select, alignment/set + alignment
  DISCOVERY_RESTART,       // HA button, restart/set
  // Per-unit health (#137) — state = integer faulty count for alerting;
  // json_attributes_topic carries the full per-unit breakdown for drill-down.
  DISCOVERY_UNITS_FAULTY,
  // Wear warning (#231) — binary problem sensor, ON when any unit wears past
  // the relative threshold (WearPolicy.h); median + flagged list ride the
  // units/wear attrs topic.
  DISCOVERY_UNIT_WEAR,
  // Fleet-min supply Vcc (#366) — diagnostic voltage sensor, the lowest
  // since-boot vccMin any unit reports (the brownout smoking gun). Rides the
  // periodic telemetry packet; the per-unit vmin lives in units/attrs.
  DISCOVERY_VCC_MIN,
  // Fleet reboot odometer (#368) — diagnostic sensor, sum of lifetime
  // brownout+watchdog resets across every unit we hold a valid status read
  // for (unitFleetRebootTotal). A rising counter — HA's total_increasing
  // state_class would be the right fit, but buildEntityDiscovery has no
  // state_class slot, so it's omitted here.
  DISCOVERY_REBOOT_TOTAL,
  // Fleet jam/stall problem (#365) — binary problem sensor, ON when
  // any extDiagValid unit's last move set EXT_DIAG_STATUS_STALL
  // (unitFleetAnyJam). Rides the periodic telemetry packet like vccMin/reboots.
  DISCOVERY_UNIT_JAM,
  // Fleet worst-seen home-step excess (#365) — diagnostic sensor,
  // the largest stepExcessMax any extDiagValid unit reports
  // (unitFleetMaxExcess); the drag/binding alarm CMD_GET_EXT_DIAG exists for.
  DISCOVERY_STEPS_EXCESS,
  DISCOVERY_ENTITY_COUNT
};

// Per-entity HA component + object-id suffix. Single source of truth so the
// config topic and the payload's uniq_id can never drift apart.
struct MqttDiscMeta { const char* comp; const char* obj; };
inline MqttDiscMeta mqttDiscMeta(int entity) {
  switch (entity) {
    case DISCOVERY_TEXT:         return { "text",          "" };
    case DISCOVERY_MODE:         return { "select",        "" };
    case DISCOVERY_HEAP:         return { "sensor",        "_heap" };
    case DISCOVERY_RSSI:         return { "sensor",        "_rssi" };
    case DISCOVERY_UNIT_ERRORS:  return { "sensor",        "_unit_errors" };
    case DISCOVERY_HEAP_FRAG:    return { "sensor",        "_heap_frag" };
    case DISCOVERY_UPTIME:       return { "sensor",        "_uptime" };
    case DISCOVERY_NTP:          return { "binary_sensor", "_ntp" };
    case DISCOVERY_CURRENT_TEXT: return { "sensor",        "_text_state" };
    case DISCOVERY_NOTIFICATION: return { "binary_sensor", "_notification" };
    case DISCOVERY_WIDTH:        return { "sensor",        "_width" };
    case DISCOVERY_UNITS:        return { "sensor",        "_units" };
    case DISCOVERY_IP:           return { "sensor",        "_ip" };
    case DISCOVERY_SSID:         return { "sensor",        "_ssid" };
    case DISCOVERY_RESET:        return { "sensor",        "_reset" };
    case DISCOVERY_BOOTS:        return { "sensor",        "_boots" };
    case DISCOVERY_OTA_REVERTED: return { "binary_sensor", "_ota_reverted" };
    case DISCOVERY_TIMEZONE:     return { "sensor",        "_timezone" };
    case DISCOVERY_SPEED:        return { "number",        "_speed" };
    case DISCOVERY_ALIGNMENT:    return { "select",        "_alignment" };
    case DISCOVERY_RESTART:      return { "button",        "_restart" };
    case DISCOVERY_UNITS_FAULTY: return { "sensor",        "_units_faulty" };
    case DISCOVERY_UNIT_WEAR:    return { "binary_sensor", "_unit_wear" };
    case DISCOVERY_VCC_MIN:      return { "sensor",        "_vcc_min" };
    case DISCOVERY_REBOOT_TOTAL: return { "sensor",        "_reboot_total" };
    case DISCOVERY_UNIT_JAM:     return { "binary_sensor", "_unit_jam" };
    case DISCOVERY_STEPS_EXCESS: return { "sensor",        "_steps_excess" };
  }
  return { nullptr, nullptr };
}

// TEXT and MODE keep an empty object suffix (their pre-#132 topics) — no
// collision, since HA keys the config topic by component too (text/ vs
// select/). Their uniq_id still carries the _text/_mode suffix in the explicit
// payload cases below. All new sensors use a distinct _<key> suffix.

inline size_t buildDiscoveryTopic(char* buf, size_t bufLen, int entity, const char* deviceId) {
  MqttDiscMeta m = mqttDiscMeta(entity);
  if (!m.comp) { if (bufLen > 0) buf[0] = '\0'; return 0; }
  return (size_t)mqttSnprintf(buf, bufLen, MQTT_FMT("homeassistant/%s/%s%s/config"), m.comp, deviceId, m.obj);
}

// Shared device block; %s slots are (deviceId, deviceId, fwVersion).
// The model string in HA's device registry defaults to the v1 label; the v2
// master overrides it via build flag (-D MQTT_DEVICE_MODEL=...) so this
// header stays a byte-identical copy in both trees.
#ifndef MQTT_DEVICE_MODEL
#define MQTT_DEVICE_MODEL "v1 ESPMaster"
#endif
#define MQTT_DEVICE_BLOCK "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Split-Flap %s\",\"mf\":\"split-flap\",\"mdl\":\"" MQTT_DEVICE_MODEL "\",\"sw\":\"%s\"}"

// Append-with-guard: bail the moment the buffer is full so buf+o never runs
// past the end. The caller rejects any payload with returned length >= bufLen.
#define MQTT_DISC_APPEND(...) do { \
    if (o >= bufLen) return o; \
    o += (size_t)mqttSnprintf(buf + o, bufLen - o, __VA_ARGS__); \
  } while (0)

// Generic read-only entity (sensor / binary_sensor). Optional fragments are
// emitted only when non-null. statSuffix is the topic under splitflap/<id>/.
// For binary_sensor pass pOn/pOff; for a numeric/string sensor leave them null.
inline size_t buildEntityDiscovery(char* buf, size_t bufLen, const char* deviceId, const char* fw,
    const char* objSuffix, const char* name, const char* statSuffix, const char* valTpl,
    const char* devCla, const char* unit, const char* entCat, const char* pOn, const char* pOff) {
  size_t o = 0;
  MQTT_DISC_APPEND(MQTT_FMT("{\"name\":\"%s\",\"stat_t\":\"splitflap/%s/%s\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s%s\""),
                   name, deviceId, statSuffix, deviceId, deviceId, objSuffix);
  if (valTpl) MQTT_DISC_APPEND(MQTT_FMT(",\"val_tpl\":\"%s\""), valTpl);
  if (devCla) MQTT_DISC_APPEND(MQTT_FMT(",\"dev_cla\":\"%s\""), devCla);
  if (unit)   MQTT_DISC_APPEND(MQTT_FMT(",\"unit_of_meas\":\"%s\""), unit);
  if (entCat) MQTT_DISC_APPEND(MQTT_FMT(",\"ent_cat\":\"%s\""), entCat);
  if (pOn)    MQTT_DISC_APPEND(MQTT_FMT(",\"pl_on\":\"%s\",\"pl_off\":\"%s\""), pOn, pOff);
  MQTT_DISC_APPEND(MQTT_FMT("," MQTT_DEVICE_BLOCK "}"), deviceId, deviceId, fw);
  return o;
}

inline size_t buildDiscoveryPayload(char* buf, size_t bufLen, int entity, const char* deviceId, const char* fwVersion) {
  switch (entity) {
    case DISCOVERY_TEXT:
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Text\",\"cmd_t\":\"splitflap/%s/text/set\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_text\",\"max\":255," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion);
    case DISCOVERY_MODE:
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Mode\",\"cmd_t\":\"splitflap/%s/mode/set\",\"stat_t\":\"splitflap/%s/mode\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_mode\",\"ops\":[\"text\",\"clock\"]," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion);
    //                        obj             name                 stat_t          val_tpl                        dev_cla       unit    ent_cat        pOn    pOff
    case DISCOVERY_HEAP:         return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_heap",         "Free heap",           "telemetry",   "{{ value_json.heap }}",      nullptr,       "B",   nullptr,      nullptr, nullptr);
    case DISCOVERY_RSSI:         return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_rssi",         "WiFi RSSI",           "telemetry",   "{{ value_json.rssi }}",      "signal_strength", "dBm", nullptr,  nullptr, nullptr);
    case DISCOVERY_UNIT_ERRORS:  return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_unit_errors",  "Unit errors",         "telemetry",   "{{ value_json.unitErrors }}", nullptr,      nullptr, nullptr,     nullptr, nullptr);
    case DISCOVERY_HEAP_FRAG:    return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_heap_frag",    "Heap fragmentation",  "telemetry",   "{{ value_json.heapFrag }}",  nullptr,       "%",   "diagnostic", nullptr, nullptr);
    case DISCOVERY_UPTIME:       return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_uptime",       "Uptime",              "telemetry",   "{{ value_json.uptime }}",    "duration",    "s",   "diagnostic", nullptr, nullptr);
    case DISCOVERY_NTP:          return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_ntp",          "NTP synced",          "telemetry",   "{{ value_json.ntp }}",       "connectivity", nullptr, "diagnostic", "1", "0");
    case DISCOVERY_CURRENT_TEXT: return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_text_state",   "Current text",        "text/state",  nullptr,                      nullptr,       nullptr, nullptr,     nullptr, nullptr);
    case DISCOVERY_NOTIFICATION: return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_notification", "Notification active", "notification", nullptr,                     nullptr,       nullptr, nullptr,     "ON", "OFF");
    case DISCOVERY_WIDTH:        return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_width",        "Display width",       "width",       nullptr,                      nullptr,       nullptr, "diagnostic", nullptr, nullptr);
    case DISCOVERY_UNITS:        return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_units",        "Units responding",    "units",       nullptr,                      nullptr,       nullptr, "diagnostic", nullptr, nullptr);
    case DISCOVERY_IP:           return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_ip",           "IP address",          "diag/ip",     nullptr,                      nullptr,       nullptr, "diagnostic", nullptr, nullptr);
    case DISCOVERY_SSID:         return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_ssid",         "WiFi SSID",           "diag/ssid",   nullptr,                      nullptr,       nullptr, "diagnostic", nullptr, nullptr);
    case DISCOVERY_RESET:        return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_reset",        "Last reset reason",   "diag/reset",  nullptr,                      nullptr,       nullptr, "diagnostic", nullptr, nullptr);
    case DISCOVERY_BOOTS:        return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_boots",        "Boot counter",        "diag/boots",  nullptr,                      nullptr,       nullptr, "diagnostic", nullptr, nullptr);
    case DISCOVERY_OTA_REVERTED: return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_ota_reverted", "OTA reverted",        "diag/ota",    nullptr,                      "problem",     nullptr, "diagnostic", "ON", "OFF");
    case DISCOVERY_TIMEZONE:     return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_timezone",     "Timezone",            "diag/tz",     nullptr,                      nullptr,       nullptr, "diagnostic", nullptr, nullptr);
    case DISCOVERY_SPEED:
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Flap speed\",\"cmd_t\":\"splitflap/%s/speed/set\",\"stat_t\":\"splitflap/%s/speed\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_speed\",\"min\":1,\"max\":100,\"step\":1,\"mode\":\"slider\"," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion);
    case DISCOVERY_ALIGNMENT:
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Alignment\",\"cmd_t\":\"splitflap/%s/alignment/set\",\"stat_t\":\"splitflap/%s/alignment\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_alignment\",\"ops\":[\"left\",\"center\",\"right\"]," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion);
    case DISCOVERY_RESTART:
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Restart\",\"cmd_t\":\"splitflap/%s/restart/set\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_restart\",\"pl_prs\":\"PRESS\",\"dev_cla\":\"restart\"," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion);
    case DISCOVERY_UNITS_FAULTY:
      //State is the integer faulty count (great for HA automations); the full
      //per-unit breakdown rides json_attr_t so it isn't bound by the 255-char
      //sensor-state limit (#137).
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Faulty units\",\"stat_t\":\"splitflap/%s/units_faulty\",\"json_attr_t\":\"splitflap/%s/units/attrs\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_units_faulty\",\"ent_cat\":\"diagnostic\"," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion);
    case DISCOVERY_UNIT_WEAR:
      //Binary problem sensor (#231): ON when any unit wears past the relative
      //threshold. The median + flagged unit list ride a dedicated attrs topic
      //so an automation can name the offender without parsing units/attrs.
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Unit wear warning\",\"stat_t\":\"splitflap/%s/units_wear\",\"json_attr_t\":\"splitflap/%s/units/wear\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_unit_wear\",\"dev_cla\":\"problem\",\"ent_cat\":\"diagnostic\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\"," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion);
    //                        obj             name                 stat_t          val_tpl                       dev_cla    unit    ent_cat        pOn    pOff
    case DISCOVERY_VCC_MIN:      return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_vcc_min",      "Unit supply Vcc (min)", "telemetry", "{{ value_json.vccMin }}",  "voltage", "mV",  "diagnostic", nullptr, nullptr);
    case DISCOVERY_REBOOT_TOTAL: return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_reboot_total", "Unit reboots (fleet)",  "telemetry", "{{ value_json.reboots }}", nullptr,   nullptr, "diagnostic", nullptr, nullptr);
    // #365: fleet jam/stall problem sensor + worst-seen home-step
    // excess, both telemetry-backed like vccMin/reboots. Binary_sensor
    // pl_on/pl_off "ON"/"OFF" matches DISCOVERY_UNIT_WEAR's convention (the
    // telemetry key is a quoted "ON"/"OFF" string, not NTP's bare 1/0).
    case DISCOVERY_UNIT_JAM:     return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_unit_jam",     "Unit jam",              "telemetry", "{{ value_json.jam }}",         "problem", nullptr, "diagnostic", "ON", "OFF");
    case DISCOVERY_STEPS_EXCESS: return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_steps_excess", "Unit steps excess (max)", "telemetry", "{{ value_json.stepsExcess }}", nullptr, "steps", "diagnostic", nullptr, nullptr);
  }
  if (bufLen > 0) buf[0] = '\0';
  return 0;
}
