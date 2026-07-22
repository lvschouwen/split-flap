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

// Render-busy grace (#326): a contact timeout in the window after a follower
// ACKed a render is almost always the single-core follower heads-down applying
// it — its blocking I2C flap starves the async contact path. Such a timeout
// backs off and retries but does NOT count toward degrade. Sized to cover a
// worst-case normal full-row flap + readback; a genuinely stuck unit (bounded
// by the follower's 30 s show-timeout) outlasts it and degrades honestly.
static const uint32_t CLUSTER_RENDER_GRACE_MS = 6000UL;

// This master's own platform tag (#297). Members report theirs via the
// additive `plat` join/ping-reply key; ABSENT means same-platform (the
// pre-#297 S3 fleet), so only a foreign non-empty plat changes behavior —
// today that is the ESP-01 dumb row ("esp01", #298), which the firmware
// rollout must never stream the S3 image at.
static const char CLUSTER_LEADER_PLAT[] = "esp32s3";

// Per-row unit health distilled from a #294 ping reply. valid stays false
// for members that never delivered one (old firmware, not yet pinged) —
// the UI hides that row's strip instead of reading zeros.
struct ClusterMemberHealth {
  bool valid = false;
  int faulty = 0;
  int detected = 0;
  String faultMask;  // hex bitmap, bit i = unit at position i faulty
  bool wear = false;
};

struct ClusterMemberRuntime {
  bool joined = false;
  bool degraded = false;
  uint8_t failures = 0;
  uint32_t nextAttemptMs = 0;  // backoff gate for the next HTTP attempt
  uint32_t lastContactMs = 0;  // last successful round-trip
  uint32_t lastRenderMs = 0;   // last ACKed render (#326 busy-grace; 0 = never)
  bool renderDirty = false;    // segment changed since the last acked render
  String rev;                  // follower firmware rev (join + ping replies, #276)
  String plat;                 // reported platform, "" = same as leader (#297)
  bool rescue = false;         // rescue-beacon marker (#343) — the member is
                               // boot-looping and wants the stored image back
  uint32_t healthySinceMs = 0; // start of the current continuous joined+
                               // non-rescue window (#343 healthy forgiveness)
  String role;                 // reported deviceRole, "" = pre-#332 peer (#332)
  int reportedWidth = 0;       // join-handshake width fact
  uint32_t logCursor = 0;      // last ingested byte cursor from GET /log (#318 E)
  ClusterMemberHealth health;  // last #294 ping-reply health
  // Cluster-wire auth key (#313 follow-on): minted at the first join, sent in
  // the join body, reused across rejoins, wiped on a config change (runtime
  // reset) and on reboot — so a leader reboot re-mints + re-distributes via
  // the join fan-out. Raw 32 bytes (ClusterHmac.h's CLUSTER_HMAC_KEY_LEN); the
  // crypto lives in the glue, this header stays crypto-free.
  uint8_t hmacKey[32] = {0};
  bool hmacKeyValid = false;
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

// Round-robin fan-out selector (#320). The leader contacts at most ONE member
// per tick: a promote-time catch-up makes every member due at once (all
// un-joined after the runtime reset), and if one is a dead host it burns the
// full HTTP timeout — doing the whole round in a single tick blocks core 0
// past the 5 s task watchdog (the crash that motivated this). Scanning starts
// just AFTER the last-served index and wraps, so a member stuck needing the
// same op every tick (a dead host that never joins) can never starve the
// healthy ones.
//
// `cursor` is the last-served member index (-1 = none served yet, and stale
// values past `count` are normalized); it is advanced to the returned index.
// `needsAction[i]` is true when member i has a due op this tick (the caller
// derives it from clusterMemberNextAction). Returns the chosen index, or -1
// when none are due (cursor left unchanged so progress resumes next tick).
inline int clusterFanoutNext(int& cursor, const bool* needsAction, int count) {
  if (count <= 0 || needsAction == nullptr) return -1;
  int start = (cursor < 0 || cursor >= count) ? -1 : cursor;
  for (int step = 1; step <= count; step++) {
    int i = (start + step) % count;  // start == -1 → first probe is index 0
    if (needsAction[i]) {
      cursor = i;
      return i;
    }
  }
  return -1;
}

inline void clusterMemberOnSuccess(ClusterMemberRuntime& m, uint32_t nowMs) {
  m.failures = 0;
  m.degraded = false;
  m.lastContactMs = nowMs;
  m.nextAttemptMs = nowMs;
}

// #326: stamp the moment a render was ACKed — the follower is about to go
// heads-down flapping it. Refreshed ONLY on a successful render (never on a
// failed one), so a dead follower's stamp goes stale and the grace below
// cannot mask a real outage.
inline void clusterMemberNoteRender(ClusterMemberRuntime& m, uint32_t nowMs) {
  m.lastRenderMs = nowMs;
}

inline void clusterMemberOnFailure(ClusterMemberRuntime& m, uint32_t nowMs) {
  // #326 render-busy grace: a timeout shortly after an ACKed render is the
  // follower applying it, not a dead link — retry soon, but don't degrade.
  // Gated on lastRenderMs != 0 so a never-rendered member counts normally.
  if (m.lastRenderMs != 0 &&
      (uint32_t)(nowMs - m.lastRenderMs) < CLUSTER_RENDER_GRACE_MS) {
    m.nextAttemptMs = nowMs + CLUSTER_RETRY_BASE_MS;
    return;
  }
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

inline bool clusterExtractJsonBool(const String& body, const char* key,
                                   bool def) {
  int at = clusterFindJsonKey(body, key);
  if (at < 0) return def;
  if (body.startsWith("true", at)) return true;
  if (body.startsWith("false", at)) return false;
  return def;
}

// Distills a #294 ping reply's health keys. faulty + faultMask are the
// load-bearing pair — a reply without both is a pre-#294 follower and
// parses to invalid (never to zero faults).
inline bool clusterParsePingHealth(const String& body,
                                   ClusterMemberHealth& out) {
  out = ClusterMemberHealth{};
  if (clusterFindJsonKey(body, "faulty") < 0) return false;
  String mask = clusterExtractJsonString(body, "faultMask");
  if (mask.length() == 0) return false;
  out.valid = true;
  out.faulty = clusterExtractJsonInt(body, "faulty", 0);
  out.detected = clusterExtractJsonInt(body, "detected", 0);
  out.faultMask = mask;
  out.wear = clusterExtractJsonBool(body, "wear", false);
  return true;
}

// #295 sticky leadership: a join 409 whose body carries this marker means
// the follower is clustered to a DIFFERENT live leader — this board lost a
// promote race (or came back from the dead) and must demote.
inline bool clusterJoinRejectedOtherLeader(const String& body) {
  return clusterExtractJsonString(body, "error") == "other-leader";
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

// SSRF guard (#313): a member host becomes an esp_http_client URL the
// leader POSTs its running firmware to (join → render → #276 rollout). A
// public host smuggled into /cluster/config would exfiltrate that image to
// the internet, so a non-empty host must resolve to the LAN only — a
// private-IPv4 literal, a `.local` mDNS name, or localhost. Empty host =
// the leader's own row (never dialed). Mirrors clusterCorsPrivateIpv4 in
// ClusterDigest.h (kept separate to avoid an include cycle: this header is
// upstream of ClusterDigest.h).
inline bool clusterHostPrivateIpv4(const String& host) {
  int octets[4];
  int value = 0, digits = 0, index = 0;
  for (unsigned int i = 0; i <= host.length(); i++) {
    char c = i < host.length() ? host[i] : '.';
    if (c == '.') {
      if (digits == 0 || digits > 3 || index >= 4) return false;
      octets[index++] = value;
      value = 0;
      digits = 0;
    } else if (c >= '0' && c <= '9') {
      value = value * 10 + (c - '0');
      if (value > 255) return false;
      digits++;
    } else {
      return false;
    }
  }
  if (index != 4) return false;
  if (octets[0] == 10 || octets[0] == 127) return true;
  if (octets[0] == 192 && octets[1] == 168) return true;
  if (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) return true;
  return false;
}

inline bool clusterHostIsLanTarget(const String& host) {
  if (host.equalsIgnoreCase("localhost")) return true;
  String lower = host;
  lower.toLowerCase();
  if (lower.endsWith(".local") && host.length() > 6) return true;
  return clusterHostPrivateIpv4(host);
}

// Shape-parse of the stored string. Returns false on malformed input
// (bad field count, non-numeric, out-of-range, oversized host, or a
// non-LAN host — see clusterHostIsLanTarget). "" parses to an empty table
// (cluster disabled) and returns true.
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
    // Hosts become esp_http_client URLs — visible ASCII only (the same bar
    // the follower holds its leaderHost to at the web boundary).
    for (unsigned int c = 0; c < host.length(); c++) {
      if ((unsigned char)host[c] <= 0x20 || (unsigned char)host[c] > 0x7E) {
        return false;
      }
    }
    // SSRF guard (#313): a non-empty host must be a LAN target — the leader
    // streams its firmware there, so a public host is exfiltration bait.
    if (host.length() > 0 && !clusterHostIsLanTarget(host)) return false;
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
