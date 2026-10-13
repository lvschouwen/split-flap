#pragma once
// WallMqtt.h — what Home Assistant is told about a Split-Flap of several
// boards (#566): one "wall problem" binary sensor with the boards as its
// attributes, and the wall's whole text as text/state. Pure; MqttService.cpp
// fills WallMqttRow from the wall snapshot and publishes. Natively tested by
// test_wall_mqtt.
//
// Lifecycle rule (owned by MqttService.cpp): the discovery config is
// published only while the rows table has rows and blanked when it has none,
// so a master on its own never grows a meaningless entity.

#include <Arduino.h>

#include "MqttHelpers.h"   // MQTT_FMT/mqttSnprintf + MQTT_DEVICE_BLOCK
#include "SettingsJson.h"  // appendJsonString
#include "UnitEventLog.h"  // UNIT_VCC_MIN_FLOOR_MV, unitVccIsLow
#include "UnitHealth.h"    // UnitFacts
#include "WallWatch.h"     // WALL_VERDICT_BOARDS

// One board of the Split-Flap as the sensor sees it. The strings must outlive
// the call.
struct WallMqttRow {
  const char* id = "";     // "" = the master's own units
  const char* rev = "";
  const char* reach = "";  // WallRowReach's name
  const char* text = "";   // what it is to show
  uint8_t row = 0;
  uint8_t col = 0;
  uint8_t width = 0;
  bool own = false;
  bool lost = false;          // written off by the link
  bool welcomed = false;      // spoken to since this master started
  bool rescue = false;        // runs its rescue mode
  bool updateBlocked = false; // the stored image was given up on for it
  bool busDead = false;       // its unit bus is dead
  bool judged = false;        // the master has a verdict on it (BoardVerdict.h)
  bool fault = false;         // and it is a fault
  const char* level = "";     // the verdict's level and leading reason, by name
  const char* reason = "";
  bool unitsKnown = false;    // the three counts below are its own report
  uint8_t unitsFound = 0;
  uint8_t unitsFaulty = 0;
  uint8_t unitsLost = 0;
  // The board itself (#621): its name from its place, where it is reached.
  const char* host = "";
  const char* address = "";
  bool rssiKnown = false;
  int rssi = 0;
  // Its units' supply (#623), from wallMqttUnitAggregates: the lowest
  // since-boot minimum any of them reports (0 = none), whose, and how many
  // are under the floor.
  uint16_t vccMin = 0;
  uint8_t vccMinUnit = 0;
  uint8_t vccUnder = 0;
};

// The supply facts of one board's units (#623).
inline void wallMqttUnitAggregates(const UnitFacts* units, int width, uint8_t addressBase,
                                   WallMqttRow& r) {
  r.vccMin = 0;
  r.vccMinUnit = 0;
  r.vccUnder = 0;
  for (int i = 0; i < width; i++) {
    const UnitFacts& u = units[i];
    if (!u.vitalsValid || u.vitals.vccMin_mV == 0) continue;
    if (r.vccMin == 0 || u.vitals.vccMin_mV < r.vccMin) {
      r.vccMin = u.vitals.vccMin_mV;
      r.vccMinUnit = (uint8_t)(addressBase + i);
    }
    if (unitVccIsLow(true, u.vitals.vccMin_mV, UNIT_VCC_MIN_FLOOR_MV)) r.vccUnder++;
  }
}

// The wall's weakest unit and how many are under the floor (#623).
struct WallMqttSupply {
  uint16_t mV = 0;   // 0 = no unit reports its supply
  uint8_t unit = 0;  // its address
  int board = -1;    // its board, an index into the rows given
  uint8_t under = 0;
};

inline WallMqttSupply wallMqttSupply(const WallMqttRow* rows, int count) {
  WallMqttSupply s;
  for (int i = 0; i < count; i++) {
    const WallMqttRow& r = rows[i];
    s.under = (uint8_t)(s.under + r.vccUnder);
    if (r.vccMin != 0 && (s.mV == 0 || r.vccMin < s.mV)) {
      s.mV = r.vccMin;
      s.unit = r.vccMinUnit;
      s.board = i;
    }
  }
  return s;
}

// The wall's flips (#619): every unit's lifetime revolutions, summed. A unit
// that cannot be read keeps the count it last gave, so the sum never drops
// for a silence; it drops only when an odometer is reset. The sum is not
// told before every unit a board found has been read once, so a sum that
// starts short of the truth (a rising counter's statistics would take the
// rest as flips) is never published.
struct WallMqttFlips {
  bool have[WALL_VERDICT_BOARDS][UNITS_AMOUNT] = {};
  uint32_t last[WALL_VERDICT_BOARDS][UNITS_AMOUNT] = {};
};

inline void wallMqttFlipsReset(WallMqttFlips& f) { f = WallMqttFlips(); }

// Folds one board's units in. False when a unit it found has no reading yet.
inline bool wallMqttFlipsNote(WallMqttFlips& f, int board, const UnitFacts* units, int width) {
  if (board < 0 || board >= WALL_VERDICT_BOARDS) return false;
  bool complete = true;
  for (int i = 0; i < width && i < UNITS_AMOUNT; i++) {
    const UnitFacts& u = units[i];
    if (u.odometerValid) {
      f.have[board][i] = true;
      f.last[board][i] = u.odometer;
    } else if (u.state != 0 && !f.have[board][i]) {
      complete = false;
    }
  }
  return complete;
}

inline uint32_t wallMqttFlipsTotal(const WallMqttFlips& f, int boards) {
  uint32_t total = 0;
  for (int b = 0; b < boards && b < WALL_VERDICT_BOARDS; b++) {
    for (int i = 0; i < UNITS_AMOUNT; i++) {
      if (f.have[b][i]) total += f.last[b][i];
    }
  }
  return total;
}

// ---- one problem sensor a board (#621) ----
// Keyed by the board's id, "own" for the master's own row: ids never change,
// names from places do (#611), so a renamed board keeps its entity.
inline const char* wallMqttBoardKey(const WallMqttRow& r) { return r.own ? "own" : r.id; }

inline bool wallMqttBoardProblem(const WallMqttRow& r) { return r.judged && r.fault; }

inline size_t buildBoardProblemDiscoveryTopic(char* buf, size_t bufLen, const char* deviceId,
                                              const char* key) {
  return (size_t)mqttSnprintf(buf, bufLen,
                              MQTT_FMT("homeassistant/binary_sensor/%s_board_%s/config"),
                              deviceId, key);
}

// Named by the board's hostname, which is its place on the wall.
inline size_t buildBoardProblemDiscovery(char* buf, size_t bufLen, const char* deviceId,
                                         const char* fwVersion, const char* key,
                                         const char* host) {
  return (size_t)mqttSnprintf(
      buf, bufLen,
      MQTT_FMT("{\"name\":\"%s\","
               "\"stat_t\":\"splitflap/%s/board/%s/problem\","
               "\"json_attr_t\":\"splitflap/%s/board/%s/attrs\","
               "\"avty_t\":\"splitflap/%s/availability\","
               "\"uniq_id\":\"%s_board_%s\","
               "\"dev_cla\":\"problem\",\"ent_cat\":\"diagnostic\","
               "\"pl_on\":\"ON\",\"pl_off\":\"OFF\"," MQTT_DEVICE_BLOCK "}"),
      host, deviceId, key, deviceId, key, deviceId, deviceId, key, deviceId, deviceId, fwVersion,
      deviceId);
}

inline String wallMqttBoardAttrsJson(const WallMqttRow& r) {
  String out;
  out.reserve(256);
  out += "{\"id\":";
  appendJsonString(out, String(r.id));
  out += ",\"host\":";
  appendJsonString(out, String(r.host));
  out += ",\"own\":";
  out += r.own ? "true" : "false";
  out += ",\"row\":";
  out += r.row;
  out += ",\"col\":";
  out += r.col;
  out += ",\"width\":";
  out += r.width;
  if (r.address[0] != 0) {
    out += ",\"address\":";
    appendJsonString(out, String(r.address));
  }
  if (r.rssiKnown) {
    out += ",\"rssi\":";
    out += r.rssi;
  }
  if (r.own || r.welcomed) {
    out += ",\"rev\":";
    appendJsonString(out, String(r.rev));
  }
  if (!r.own) {
    out += ",\"reach\":";
    appendJsonString(out, String(r.reach));
    out += ",\"rescue\":";
    out += r.rescue ? "true" : "false";
  }
  if (r.judged) {
    out += ",\"level\":";
    appendJsonString(out, String(r.level));
    out += ",\"reason\":";
    appendJsonString(out, String(r.reason));
  }
  if (r.unitsKnown) {
    out += ",\"found\":";
    out += r.unitsFound;
    out += ",\"faulty\":";
    out += r.unitsFaulty;
    out += ",\"lost\":";
    out += r.unitsLost;
  }
  if (r.vccMin != 0) {
    out += ",\"supplyMinMv\":";
    out += r.vccMin;
    out += ",\"supplyMinUnit\":";
    out += r.vccMinUnit;
  }
  out += '}';
  return out;
}

// ON = somebody has to look: a board whose verdict is a fault, the master's
// own row included. A fault is something wrong now, so the sensor clears
// when the trouble does; notes never trip it.
inline bool wallMqttProblem(const WallMqttRow* rows, int count) {
  for (int i = 0; i < count; i++) {
    if (rows[i].judged && rows[i].fault) return true;
  }
  return false;
}

// Text capacity of the wall: every board's units.
inline int wallMqttCapacity(const WallMqttRow* rows, int count) {
  int total = 0;
  for (int i = 0; i < count; i++) total += rows[i].width;
  return total;
}

// json_attributes payload of the sensor. Ids are operator-visible names and
// revs arrive from the boards, so both get real JSON escaping.
inline String wallMqttAttrsJson(const WallMqttRow* rows, int count, const char* updatePhase) {
  String out;
  out.reserve(96 + count * 220);
  out += "{\"boards\":[";
  for (int i = 0; i < count; i++) {
    const WallMqttRow& r = rows[i];
    if (i > 0) out += ',';
    out += "{\"id\":";
    appendJsonString(out, String(r.id));
    out += ",\"own\":";
    out += r.own ? "true" : "false";
    out += ",\"row\":";
    out += r.row;
    out += ",\"col\":";
    out += r.col;
    out += ",\"width\":";
    out += r.width;
    if (!r.own) {
      out += ",\"reach\":";
      appendJsonString(out, String(r.reach));
      if (r.welcomed) {
        out += ",\"rev\":";
        appendJsonString(out, String(r.rev));
        out += ",\"rescue\":";
        out += r.rescue ? "true" : "false";
        out += ",\"updateBlocked\":";
        out += r.updateBlocked ? "true" : "false";
        out += ",\"busDead\":";
        out += r.busDead ? "true" : "false";
      }
    }
    if (r.judged) {
      out += ",\"level\":";
      appendJsonString(out, String(r.level));
      out += ",\"reason\":";
      appendJsonString(out, String(r.reason));
    }
    if (r.unitsKnown) {
      out += ",\"found\":";
      out += r.unitsFound;
      out += ",\"faulty\":";
      out += r.unitsFaulty;
      out += ",\"lost\":";
      out += r.unitsLost;
    }
    out += '}';
  }
  out += "],\"update\":";
  appendJsonString(out, String(updatePhase));
  out += '}';
  return out;
}

// text/state of a wall: every grid row on its own line, boards side by side
// on one grid row joined in column order, truncated to Home Assistant's
// 255-character state limit.
inline String wallMqttStateText(const WallMqttRow* rows, int count) {
  String out;
  int maxRow = -1;
  for (int i = 0; i < count; i++) {
    if (rows[i].row > maxRow) maxRow = rows[i].row;
  }
  for (int line = 0; line <= maxRow; line++) {
    if (line > 0) out += '\n';
    int lastCol = -1;
    for (;;) {
      int next = -1;
      for (int i = 0; i < count; i++) {
        if (rows[i].row != line || (int)rows[i].col <= lastCol) continue;
        if (next < 0 || rows[i].col < rows[next].col) next = i;
      }
      if (next < 0) break;
      out += rows[next].text;
      lastCol = rows[next].col;
    }
  }
  if (out.length() > 255) out = out.substring(0, 255);
  return out;
}

inline size_t buildWallProblemDiscoveryTopic(char* buf, size_t bufLen, const char* deviceId) {
  return (size_t)mqttSnprintf(
      buf, bufLen, MQTT_FMT("homeassistant/binary_sensor/%s_wall_problem/config"), deviceId);
}

// Same shape rules as MqttHelpers.h's entities (shared device block, the
// documented HA short keys, 512-byte truncation guard in the caller).
inline size_t buildWallProblemDiscovery(char* buf, size_t bufLen, const char* deviceId,
                                        const char* fwVersion) {
  return (size_t)mqttSnprintf(
      buf, bufLen,
      MQTT_FMT("{\"name\":\"Wall problem\","
               "\"stat_t\":\"splitflap/%s/wall_problem\","
               "\"json_attr_t\":\"splitflap/%s/wall/attrs\","
               "\"avty_t\":\"splitflap/%s/availability\","
               "\"uniq_id\":\"%s_wall_problem\","
               "\"dev_cla\":\"problem\",\"ent_cat\":\"diagnostic\","
               "\"pl_on\":\"ON\",\"pl_off\":\"OFF\"," MQTT_DEVICE_BLOCK "}"),
      deviceId, deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion, deviceId);
}

// Entities this firmware published before the wall link and no longer does.
// Their retained configs are blanked once per session so Home Assistant drops
// them. `index` runs from 0; returns 0 when there are no more.
inline size_t buildRetiredDiscoveryTopic(char* buf, size_t bufLen, const char* deviceId,
                                         int index) {
  static const char* const retired[] = {"cluster_degraded", "leader_lost"};
  if (index < 0 || index >= (int)(sizeof(retired) / sizeof(retired[0]))) return 0;
  return (size_t)mqttSnprintf(buf, bufLen,
                              MQTT_FMT("homeassistant/binary_sensor/%s_%s/config"), deviceId,
                              retired[index]);
}
