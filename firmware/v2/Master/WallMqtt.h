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
  bool unitsKnown = false;    // the three counts below are its own report
  uint8_t unitsFound = 0;
  uint8_t unitsFaulty = 0;
  uint8_t unitsLost = 0;
};

// ON = somebody has to look: a row board that is gone, in rescue mode or
// given up on by the update, or one whose units went dark. A row merely busy
// or briefly away is normal; so is an update in progress. Faulty units fold
// sticky lifetime counters and would latch, so they are attributes only.
inline bool wallMqttProblem(const WallMqttRow* rows, int count) {
  for (int i = 0; i < count; i++) {
    const WallMqttRow& r = rows[i];
    if (r.own) continue;
    if (r.lost || r.rescue || r.updateBlocked || r.busDead) return true;
    if (r.unitsKnown && r.unitsLost > 0) return true;
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
  out.reserve(96 + count * 170);
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
      deviceId, deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion);
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
