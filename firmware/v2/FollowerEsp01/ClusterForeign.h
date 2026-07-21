#pragma once
// ClusterForeign.h — foreign-contact observability (#358). Byte-identical
// copy in firmware/v2/Master and firmware/v2/FollowerEsp01 (copy policy:
// fix bugs in both trees; pinned by tests/test_copied_headers.py).
//
// A follower already arbitrates multi-master contention correctly (409
// other-leader on a foreign join, 403 source-IP/HMAC rejection on foreign
// render/ping) — but every refusal used to be silent: a misconfigured
// second leader with an overlapping member table could hammer a row and
// nothing surfaced it. These counters + the last foreign host ride
// /cluster/health as a "foreign" block. RAM-only, resets on reboot; the
// web-handler context is the sole writer AND reader on both platforms.
// Natively tested (FollowerEsp01 test_follower_json).

#include <Arduino.h>
#include <stdint.h>

enum class ForeignContactKind : uint8_t { Join, Ping, Render };

struct ForeignContactStats {
  uint32_t joins = 0;
  uint32_t pings = 0;
  uint32_t renders = 0;
  String lastHost;
  uint32_t lastMs = 0;  // millis() at the last refusal
  bool any = false;     // distinguishes a real lastMs==0 from "never"
};

inline void foreignContactRecord(ForeignContactStats& s, ForeignContactKind k,
                                 const String& host, uint32_t nowMs) {
  switch (k) {
    case ForeignContactKind::Join:   s.joins++;   break;
    case ForeignContactKind::Ping:   s.pings++;   break;
    case ForeignContactKind::Render: s.renders++; break;
  }
  s.lastHost = host;
  s.lastMs = nowMs;
  s.any = true;
}

// Appends `,"foreign":{...}` (leading comma: callers splice it before their
// closing brace). msSince -1 = no foreign contact since boot. lastHost is
// a remoteIP().toString() literal — JSON-safe without escaping.
inline void foreignContactAppendJson(String& out, const ForeignContactStats& s,
                                     uint32_t nowMs) {
  out += ",\"foreign\":{\"joins\":";
  out += String((unsigned long)s.joins);
  out += ",\"pings\":";
  out += String((unsigned long)s.pings);
  out += ",\"renders\":";
  out += String((unsigned long)s.renders);
  out += ",\"lastHost\":\"";
  out += s.lastHost;
  out += "\",\"msSince\":";
  out += s.any ? String((unsigned long)(nowMs - s.lastMs)) : String(-1);
  out += '}';
}
