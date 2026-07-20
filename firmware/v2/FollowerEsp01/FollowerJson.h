#pragma once
// FollowerJson.h — the follower's wire-reply builders (#298), natively
// tested by test_follower_json. Pure String assembly: the join/ping replies
// the S3 leader parses (#272 trio + #294 health keys + the #297 additive
// plat/vitals block), the tiny /settings JSON the member ⚙ panel reads,
// and /cluster/health. followerFaultMaskHex is a copy of the v2 master's
// clusterFaultMaskHex (ClusterDigest.h — copy policy: fix bugs in both
// trees); appendJsonString mirrors the fleet-wide escaping rule.

#include <Arduino.h>

#include "UnitHealth.h"

#define FOLLOWER_PLAT "esp01"

// Minimal JSON string escaping (quotes, backslash, control chars) — wire
// strings like the leader name come off an unauthenticated LAN POST.
inline void followerAppendJsonString(String& out, const String& value) {
  out += '"';
  for (unsigned int i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if ((unsigned char)c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
      out += buf;
    } else {
      out += c;
    }
  }
  out += '"';
}

// Hex fault bitmap for this row: bit i = unit at position i is faulty (same
// statusValid gate as computeFaultyUnitCount — unread units are unknown,
// not faulty). Fixed width ceil(width/4) nibbles so the string length
// itself carries the row width.
inline size_t followerFaultMaskHex(const UnitFacts* units, int width,
                                   char* buf, size_t cap) {
  if (width > 32) width = 32;  // uint32 bitmap; real rows are ≤16
  if (width <= 0) {            // no units, no mask
    if (cap > 0) buf[0] = '\0';
    return 0;
  }
  uint32_t mask = 0;
  for (int i = 0; i < width; i++) {
    if (units[i].statusValid && unitStatusIsFaulty(units[i].status)) {
      mask |= (1UL << i);
    }
  }
  int nibbles = (width + 3) / 4;
  if (cap == 0) return 0;
  int n = snprintf(buf, cap, "%0*X", nibbles, (unsigned)mask);
  if (n < 0) {
    buf[0] = '\0';
    return 0;
  }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

// One row's health facts, snapshotted by the caller.
struct FollowerHealthFacts {
  int width = 0;
  int detected = 0;
  int faulty = 0;
  const char* faultMask = "";
  bool wear = false;
};

// The #297 vitals the ESP-01 reports on every join/ping reply and /settings.
struct FollowerVitals {
  uint32_t heapBytes = 0;
  int rssiDbm = 0;
  uint32_t upSeconds = 0;
};

// ,"plat":"esp01","heap":H,"rssi":R,"up":U — the #297 additive block.
inline void followerAppendPlatVitals(String& out, const FollowerVitals& v) {
  out += ",\"plat\":\"" FOLLOWER_PLAT "\",\"heap\":";
  out += String((unsigned long)v.heapBytes);
  out += ",\"rssi\":";
  out += v.rssiDbm;
  out += ",\"up\":";
  out += String((unsigned long)v.upSeconds);
}

// The #294 health keys both the join and ping replies carry.
inline void followerAppendHealthKeys(String& out,
                                     const FollowerHealthFacts& h) {
  out += ",\"width\":";
  out += h.width;
  out += ",\"detected\":";
  out += h.detected;
  out += ",\"faulty\":";
  out += h.faulty;
  out += ",\"faultMask\":\"";
  out += h.faultMask;
  out += "\",\"wear\":";
  out += h.wear ? "true" : "false";
}

// #343 additive: only a rescue-beacon boot emits the marker (`"rescue":1`
// — an int so the leader's existing bare-number extractor reads it);
// absent = healthy, so pre-#343 leaders see an unchanged reply.
inline void followerAppendRescue(String& out, bool rescue) {
  if (rescue) out += ",\"rescue\":1";
}

// POST /cluster/join reply — the v2 handshake shape plus plat/vitals.
inline String followerJoinReplyJson(const String& name, const char* rev,
                                    const FollowerHealthFacts& h,
                                    const FollowerVitals& v, bool rescue) {
  String out;
  out.reserve(224);
  out += "{\"name\":";
  followerAppendJsonString(out, name);
  out += ",\"rev\":\"";
  out += rev;
  out += '"';
  followerAppendHealthKeys(out, h);
  followerAppendPlatVitals(out, v);
  followerAppendRescue(out, rescue);
  out += ",\"protocol\":1}";
  return out;
}

// POST /cluster/ping reply — state/epoch/seq trio, then the health keys +
// rev (the leader's rev-refresh fact), then plat/vitals.
inline String followerPingReplyJson(const char* phaseName, uint32_t epoch,
                                    uint32_t seq,
                                    const FollowerHealthFacts& h,
                                    const FollowerVitals& v, const char* rev,
                                    bool rescue) {
  String out;
  out.reserve(224);
  out += "{\"state\":\"";
  out += phaseName;
  out += "\",\"epoch\":";
  out += String((unsigned long)epoch);
  out += ",\"seq\":";
  out += String((unsigned long)seq);
  followerAppendHealthKeys(out, h);
  out += ",\"rev\":\"";
  out += rev;
  out += '"';
  followerAppendPlatVitals(out, v);
  followerAppendRescue(out, rescue);
  out += '}';
  return out;
}

// GET /cluster/health — the v2 follower's shape (leader + member panel).
struct FollowerClusterDiag {
  int32_t msSinceRender = -1;
  int32_t secsUntilBlank = -1;
  uint32_t i2cTx = 0;
  uint32_t i2cErr = 0;
  uint32_t minHeap = 0;
  bool sntpSynced = false;
  bool hmac = false;  // #313 follow-on: enforcing signed leader-wire requests
};

inline String followerClusterHealthJson(
    const char* phaseName, const String& leaderName, const String& leaderHost,
    int row, uint32_t epoch, uint32_t seq, const String& segment,
    const char* rev, int width, int detected, int faulty,
    const FollowerClusterDiag& d) {
  String out;
  out.reserve(320);
  out += "{\"state\":\"";
  out += phaseName;
  out += "\",\"leaderName\":";
  followerAppendJsonString(out, leaderName);
  out += ",\"leaderHost\":";
  followerAppendJsonString(out, leaderHost);
  out += ",\"row\":";
  out += row;
  out += ",\"epoch\":";
  out += String((unsigned long)epoch);
  out += ",\"seq\":";
  out += String((unsigned long)seq);
  out += ",\"segment\":";
  followerAppendJsonString(out, segment);
  out += ",\"rev\":\"";
  out += rev;
  out += "\",\"width\":";
  out += width;
  out += ",\"detected\":";
  out += detected;
  out += ",\"faulty\":";
  out += faulty;
  // Follower diagnostics (#306): why a row is blank/stale + bus/heap/clock.
  out += ",\"msSinceRender\":";
  out += String((long)d.msSinceRender);
  out += ",\"secsUntilBlank\":";
  out += String((long)d.secsUntilBlank);
  out += ",\"i2cTx\":";
  out += String((unsigned long)d.i2cTx);
  out += ",\"i2cErr\":";
  out += String((unsigned long)d.i2cErr);
  out += ",\"minHeap\":";
  out += String((unsigned long)d.minHeap);
  out += ",\"sntpSynced\":";
  out += d.sntpSynced ? "true" : "false";
  out += ",\"hmac\":";
  out += d.hmac ? "true" : "false";
  out += '}';
  return out;
}

// GET /settings — tiny: identity, rev, plat, width, phase, vitals. Enough
// for the member ⚙ panel (name/fw line + vitals row) and ota-flash.sh's
// `version` verdict poll. deviceName == effectiveDeviceName: this firmware
// has no rename (chip-id identity only).
inline String followerSettingsJson(const String& name, const char* rev,
                                   int width, const char* phaseName,
                                   const String& leaderName,
                                   const String& leaderHost, int row,
                                   const FollowerVitals& v) {
  String out;
  out.reserve(288);
  out += "{\"deviceName\":";
  followerAppendJsonString(out, name);
  out += ",\"effectiveDeviceName\":";
  followerAppendJsonString(out, name);
  out += ",\"version\":\"";
  out += rev;
  out += "\",\"width\":";
  out += width;
  out += ",\"clusterState\":\"";
  out += phaseName;
  out += "\",\"clusterLeaderName\":";
  followerAppendJsonString(out, leaderName);
  out += ",\"clusterLeaderHost\":";
  followerAppendJsonString(out, leaderHost);
  out += ",\"clusterRow\":";
  out += row;
  followerAppendPlatVitals(out, v);
  out += '}';
  return out;
}
