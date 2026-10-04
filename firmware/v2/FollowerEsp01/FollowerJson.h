#pragma once
// FollowerJson.h — the follower's wire-reply builders (#298), natively
// tested by test_follower_json. Pure String assembly: the join/ping replies
// the S3 leader parses (#272 trio + #294 health keys + the #297 additive
// plat/vitals block), the tiny /settings JSON the member ⚙ panel reads,
// and /cluster/health. appendJsonString mirrors the fleet-wide escaping
// rule.

#include <Arduino.h>

#include "FollowerResetLog.h"
#include "ClusterForeign.h"
#include "JsonEscape.h"  // appendJsonString
#include "ClusterWireGuards.h"  // ClusterRowHealth + the health-key block
#include "FollowerBusRecovery.h"
#include "UnitHealth.h"

#define FOLLOWER_PLAT "esp01"

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

// #343 additive: only a rescue-beacon boot emits the marker (`"rescue":1`
// — an int so the leader's existing bare-number extractor reads it);
// absent = healthy, so pre-#343 leaders see an unchanged reply.
inline void followerAppendRescue(String& out, bool rescue) {
  if (rescue) out += ",\"rescue\":1";
}

// POST /cluster/join reply — the v2 handshake shape plus plat/vitals.
inline String followerJoinReplyJson(const String& name, const char* rev,
                                    const ClusterRowHealth& h,
                                    const FollowerVitals& v, bool rescue) {
  String out;
  out.reserve(224);
  out += "{\"name\":";
  appendJsonString(out, name);
  out += ",\"rev\":\"";
  out += rev;
  out += '"';
  clusterAppendHealthKeys(out, h);
  followerAppendPlatVitals(out, v);
  followerAppendRescue(out, rescue);
  out += ",\"protocol\":1}";
  return out;
}

// POST /cluster/ping reply — state/epoch/seq trio, then the health keys +
// rev (the leader's rev-refresh fact), then plat/vitals.
inline String followerPingReplyJson(const char* phaseName, uint32_t epoch,
                                    uint32_t seq,
                                    const ClusterRowHealth& h,
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
  clusterAppendHealthKeys(out, h);
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
  // #435: since-boot low-water of the ONE painted 4 KB cont stack the whole
  // superloop runs on — ESP.getFreeContStack() is already the minimum (paint
  // check), no sampling needed. The 8266's answer to the S3's #415 hwm.
  uint32_t stackFree = 0;
  bool sntpSynced = false;
  bool hmac = false;  // #313 follow-on: enforcing signed leader-wire requests
  ForeignContactStats foreign;  // #358: refused foreign-leader contacts
  uint32_t nowMs = 0;           // for the foreign block's msSince + bus deadMs
  BusRecoveryState bus;         // #488: row-wide bus-death recovery
  // #503: reset history, newest first (this boot leads). Null = omit the key.
  const FollowerResetLogBlob* resets = nullptr;
};

inline String followerClusterHealthJson(
    const char* phaseName, const String& leaderName, const String& leaderHost,
    int row, uint32_t epoch, uint32_t seq, const String& segment,
    const char* rev, int width, int detected, int faulty,
    const FollowerClusterDiag& d) {
  String out;
  out.reserve(608);  // ~480 before the #503 resets array
  out += "{\"state\":\"";
  out += phaseName;
  out += "\",\"leaderName\":";
  appendJsonString(out, leaderName);
  out += ",\"leaderHost\":";
  appendJsonString(out, leaderHost);
  out += ",\"row\":";
  out += row;
  out += ",\"epoch\":";
  out += String((unsigned long)epoch);
  out += ",\"seq\":";
  out += String((unsigned long)seq);
  out += ",\"segment\":";
  appendJsonString(out, segment);
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
  out += ",\"stackFree\":";
  out += String((unsigned long)d.stackFree);
  out += ",\"sntpSynced\":";
  out += d.sntpSynced ? "true" : "false";
  out += ",\"hmac\":";
  out += d.hmac ? "true" : "false";
  foreignContactAppendJson(out, d.foreign, d.nowMs);  // #358
  out += ",\"bus\":{\"dead\":";  // #488
  out += d.bus.dead ? "true" : "false";
  out += ",\"deadMs\":";
  out += String((unsigned long)(d.bus.dead ? d.nowMs - d.bus.deadSinceMs : 0));
  out += ",\"episodes\":";
  out += String((unsigned long)d.bus.episodes);
  out += ",\"recovered\":";
  out += String((unsigned long)d.bus.recovered);
  out += ",\"attempts\":";
  out += String((unsigned long)d.bus.attempts);
  out += ",\"lastStatus\":";
  out += String((int)d.bus.lastStatus);
  out += ",\"lastDeadMs\":";
  out += String((unsigned long)d.bus.lastDeadMs);
  out += '}';
  if (d.resets != nullptr) {
    // "<reason>:<exccause>:<epc1>:<excvaddr>" per boot since the last power
    // cycle (FollowerResetLog.h).
    out += ",\"resets\":[";
    int n = followerResetLogCount(*d.resets);
    for (int i = 0; i < n; i++) {
      char entry[32];
      followerResetEntryFormat(d.resets->e[i], entry, sizeof(entry));
      if (i > 0) out += ',';
      out += '"';
      out += entry;
      out += '"';
    }
    out += ']';
  }
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
                                   const FollowerVitals& v,
                                   int txPowerDbm10, bool reflashOnBoot) {
  String out;
  out.reserve(328);
  out += "{\"deviceName\":";
  appendJsonString(out, name);
  out += ",\"effectiveDeviceName\":";
  appendJsonString(out, name);
  out += ",\"version\":\"";
  out += rev;
  out += "\",\"width\":";
  out += width;
  out += ",\"clusterState\":\"";
  out += phaseName;
  out += "\",\"clusterLeaderName\":";
  appendJsonString(out, leaderName);
  out += ",\"clusterLeaderHost\":";
  appendJsonString(out, leaderHost);
  out += ",\"clusterRow\":";
  out += row;
  followerAppendPlatVitals(out, v);
  out += ",\"txPower\":";  // #508: WiFi TX power cap x10 (dBm)
  out += txPowerDbm10;
  // #513: same key and type as the S3's, so one campaign script reads both.
  out += ",\"reflashOnBoot\":";
  out += reflashOnBoot ? "true" : "false";
  out += '}';
  return out;
}
