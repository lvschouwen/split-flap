#pragma once

#include <Arduino.h>

// Pure logic for the MQTT broker auto-detect endpoint (#129): which host and
// port to suggest for an mDNS answer, and the /mqtt/discover JSON assembly.
// No mDNS stack and no networking — everything in this header is exercised
// by `pio test -e native` (test_mdns_discovery). The loop()-side code fills
// MdnsBrokerCandidate structs from MDNS.queryService() answers and hands
// them here.

#define MQTT_DEFAULT_BROKER_PORT 1883

struct MdnsBrokerCandidate {
  String name;             // mDNS hostname, without ".local"
  String ip;               // dotted quad, empty when the answer carried none
  uint16_t advertisedPort; // port from the service record, 0 when absent
  bool fromHomeAssistant;  // found via _home-assistant._tcp, not _mqtt._tcp
};

// A _mqtt._tcp answer advertises the broker itself — trust its port. A
// _home-assistant._tcp answer advertises the HA web UI (8123); the useful
// suggestion is the Mosquitto add-on on the same host, which listens on 1883.
inline uint16_t suggestedBrokerPort(const MdnsBrokerCandidate& candidate) {
  if (candidate.fromHomeAssistant || candidate.advertisedPort == 0) {
    return MQTT_DEFAULT_BROKER_PORT;
  }
  return candidate.advertisedPort;
}

// LEAmDNS answer hostnames arrive in domain form ("host.local." or
// "host.local"); candidates carry the bare label.
inline String normalizeMdnsHostname(const String& hostname) {
  String label = hostname;
  if (label.endsWith(".")) label.remove(label.length() - 1);
  if (label.endsWith(".local")) label.remove(label.length() - 6);
  return label;
}

// AsyncMqttClient resolves hostnames via plain DNS, not mDNS, so a raw IP is
// the suggestion that always works; "<name>.local" is the fallback when the
// answer carried no address.
inline String preferredBrokerHost(const MdnsBrokerCandidate& candidate) {
  if (candidate.ip.length() > 0) return candidate.ip;
  return candidate.name + ".local";
}

// JSON string literal (with quotes) appended to `out`. mDNS labels shouldn't
// need escaping, but the answer comes off the wire — never trust it into a
// JSON literal raw.
inline void mdnsAppendJsonString(String& out, const String& value) {
  out += '"';
  for (unsigned int i = 0; i < value.length(); i++) {
    char c = value[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
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

// The GET /mqtt/discover "done" payload:
// {"status":"done","candidates":[{"host":…,"name":…,"port":…,"source":…},…]}
// "host"/"port" are the prefill suggestions; "name" is the display label.
inline String buildDiscoverJson(const MdnsBrokerCandidate* candidates, size_t count) {
  String out;
  out.reserve(48 + count * 96);
  out += "{\"status\":\"done\",\"candidates\":[";
  for (size_t i = 0; i < count; i++) {
    if (i > 0) out += ',';
    out += "{\"host\":";
    mdnsAppendJsonString(out, preferredBrokerHost(candidates[i]));
    out += ",\"name\":";
    mdnsAppendJsonString(out, candidates[i].name);
    out += ",\"port\":";
    out += String(suggestedBrokerPort(candidates[i]));
    out += ",\"source\":\"";
    out += candidates[i].fromHomeAssistant ? "home-assistant" : "mqtt";
    out += "\"}";
  }
  out += "]}";
  return out;
}
