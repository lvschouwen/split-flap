#pragma once
// QuietPolicy.h — quiet mode (#227, round 1): while it is on the wall issues
// no flap commands. The clock stops updating and the last frame stays up;
// texts and notifications that arrive are dropped, not queued.
//
//   - One exception: a text whose JSON payload carries "force":true is shown
//     regardless (an alarm). Nothing else passes.
//   - Operator actions are not content: Stop, unit maintenance and reflash
//     keep working.
//   - Stored in NVS, so a board that restarts at night without a broker stays
//     quiet. The MQTT command is expected RETAINED by its sender, so a board
//     that was offline for the change still receives it on reconnect.
//   - The master owns the state and tells every row board (Quiet on the wall
//     link, at its Hello and on every change): a row that loses its master at
//     night would otherwise start its own clock.
//
// Pure logic, natively tested (test_quiet_policy). v2-only — MqttHelpers.h
// stays the v1-tracking copy, so this entity is not part of its discovery
// enum; MqttService.cpp publishes it beside the shared set.
#include <Arduino.h>
#include <ctype.h>

#include "MqttHelpers.h"  // MQTT_FMT/mqttSnprintf + MQTT_DEVICE_BLOCK

// "ON"/"OFF" as Home Assistant's switch sends them; also 1/0 and true/false.
// Anything else is not a command.
inline bool quietParseCommand(const String& payload, bool& out) {
  String p = payload;
  p.trim();
  p.toUpperCase();
  if (p == "ON" || p == "1" || p == "TRUE") {
    out = true;
    return true;
  }
  if (p == "OFF" || p == "0" || p == "FALSE") {
    out = false;
    return true;
  }
  return false;
}

// Does a text/set payload ask to be shown even while quiet? Only a JSON
// object whose TOP-LEVEL key "force" has the literal value true. The word
// inside a string value, a nested object or a plain-text payload does not
// count: this is the one way past quiet.
inline bool quietTextForced(const String& payload) {
  String p = payload;
  p.trim();
  if (!p.startsWith("{") || !p.endsWith("}")) return false;
  const unsigned int n = p.length();
  int depth = 0;
  bool inString = false;
  unsigned int strStart = 0;
  for (unsigned int i = 0; i < n; i++) {
    char c = p[i];
    if (inString) {
      if (c == '\\') {
        i++;  // the escaped character belongs to the string
      } else if (c == '"') {
        inString = false;
        // A string at depth 1 followed by ':' is a top-level key.
        if (depth == 1 && i - strStart == 6 &&
            p.substring(strStart + 1, i) == "force") {
          unsigned int j = i + 1;
          while (j < n && isspace((unsigned char)p[j])) j++;
          if (j < n && p[j] == ':') {
            j++;
            while (j < n && isspace((unsigned char)p[j])) j++;
            if (p.substring(j, j + 4) != "true") return false;
            j += 4;
            return j >= n || p[j] == ',' || p[j] == '}' ||
                   isspace((unsigned char)p[j]);
          }
        }
      }
    } else if (c == '"') {
      inString = true;
      strStart = i;
    } else if (c == '{' || c == '[') {
      depth++;
    } else if (c == '}' || c == ']') {
      depth--;
    }
  }
  return false;
}

// Is this piece of content dropped?
inline bool quietBlocksContent(bool quiet, bool forced) {
  return quiet && !forced;
}

inline size_t buildQuietDiscoveryTopic(char* buf, size_t bufLen,
                                       const char* deviceId) {
  return (size_t)mqttSnprintf(
      buf, bufLen, MQTT_FMT("homeassistant/switch/%s_quiet/config"), deviceId);
}

// "ret":true makes Home Assistant publish its command retained.
inline size_t buildQuietDiscovery(char* buf, size_t bufLen,
                                  const char* deviceId,
                                  const char* fwVersion) {
  return (size_t)mqttSnprintf(
      buf, bufLen,
      MQTT_FMT("{\"name\":\"Quiet\","
               "\"cmd_t\":\"splitflap/%s/quiet/set\","
               "\"stat_t\":\"splitflap/%s/quiet\","
               "\"avty_t\":\"splitflap/%s/availability\","
               "\"uniq_id\":\"%s_quiet\",\"ic\":\"mdi:sleep\","
               "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"ret\":true,"
               MQTT_DEVICE_BLOCK "}"),
      deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion);
}
