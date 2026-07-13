#pragma once
// ClusterLeaderPolicy.h — pure leader-side cluster logic (#273, epic #270;
// spec docs/superpowers/specs/2026-07-13-multi-display-cluster-design.md),
// natively tested by test_cluster_leader_policy. No networking — the HTTP
// verbs live in ClusterLeader.cpp's clusterTask; this header owns the
// member-table wire format for NVS and the per-member supervision state
// machine (join → render → ping, backoff → degraded, re-join recovery).
//
// Member-table storage format (one NVS string, key clMembers):
// `host|row|col|width` entries joined by `;`. An EMPTY host marks the
// leader's own row — its segment goes through displayEnqueue instead of
// HTTP. The parsed table is re-validated by validateMemberTable()
// (ClusterLayout.h) before every use; this parser only guards shape.

#include <Arduino.h>

#include "ClusterLayout.h"

// Follower flip lead: commitAtMs = leader-now + this. Both ends NTP-sync,
// so every row starts flipping within tens of ms (the follower clamps a
// bogus far-future value at CLUSTER_COMMIT_MAX_DELAY_MS).
static const uint32_t CLUSTER_COMMIT_LEAD_MS = 400UL;

// Idle keep-alive cadence — feeds the follower's 25 s contact-fresh window
// with ~2.5 pings of margin.
static const uint32_t CLUSTER_LEADER_PING_MS = 10000UL;

// Failure handling: exponential backoff 1-2-4-8 s (capped), degraded after
// 3 straight failures. A degraded member keeps being retried at the capped
// cadence; recovery goes through a fresh join (idempotent — the handshake
// ends with a re-send of the current segment).
static const uint32_t CLUSTER_RETRY_BASE_MS = 1000UL;
static const uint32_t CLUSTER_RETRY_MAX_MS = 8000UL;
static const uint8_t CLUSTER_DEGRADED_AFTER_FAILURES = 3;

struct ClusterMemberRuntime {
  bool joined = false;
  bool degraded = false;
  uint8_t failures = 0;
  uint32_t nextAttemptMs = 0;  // backoff gate for the next HTTP attempt
  uint32_t lastContactMs = 0;  // last successful round-trip
  bool renderDirty = false;    // segment changed since the last acked render
  String rev;                  // follower firmware rev (join reply, #276)
  int reportedWidth = 0;       // join-handshake width fact
};

enum class ClusterLeaderAction : uint8_t { None = 0, Join, Render, Ping };

// The leader's own row rides the member table with an empty host.
inline bool clusterMemberIsSelf(const ClusterMemberDef& def) {
  return def.host[0] == '\0';
}

// One member's next outbound op. Priority: membership before content
// before keep-alive; the backoff gate outranks everything.
inline ClusterLeaderAction clusterMemberNextAction(
    const ClusterMemberRuntime& m, uint32_t nowMs) {
  if ((int32_t)(nowMs - m.nextAttemptMs) < 0) return ClusterLeaderAction::None;
  if (!m.joined) return ClusterLeaderAction::Join;
  if (m.renderDirty) return ClusterLeaderAction::Render;
  if (nowMs - m.lastContactMs >= CLUSTER_LEADER_PING_MS) {
    return ClusterLeaderAction::Ping;
  }
  return ClusterLeaderAction::None;
}

inline void clusterMemberOnSuccess(ClusterMemberRuntime& m, uint32_t nowMs) {
  m.failures = 0;
  m.degraded = false;
  m.lastContactMs = nowMs;
  m.nextAttemptMs = nowMs;
}

inline void clusterMemberOnFailure(ClusterMemberRuntime& m, uint32_t nowMs) {
  if (m.failures < 255) m.failures++;
  if (m.failures >= CLUSTER_DEGRADED_AFTER_FAILURES) {
    m.degraded = true;
    // Recovery is a fresh join: idempotent, and the handshake ends with a
    // re-send of the current segment.
    m.joined = false;
  }
  uint32_t backoff = CLUSTER_RETRY_BASE_MS << (m.failures - 1 > 3 ? 3 : m.failures - 1);
  if (backoff > CLUSTER_RETRY_MAX_MS) backoff = CLUSTER_RETRY_MAX_MS;
  m.nextAttemptMs = nowMs + backoff;
}

// A 409 "not clustered" reply: the follower lost its membership (left /
// factory reset) — not a transport failure, so no backoff, but the next
// action must be a fresh join.
inline void clusterMemberOnNotClustered(ClusterMemberRuntime& m) {
  m.joined = false;
}

// --- reply parsing ------------------------------------------------------------------
// Fixed-shape extraction from our own join/ping reply bodies — not general
// JSON (SettingsJson.h precedent: no ArduinoJson for fixed shapes). The
// leading quote in the needle keys off exact names.

inline int clusterFindJsonKey(const String& body, const char* key) {
  String needle = String("\"") + key + "\":";
  int at = body.indexOf(needle);
  return at < 0 ? -1 : at + (int)needle.length();
}

inline String clusterExtractJsonString(const String& body, const char* key) {
  int at = clusterFindJsonKey(body, key);
  if (at < 0 || at >= (int)body.length() || body[at] != '"') return "";
  int end = body.indexOf('"', at + 1);
  if (end < 0) return "";
  return body.substring(at + 1, end);
}

inline int clusterExtractJsonInt(const String& body, const char* key, int def) {
  int at = clusterFindJsonKey(body, key);
  if (at < 0) return def;
  const char* text = body.c_str() + at;
  char* end = nullptr;
  long parsed = strtol(text, &end, 10);
  if (end == text) return def;
  return (int)parsed;
}

// --- member-table wire format (NVS) ------------------------------------------------

inline String clusterTableToString(const ClusterMemberTable& table) {
  String out;
  for (int i = 0; i < table.count; i++) {
    if (i) out += ';';
    out += table.members[i].host;
    out += '|';
    out += (int)table.members[i].row;
    out += '|';
    out += (int)table.members[i].col;
    out += '|';
    out += (int)table.members[i].width;
  }
  return out;
}

// Strictly-decimal field parse ("" and stray chars reject; the wire format
// is machine-written, so leniency only hides corruption).
inline bool clusterParseFieldInt(const String& field, long minVal, long maxVal,
                                 long& out) {
  if (field.length() == 0) return false;
  const char* text = field.c_str();
  char* end = nullptr;
  long parsed = strtol(text, &end, 10);
  if (end == text || *end != '\0') return false;
  if (parsed < minVal || parsed > maxVal) return false;
  out = parsed;
  return true;
}

// Shape-parse of the stored string. Returns false on malformed input
// (bad field count, non-numeric, out-of-range, oversized host). "" parses
// to an empty table (cluster disabled) and returns true.
inline bool clusterTableFromString(const String& stored,
                                   ClusterMemberTable& outTable) {
  outTable = ClusterMemberTable{};
  if (stored.length() == 0) return true;

  int entryStart = 0;
  while (entryStart <= (int)stored.length()) {
    int entryEnd = stored.indexOf(';', entryStart);
    if (entryEnd < 0) entryEnd = stored.length();
    if (outTable.count >= CLUSTER_MAX_MEMBERS) return false;
    String entry = stored.substring(entryStart, entryEnd);

    // host|row|col|width — exactly four fields.
    int p1 = entry.indexOf('|');
    int p2 = p1 < 0 ? -1 : entry.indexOf('|', p1 + 1);
    int p3 = p2 < 0 ? -1 : entry.indexOf('|', p2 + 1);
    if (p3 < 0 || entry.indexOf('|', p3 + 1) >= 0) return false;

    String host = entry.substring(0, p1);
    if (host.length() > CLUSTER_HOST_MAX_LEN) return false;
    long row, col, width;
    if (!clusterParseFieldInt(entry.substring(p1 + 1, p2), 0, 255, row) ||
        !clusterParseFieldInt(entry.substring(p2 + 1, p3), 0, 255, col) ||
        !clusterParseFieldInt(entry.substring(p3 + 1), 0, 255, width)) {
      return false;
    }

    ClusterMemberDef& def = outTable.members[outTable.count++];
    memcpy(def.host, host.c_str(), host.length());
    def.host[host.length()] = '\0';
    def.row = (uint8_t)row;
    def.col = (uint8_t)col;
    def.width = (uint8_t)width;

    entryStart = entryEnd + 1;
    if (entryEnd == (int)stored.length()) break;
  }
  return true;
}
