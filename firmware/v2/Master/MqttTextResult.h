#pragma once
// MqttTextResult.h — what became of the last text (#622), as a Home
// Assistant `event` entity: one message on text/result after every text
// command from MQTT or the API, never retained. An event entity fires on
// every message, so two texts dropped in a row are two events, where a
// sensor reading "dropped" twice would not change. Pure; MqttService.cpp
// queues results from any task and publishes them on mqttTask. Natively
// tested by test_mqtt_text_result.
//
//   {"event_type":"shown","text":"…","priority":0,"hold":false,"dwell":60,
//    "seq":12,"truncated":false,"unsupported":0}
//
// dropped_busy = a unit firmware update is running or the display queue is
// full; a moving frame is not busy (the text queues and reports shown).
// refused = a lower priority than the text showing (#625).

#include <Arduino.h>

#include "JsonEscape.h"   // appendJsonString
#include "MqttHelpers.h"  // MQTT_FMT/mqttSnprintf + MQTT_DEVICE_BLOCK

// A text/set payload's longest text (the v1 wire truncation rule).
#ifndef MQTT_MAX_TEXT_LEN
#define MQTT_MAX_TEXT_LEN 256
#endif

enum class MqttTextResultKind : uint8_t {
  Shown,
  Held,
  Refused,
  DroppedQuiet,
  DroppedBusy,
  Cleared,
  Count
};

inline const char* mqttTextResultName(MqttTextResultKind kind) {
  switch (kind) {
    case MqttTextResultKind::Shown: return "shown";
    case MqttTextResultKind::Held: return "held";
    case MqttTextResultKind::Refused: return "refused";
    case MqttTextResultKind::DroppedQuiet: return "dropped_quiet";
    case MqttTextResultKind::DroppedBusy: return "dropped_busy";
    case MqttTextResultKind::Cleared: return "cleared";
    default: return "";
  }
}

// One result as noted at its site; the wall facts (truncated, unsupported)
// are added when it is published.
struct MqttTextResult {
  MqttTextResultKind kind = MqttTextResultKind::Shown;
  int priority = 0;
  bool hold = false;
  long dwellSeconds = 0;  // 0 while held or for a text without a dwell
  char text[MQTT_MAX_TEXT_LEN + 1] = {0};
};

// Letters a text asks for, line breaks left out.
inline int mqttTextLetters(const char* text) {
  int n = 0;
  for (; *text != 0; text++) {
    if (*text != '\n') n++;
  }
  return n;
}

inline String buildTextResultJson(const MqttTextResult& r, uint32_t seq, int width,
                                  int unsupported) {
  String out;
  out.reserve(96 + strlen(r.text));
  out += "{\"event_type\":\"";
  out += mqttTextResultName(r.kind);
  out += "\",\"text\":";
  appendJsonString(out, String(r.text));
  out += ",\"priority\":";
  out += r.priority;
  out += ",\"hold\":";
  out += r.hold ? "true" : "false";
  out += ",\"dwell\":";
  out += r.dwellSeconds;
  out += ",\"seq\":";
  out += (unsigned long)seq;
  out += ",\"truncated\":";
  out += (width > 0 && mqttTextLetters(r.text) > width) ? "true" : "false";
  out += ",\"unsupported\":";
  out += unsupported;
  out += '}';
  return out;
}

inline size_t buildTextResultDiscoveryTopic(char* buf, size_t bufLen, const char* deviceId) {
  return (size_t)mqttSnprintf(buf, bufLen, MQTT_FMT("homeassistant/event/%s_text_result/config"),
                              deviceId);
}

// Home Assistant drops an event whose type is not listed: every kind is.
inline size_t buildTextResultDiscovery(char* buf, size_t bufLen, const char* deviceId,
                                       const char* fwVersion) {
  size_t o = (size_t)mqttSnprintf(
      buf, bufLen,
      MQTT_FMT("{\"name\":\"Text result\",\"stat_t\":\"splitflap/%s/text/result\","
               "\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_text_result\","
               "\"ent_cat\":\"diagnostic\",\"event_types\":["),
      deviceId, deviceId, deviceId);
  for (int k = 0; k < (int)MqttTextResultKind::Count; k++) {
    if (o >= bufLen) return o;
    o += (size_t)mqttSnprintf(buf + o, bufLen - o, MQTT_FMT("%s\"%s\""), k > 0 ? "," : "",
                              mqttTextResultName((MqttTextResultKind)k));
  }
  if (o >= bufLen) return o;
  o += (size_t)mqttSnprintf(buf + o, bufLen - o, MQTT_FMT("]," MQTT_DEVICE_BLOCK "}"), deviceId,
                            deviceId, fwVersion, deviceId);
  return o;
}
