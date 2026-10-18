#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "SettingsLimits.h"  // TEXT_PRIORITY_MAX
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

// A text's priority (#625): 0..MQTT_TEXT_PRIORITY_MAX. A text with a lower
// priority than the one showing (held or dwelling) is refused; equal or
// higher replaces it, so callers that never set one see today's behaviour
// (latest wins). The top priority is what "force" meant: it is the one
// priority that passes quiet.
#define MQTT_TEXT_PRIORITY_MAX TEXT_PRIORITY_MAX

inline int clampTextPriority(long priority) {
  if (priority < 0) return 0;
  if (priority > MQTT_TEXT_PRIORITY_MAX) return MQTT_TEXT_PRIORITY_MAX;
  return (int)priority;
}

// What a text/set payload asks for (#616/#625).
struct MqttTextRequest {
  String text;
  long dwellSeconds = MQTT_TEXT_DWELL_S;  // clamped; meaningless while held
  bool hold = false;                       // until the next text or a clear
  int priority = 0;
};

// Reads {"text":"…","dwell":N,"hold":true,"priority":N,"force":true}.
// Returns true iff `payload` is a JSON object with a string "text" member of
// its own; `out` is written only on success. "dwell" is optional (default
// MQTT_TEXT_DWELL_S) and clamped; a dwell of exactly 0 holds the text, as
// "hold":true does; a dwell that is otherwise not a positive number counts as
// absent. "force":true is the top priority. On false the caller treats the
// whole payload as plain text.
inline bool parseMqttTextRequest(const String& payload, MqttTextRequest& out) {
  JsonDocument doc;
  if (deserializeJson(doc, payload.c_str(), payload.length())) return false;
  JsonObjectConst object = doc.as<JsonObjectConst>();
  if (object.isNull()) return false;
  JsonVariantConst text = object["text"];
  if (!text.is<const char*>()) return false;

  MqttTextRequest r;
  r.text = text.as<const char*>();
  JsonVariantConst dwell = object["dwell"];
  long dwellSeconds = dwell | 0L;
  r.hold = (object["hold"] | false) || (dwell.is<long>() && dwellSeconds == 0);
  if (dwellSeconds <= 0) dwellSeconds = MQTT_TEXT_DWELL_S;
  r.dwellSeconds = clampDwellSeconds(dwellSeconds);
  r.priority = (object["force"] | false) ? MQTT_TEXT_PRIORITY_MAX
                                         : clampTextPriority(object["priority"] | 0L);
  out = r;
  return true;
}

// The text and dwell only (the callers that predate hold and priority).
inline bool parseMqttTextPayload(const String& payload, String& textOut, long& dwellSecondsOut) {
  MqttTextRequest r;
  if (!parseMqttTextRequest(payload, r)) return false;
  textOut = r.text;
  dwellSecondsOut = r.dwellSeconds;
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
  bool held = false;  // #616: no deadline, until the next text or a clear
  int priority = 0;   // #625: of the text showing; 0 again when it ends
  String text;
  uint32_t deadlineMs = 0;
};

inline void notificationStart(MqttNotification& n, const String& text, long dwellSeconds, uint32_t nowMs,
                              int priority = 0) {
  n.active = true;
  n.held = false;
  n.priority = priority;
  n.text = text;
  n.deadlineMs = nowMs + (uint32_t)clampDwellSeconds(dwellSeconds) * 1000UL;
}

// A text that stays until the next text or a clear (#616).
inline void notificationHold(MqttNotification& n, const String& text, int priority) {
  n.active = true;
  n.held = true;
  n.priority = priority;
  n.text = text;
  n.deadlineMs = 0;
}

// May a text of `priority` take the display (#625)? Always when nothing is
// showing; otherwise only one of equal or higher priority.
inline bool notificationAdmits(const MqttNotification& n, int priority) {
  return !n.active || priority >= n.priority;
}

// Explicit mode change cancels an active notification (#130): a user who
// just switched text/clock wants that NOW, not after the dwell runs out.
// So does a clear (#616). The priority ends with the text.
inline void notificationCancel(MqttNotification& n) {
  n.active = false;
  n.held = false;
  n.priority = 0;
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

// When a notification that starts now ends, as Unix seconds, for whoever
// shows it to a reader; 0 while the clock is not set.
inline uint32_t notificationEndsEpoch(uint32_t nowEpoch, bool clockSet, long dwellSeconds) {
  return clockSet ? nowEpoch + (uint32_t)clampDwellSeconds(dwellSeconds) : 0;
}

// millis()-wraparound-safe via signed difference of fixed-width unsigned.
inline bool notificationTick(MqttNotification& n, uint32_t nowMs) {
  if (!n.active) return false;
  if (n.held) return true;
  if ((int32_t)(nowMs - n.deadlineMs) >= 0) {
    notificationCancel(n);
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

// ---- The Text entity's attributes (#618) ----
// {"charset":"…","width":N}: what a text may be made of and how many flaps
// the wall has, so an automation can check and wrap before it sends. The
// charset holds no character JSON needs escaped (SplitFlapProtocol.h).
inline size_t buildTextAttrsJson(char* buf, size_t bufLen, const char* charset, int width) {
  return (size_t)mqttSnprintf(buf, bufLen, MQTT_FMT("{\"charset\":\"%s\",\"width\":%d}"), charset, width);
}

// ---- The update entity's state (#620) ----
// installed = the running rev, latest = the release's master rev when one
// was found (else the running rev: up to date, not unknown). The title and
// the release URL name the tag when there is one. The revs and the tag are
// hex and [A-Za-z0-9.-] by construction (ReleaseManifest.h), so no escaping.
inline size_t buildUpdateStateJson(char* buf, size_t bufLen, const char* runningRev,
                                   const char* releaseRev, const char* tag,
                                   const char* notesUrlPrefix, bool inProgress) {
  const bool known = releaseRev != nullptr && releaseRev[0] != 0;
  size_t o = (size_t)mqttSnprintf(buf, bufLen,
      MQTT_FMT("{\"installed_version\":\"%s\",\"latest_version\":\"%s\",\"in_progress\":%s"),
      runningRev, known ? releaseRev : runningRev, inProgress ? "true" : "false");
  if (known && tag != nullptr && tag[0] != 0 && o < bufLen) {
    o += (size_t)mqttSnprintf(buf + o, bufLen - o,
        MQTT_FMT(",\"title\":\"Split-Flap %s\",\"release_url\":\"%s%s\""), tag, notesUrlPrefix, tag);
  } else if (o < bufLen) {
    o += (size_t)mqttSnprintf(buf + o, bufLen - o, MQTT_FMT(",\"title\":\"Split-Flap firmware\""));
  }
  if (o < bufLen) o += (size_t)mqttSnprintf(buf + o, bufLen - o, MQTT_FMT("}"));
  return o;
}

// Install button of the update entity: HA publishes payload_install verbatim.
inline bool parseInstallCommand(const String& payload) {
  String trimmed = payload;
  trimmed.trim();
  return trimmed == "INSTALL";
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
// always emits it. MQTT-only derived stat (nothing else reads it), so it
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
// failed) contributes no jam. MQTT-only derived stat (nothing else
// reads it — the per-unit "sb" field already carries this), so it lives
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

// The weakest unit and the units under the floor (#623), beside vccMin:
// present only when vccMin is. `board` is a hostname ([a-z0-9-], no escaping).
// The telemetry packet at its widest: every number at its full width and the
// longest board name a place gives (WallLinkNames.h). test_mqtt_helpers
// builds that packet into a buffer of this size.
#define MQTT_TELEMETRY_MAX 288

struct MqttSupplyFacts {
  uint8_t unit = 0;
  const char* board = "";
  uint8_t under = 0;
  uint32_t unitUpS = 0;  // the weakest unit's seconds since its boot (#624): since when its minimum counts
};

inline size_t buildTelemetryPayload(char* buf, size_t bufLen, uint32_t freeHeap, int heapFragPct,
                                    long rssi, int unitErrors, unsigned long uptimeSec,
                                    bool ntpSynced, uint16_t vccMin_mV, uint32_t rebootTotal,
                                    bool anyJam, uint16_t maxExcess,
                                    const MqttSupplyFacts* supply = nullptr) {
  size_t o = (size_t)mqttSnprintf(buf, bufLen,
      MQTT_FMT("{\"heap\":%lu,\"heapFrag\":%d,\"rssi\":%ld,\"unitErrors\":%d,\"uptime\":%lu,\"ntp\":%d"),
      (unsigned long)freeHeap, heapFragPct, rssi, unitErrors, uptimeSec, ntpSynced ? 1 : 0);
  // Fleet-min supply Vcc (#366): omitted when no unit reports vitals (value 0)
  // so the HA sensor reads unavailable instead of a phantom 0 mV.
  if (vccMin_mV > 0 && o < bufLen)
    o += (size_t)mqttSnprintf(buf + o, bufLen - o, MQTT_FMT(",\"vccMin\":%u"), (unsigned)vccMin_mV);
  if (vccMin_mV > 0 && supply != nullptr && o < bufLen)
    o += (size_t)mqttSnprintf(buf + o, bufLen - o,
                              MQTT_FMT(",\"vccMinUnit\":%u,\"vccMinBoard\":\"%s\",\"vccUnder\":%u,\"vccMinUnitUp\":%lu"),
                              (unsigned)supply->unit, supply->board, (unsigned)supply->under,
                              (unsigned long)supply->unitUpS);
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
  // A notify entity (#615): notify.send_message lands on text/set as plain
  // text (dwell 60 s, priority 0); hold and priority ride the JSON form.
  DISCOVERY_NOTIFY,
  // A button that takes the text down (#616).
  DISCOVERY_TEXT_CLEAR,
  // The release the master looked up, as an update entity (#620): its state
  // JSON rides splitflap/<id>/update, INSTALL on update/set runs the
  // update-from-release job.
  DISCOVERY_FIRMWARE_UPDATE,
  // The weakest unit and the units under the supply floor (#623), beside
  // vccMin in the telemetry packet.
  DISCOVERY_VCC_MIN_UNIT,
  DISCOVERY_VCC_UNDER_FLOOR,
  // The wall's flips (#619): every unit's lifetime revolutions summed, a
  // rising counter on splitflap/<id>/flips.
  DISCOVERY_FLIPS,
  // Restart every unit (#624): a diagnostic button on restart_units/set; a
  // unit's since-boot supply minimum starts over with its restart.
  DISCOVERY_RESTART_UNITS,
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
    case DISCOVERY_NOTIFY:       return { "notify",        "" };
    case DISCOVERY_TEXT_CLEAR:   return { "button",        "_text_clear" };
    case DISCOVERY_FIRMWARE_UPDATE: return { "update",     "_firmware" };
    case DISCOVERY_VCC_MIN_UNIT:    return { "sensor",     "_vcc_min_unit" };
    case DISCOVERY_VCC_UNDER_FLOOR: return { "sensor",     "_vcc_under_floor" };
    case DISCOVERY_FLIPS:           return { "sensor",     "_flips" };
    case DISCOVERY_RESTART_UNITS:   return { "button",     "_restart_units" };
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

// Shared device block; %s slots are (deviceId, deviceId, fwVersion, deviceId).
// The configuration URL (#617) is the master's OWN mDNS name, which is its
// device id — never a row's name from its place (#611): that may be an ESP-01
// row board, which serves no page.
// The model string in HA's device registry defaults to the v1 label; the v2
// master overrides it via build flag (-D MQTT_DEVICE_MODEL=...) so this
// header stays a byte-identical copy in both trees.
#ifndef MQTT_DEVICE_MODEL
#define MQTT_DEVICE_MODEL "v1 ESPMaster"
#endif
#define MQTT_DEVICE_BLOCK "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Split-Flap %s\",\"mf\":\"split-flap\",\"mdl\":\"" MQTT_DEVICE_MODEL "\",\"sw\":\"%s\",\"cu\":\"http://%s.local/\"}"

// A discovery payload at its widest: the supply sensor's, with its attribute
// template and the board's id in six fields beside the device block (#633).
// The service and test_mqtt_helpers build every shared entity into a buffer
// of this size; a payload that does not fit is not published.
#define MQTT_DISCOVERY_MAX 768

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
  MQTT_DISC_APPEND(MQTT_FMT("," MQTT_DEVICE_BLOCK "}"), deviceId, deviceId, fw, deviceId);
  return o;
}

inline size_t buildDiscoveryPayload(char* buf, size_t bufLen, int entity, const char* deviceId, const char* fwVersion) {
  switch (entity) {
    case DISCOVERY_TEXT:
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Text\",\"cmd_t\":\"splitflap/%s/text/set\",\"json_attr_t\":\"splitflap/%s/text/attrs\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_text\",\"max\":255," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion, deviceId);
    case DISCOVERY_MODE:
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Mode\",\"cmd_t\":\"splitflap/%s/mode/set\",\"stat_t\":\"splitflap/%s/mode\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_mode\",\"ops\":[\"text\",\"clock\"]," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion, deviceId);
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
        deviceId, deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion, deviceId);
    case DISCOVERY_ALIGNMENT:
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Alignment\",\"cmd_t\":\"splitflap/%s/alignment/set\",\"stat_t\":\"splitflap/%s/alignment\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_alignment\",\"ops\":[\"left\",\"center\",\"right\"]," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion, deviceId);
    case DISCOVERY_RESTART:
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Restart\",\"cmd_t\":\"splitflap/%s/restart/set\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_restart\",\"pl_prs\":\"PRESS\",\"dev_cla\":\"restart\",\"ent_cat\":\"diagnostic\"," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion, deviceId);
    case DISCOVERY_RESTART_UNITS:
      // Not device_class restart: the board stays up, its units restart.
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Restart units\",\"cmd_t\":\"splitflap/%s/restart_units/set\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_restart_units\",\"pl_prs\":\"PRESS\",\"ic\":\"mdi:restart\",\"ent_cat\":\"diagnostic\"," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion, deviceId);
    case DISCOVERY_UNITS_FAULTY:
      //State is the integer faulty count (great for HA automations); the full
      //per-unit breakdown rides json_attr_t so it isn't bound by the 255-char
      //sensor-state limit (#137).
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Faulty units\",\"stat_t\":\"splitflap/%s/units_faulty\",\"json_attr_t\":\"splitflap/%s/units/attrs\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_units_faulty\",\"ent_cat\":\"diagnostic\"," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion, deviceId);
    case DISCOVERY_UNIT_WEAR:
      //Binary problem sensor (#231): ON when any unit wears past the relative
      //threshold. The median + flagged unit list ride a dedicated attrs topic
      //so an automation can name the offender without parsing units/attrs.
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Unit wear warning\",\"stat_t\":\"splitflap/%s/units_wear\",\"json_attr_t\":\"splitflap/%s/units/wear\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_unit_wear\",\"dev_cla\":\"problem\",\"ent_cat\":\"diagnostic\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\"," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion, deviceId);
    //                        obj             name                 stat_t          val_tpl                       dev_cla    unit    ent_cat        pOn    pOff
    case DISCOVERY_VCC_MIN:
      // The weakest unit, its board, since when its minimum counts and the
      // count under the floor ride as the sensor's attributes (#623/#624/#633),
      // named for a reader: the packet's keys are the firmware's.
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Unit supply Vcc (min)\",\"stat_t\":\"splitflap/%s/telemetry\",\"val_tpl\":\"{{ value_json.vccMin }}\",\"json_attr_t\":\"splitflap/%s/telemetry\",\"json_attr_tpl\":\"{{ {'unit': value_json.vccMinUnit, 'board': value_json.vccMinBoard, 'since_s': value_json.vccMinUnitUp, 'under_floor': value_json.vccUnder} | to_json }}\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_vcc_min\",\"dev_cla\":\"voltage\",\"unit_of_meas\":\"mV\",\"ent_cat\":\"diagnostic\"," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion, deviceId);
    case DISCOVERY_REBOOT_TOTAL: return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_reboot_total", "Unit reboots (fleet)",  "telemetry", "{{ value_json.reboots }}", nullptr,   nullptr, "diagnostic", nullptr, nullptr);
    // #365: fleet jam/stall problem sensor + worst-seen home-step
    // excess, both telemetry-backed like vccMin/reboots. Binary_sensor
    // pl_on/pl_off "ON"/"OFF" matches DISCOVERY_UNIT_WEAR's convention (the
    // telemetry key is a quoted "ON"/"OFF" string, not NTP's bare 1/0).
    case DISCOVERY_UNIT_JAM:     return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_unit_jam",     "Unit jam",              "telemetry", "{{ value_json.jam }}",         "problem", nullptr, "diagnostic", "ON", "OFF");
    case DISCOVERY_STEPS_EXCESS: return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_steps_excess", "Unit steps excess (max)", "telemetry", "{{ value_json.stepsExcess }}", nullptr, "steps", "diagnostic", nullptr, nullptr);
    case DISCOVERY_NOTIFY:
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Message\",\"cmd_t\":\"splitflap/%s/text/set\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_notify\"," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion, deviceId);
    case DISCOVERY_TEXT_CLEAR:
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Clear text\",\"cmd_t\":\"splitflap/%s/text/clear\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_text_clear\",\"pl_prs\":\"PRESS\",\"ic\":\"mdi:close-box-outline\"," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion, deviceId);
    case DISCOVERY_FIRMWARE_UPDATE:
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Firmware\",\"stat_t\":\"splitflap/%s/update\",\"cmd_t\":\"splitflap/%s/update/set\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_firmware\",\"pl_inst\":\"INSTALL\",\"dev_cla\":\"firmware\"," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion, deviceId);
    case DISCOVERY_VCC_MIN_UNIT:    return buildEntityDiscovery(buf, bufLen, deviceId, fwVersion, "_vcc_min_unit",    "Weakest unit",             "telemetry", "{{ value_json.vccMinBoard }} #{{ value_json.vccMinUnit }}", nullptr, nullptr, "diagnostic", nullptr, nullptr);
    case DISCOVERY_VCC_UNDER_FLOOR:
      // A count with a state class, so Home Assistant keeps its statistics.
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Units under supply floor\",\"stat_t\":\"splitflap/%s/telemetry\",\"val_tpl\":\"{{ value_json.vccUnder }}\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_vcc_under_floor\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\"," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion, deviceId);
    case DISCOVERY_FLIPS:
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Flips\",\"stat_t\":\"splitflap/%s/flips\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_flips\",\"stat_cla\":\"total_increasing\",\"ic\":\"mdi:counter\"," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion, deviceId);
  }
  if (bufLen > 0) buf[0] = '\0';
  return 0;
}
