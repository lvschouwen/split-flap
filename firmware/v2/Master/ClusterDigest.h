#pragma once
// ClusterDigest.h — pure single-pane-of-glass logic (#294 + #295, epic
// #270; spec docs/superpowers/specs/2026-07-14-cluster-single-pane-design.md),
// natively tested by test_cluster_digest. No networking — glue lives in
// ClusterLeader.cpp (digest build + ping piggyback), ClusterFollower.cpp
// (digest storage, promote) and WebEndpoints.cpp (reply splice, CORS).
//
// The core #294 trick: the leader already contacts every follower each
// tick, so the ping is piggybacked in BOTH directions instead of adding a
// message bus — per-row unit health rides the ping REPLY up (fragment +
// parse here), and the cluster digest (wall rows + member table + the
// shared /cluster/status JSON) rides the ping BODY down, so any pane can
// render the whole wall. All keys are additive: pre-#294 firmware simply
// lacks them and degrades to the #277 behavior.

#include <Arduino.h>

#include "ClusterLeader.h"
#include "ClusterRolloutPolicy.h"  // clusterMemberPlatForeign (#321 successors)
#include "SettingsJson.h"  // appendJsonString
#include "UnitHealth.h"    // UnitFacts + the faulty predicate

// Hex fault bitmap for one row: bit i = unit at position i is faulty
// (same statusValid gate as computeFaultyUnitCount — unread units are
// unknown, not faulty). Fixed width ceil(width/4) nibbles so the string
// length itself carries the row width; enough for a strip — per-unit
// DETAIL is rung 3's browser fan-out to the member's own /units/health.
inline size_t clusterFaultMaskHex(const UnitFacts* units, int width,
                                  char* buf, size_t cap) {
  if (width > 32) width = 32;  // uint32 bitmap; real displays are ≤16
  if (width <= 0) {            // no units, no mask (%0*X would still print "0")
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

// The additive #294 keys of the follower's ping reply, spliced after the
// #272 state/epoch/seq trio (leading comma). rev refreshes on every ping
// so the leader's rev fact survives its own reboot without a re-join.
inline String clusterPingHealthJson(const UnitFacts* units, int width,
                                    int detected, int faulty, bool wear,
                                    const char* rev) {
  char mask[16];
  clusterFaultMaskHex(units, width, mask, sizeof(mask));
  String out;
  out.reserve(96);
  out += ",\"width\":";
  out += width;
  out += ",\"detected\":";
  out += detected;
  out += ",\"faulty\":";
  out += faulty;
  out += ",\"faultMask\":\"";
  out += mask;
  out += "\",\"wear\":";
  out += wear ? "true" : "false";
  out += ",\"rev\":";
  appendJsonString(out, String(rev));
  return out;
}

// The one /cluster/status wire shape (#273 keys + #294 health), shared by
// the GET endpoint and the digest below so follower panes can feed the
// same browser renderer the leader uses.
inline String clusterStatusJson(const ClusterLeaderStatus& st) {
  String out;
  out.reserve(192 + st.memberCount * 200);
  out += "{\"enabled\":";
  out += st.enabled ? "true" : "false";
  out += ",\"epoch\":";
  out += String((unsigned long)st.epoch);
  out += ",\"seq\":";
  out += String((unsigned long)st.seq);
  out += ",\"members\":[";
  for (int i = 0; i < st.memberCount; i++) {
    const ClusterLeaderMemberStatus& m = st.members[i];
    if (i) out += ',';
    out += "{\"host\":";
    appendJsonString(out, m.host);
    out += ",\"self\":";
    out += m.host.length() == 0 ? "true" : "false";
    out += ",\"row\":";
    out += m.row;
    out += ",\"col\":";
    out += m.col;
    out += ",\"width\":";
    out += m.width;
    out += ",\"joined\":";
    out += m.joined ? "true" : "false";
    out += ",\"degraded\":";
    out += m.degraded ? "true" : "false";
    out += ",\"failures\":";
    out += m.failures;
    out += ",\"rev\":";
    appendJsonString(out, m.rev);
    if (m.plat.length() > 0) {
      // #297 additive: absent = same platform as the leader (S3 fleet).
      out += ",\"plat\":";
      appendJsonString(out, m.plat);
    }
    out += ",\"reportedWidth\":";
    out += m.reportedWidth;
    if (m.healthValid) {
      // #294 additive block — absent entirely for members that never
      // delivered a health-carrying ping reply.
      out += ",\"faulty\":";
      out += m.faulty;
      out += ",\"detected\":";
      out += m.detected;
      out += ",\"faultMask\":";
      appendJsonString(out, m.faultMask);
      out += ",\"wear\":";
      out += m.wear ? "true" : "false";
    }
    out += ",\"updating\":";
    out += m.updating ? "true" : "false";
    out += ",\"updateBlocked\":";
    out += m.updateBlocked ? "true" : "false";
    out += ",\"hmac\":";  // #313 follow-on: leader is signing to this member
    out += m.hmac ? "true" : "false";
    out += '}';
  }
  out += "],\"rollout\":{\"phase\":";
  appendJsonString(out, st.rolloutPhase);
  out += ",\"host\":";
  appendJsonString(out, st.rolloutHost);
  out += ",\"sent\":";
  out += String((unsigned long)st.rolloutSent);
  out += ",\"total\":";
  out += String((unsigned long)st.rolloutTotal);
  out += ",\"imageVerifyFailed\":";
  out += st.rolloutImageFailed ? "true" : "false";
  // #304 Part B: the stored ESP-01 image + the on-demand relay push. The
  // Cluster card's upload control reads followerImage; the per-member "Update
  // firmware" button reads followerPush.
  out += "},\"followerImage\":{\"present\":";
  out += st.followerImagePresent ? "true" : "false";
  out += ",\"rev\":";
  appendJsonString(out, st.followerImageRev);
  out += "},\"followerPush\":{\"phase\":";
  appendJsonString(out, st.followerPushPhase);
  out += ",\"host\":";
  appendJsonString(out, st.followerPushHost);
  out += ",\"sent\":";
  out += String((unsigned long)st.followerPushSent);
  out += ",\"total\":";
  out += String((unsigned long)st.followerPushTotal);
  out += ",\"result\":";
  appendJsonString(out, st.followerPushResult);
  out += "}}";
  return out;
}

// The digest the leader piggybacks on every ping POST (#294 rung 2): wall
// rows + the member-table wire string + the status JSON above. The table
// and the leader's own host are what make a follower promotable (#295) —
// together they are everything a successor needs. gen bumps only when the
// content changed (the leader compares digests between builds) so the
// follower UI can gate re-renders cheaply.
// #321: the leader's ordered eligible-successor list — the member indices of
// the S3 followers (in table order) that may take over if the leader dies.
// Self (empty host) and foreign-platform members (ESP-01, #297) are excluded:
// only another S3 can promote. Rides the digest so each follower learns its
// own rank by matching its `you` index against this list.
inline String clusterSuccessorList(const ClusterLeaderStatus& st) {
  String out;
  // Two passes so a width-0 warm-standby backup (#333 — no row of its own to
  // lose, spare capacity) outranks the rendering rows: preferred successors
  // (rank 0) first, then the rendering S3 followers, each in table order.
  for (int pass = 0; pass < 2; pass++) {
    const bool wantBackup = (pass == 0);
    for (int i = 0; i < st.memberCount; i++) {
      const ClusterLeaderMemberStatus& m = st.members[i];
      if (m.host.length() == 0) continue;  // the leader's own row
      if (clusterMemberPlatForeign(m.plat, CLUSTER_LEADER_PLAT)) {
        continue;  // ESP-01 / foreign board — never a takeover candidate
      }
      if ((m.width == 0) != wantBackup) continue;  // this pass's tier only
      if (out.length()) out += ',';
      out += String(i);
    }
  }
  return out;
}

inline String clusterBuildDigest(uint32_t gen, const String& leaderName,
                                 const String& leaderHost,
                                 const String& tableSpec, const String* rows,
                                 int rowCount, const ClusterLeaderStatus& st,
                                 uint32_t holdMs = 0) {
  String out;
  out.reserve(256 + rowCount * 40 + st.memberCount * 200);
  out += "{\"gen\":";
  out += String((unsigned long)gen);
  out += ",\"leader\":{\"name\":";
  appendJsonString(out, leaderName);
  out += ",\"host\":";
  appendJsonString(out, leaderHost);
  out += "},\"table\":";
  appendJsonString(out, tableSpec);
  out += ",\"rows\":[";
  for (int i = 0; i < rowCount; i++) {
    if (i) out += ',';
    appendJsonString(out, rows[i]);
  }
  out += "],\"succ\":";
  appendJsonString(out, clusterSuccessorList(st));
  out += ",\"hold\":";
  out += String((unsigned long)holdMs);  // #321: 0 unless a reboot is imminent
  out += ",\"status\":";
  out += clusterStatusJson(st);
  out += '}';
  return out;
}

// #295 promote: turns the digest-carried member table into the successor's
// own config — the promoting follower's entry (selfIndex, from the ping's
// `you` param) becomes the empty-host self row, and the dead leader's
// empty-host row gets its host filled in (it stays on the wall; the old
// leader rejoins as a plain member once it demotes). Fails rather than
// guesses: bad index, an entry that is already self, or a leader row we
// have no host for all reject — the result feeds clusterLeaderStageConfig,
// which re-validates the geometry.
inline bool clusterPromoteTransform(const String& tableSpec, int selfIndex,
                                    const String& oldLeaderHost, String& out) {
  ClusterMemberTable table;
  if (!clusterTableFromString(tableSpec, table)) return false;
  if (selfIndex < 0 || selfIndex >= table.count) return false;
  if (table.members[selfIndex].host[0] == '\0') return false;
  if (oldLeaderHost.length() > CLUSTER_HOST_MAX_LEN) return false;

  for (int i = 0; i < table.count; i++) {
    if (i == selfIndex || table.members[i].host[0] != '\0') continue;
    if (oldLeaderHost.length() == 0) return false;  // would mint a 2nd self
    memcpy(table.members[i].host, oldLeaderHost.c_str(),
           oldLeaderHost.length());
    table.members[i].host[oldLeaderHost.length()] = '\0';
  }
  table.members[selfIndex].host[0] = '\0';
  out = clusterTableToString(table);
  return true;
}

// CORS origin gate (#294 rung 3): reflected back only for origins that can
// only exist inside the LAN — private IPv4 literals, .local names and
// localhost, all http-only (the boards serve plain http; any https or
// public origin is by definition not another pane of this wall). Applied
// to the per-member maintenance surface only.
inline bool clusterCorsPrivateIpv4(const String& host) {
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

// Digest acceptance gate (review hardening): the follower re-serves the
// stored digest RAW inside its /cluster/digest wrapper, so the string must
// be exactly ONE balanced JSON object — trailing top-level data would
// inject fields into the wrapper. String-aware brace scan, no parser; the
// glue additionally requires the ping to come from the joined leader's IP.
#define CLUSTER_DIGEST_MAX_LEN 8192

inline bool clusterDigestShapeOk(const String& digest) {
  if (digest.length() == 0 || digest.length() > CLUSTER_DIGEST_MAX_LEN) {
    return false;
  }
  if (digest[0] != '{') return false;
  int depth = 0;
  bool inString = false, escaped = false;
  for (unsigned int i = 0; i < digest.length(); i++) {
    char c = digest[i];
    if (inString) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') inString = false;
      continue;
    }
    if (c == '"') inString = true;
    else if (c == '{' || c == '[') depth++;
    else if (c == '}' || c == ']') {
      if (--depth < 0) return false;
      // The root object may only close at the very last character —
      // anything after it is top-level injection.
      if (depth == 0 && i != digest.length() - 1) return false;
    }
  }
  return depth == 0 && !inString;
}

// The per-member surface the CORS gate opens (#294 rung 3): reads +
// maintenance a wall pane manages on another member's behalf. Deliberately
// closed: /firmware/* (fleet convergence owns cross-board updates),
// /cluster/* (leader-driven wire + local promote), WiFi (off-limits from
// the wall UI — re-pointing a member's WiFi remotely can strand it).
inline bool clusterCorsPathAllowed(const String& path) {
  // "/" = the per-card settings save (POST + ajax=1): the panel's
  // device-name edit rides it; the GET just serves the page.
  if (path == "/" || path == "/settings" || path == "/units/health" ||
      path == "/units/health/refresh" || path == "/system/stats" ||
      path == "/log" || path == "/log/flash" || path == "/reboot" ||
      path == "/reflash-units") {
    // #304: board-level unit reflash from the wall panel. Note the ESP-01
    // follower's FollowerCors.h copy additionally opens /firmware/master —
    // a deliberate divergence: S3 members update via #276 fleet convergence,
    // so /firmware/* stays closed HERE.
    return true;
  }
  return path.startsWith("/unit/");
}

inline bool clusterCorsOriginAllowed(const String& origin) {
  if (!origin.startsWith("http://")) return false;
  String host = origin.substring(7);
  int cut = host.indexOf(':');
  if (cut < 0) cut = host.indexOf('/');
  if (cut >= 0) host = host.substring(0, cut);
  if (host.length() == 0) return false;
  if (host.equalsIgnoreCase("localhost")) return true;
  String lower = host;
  lower.toLowerCase();
  if (lower.endsWith(".local") && host.length() > 6) return true;
  return clusterCorsPrivateIpv4(host);
}

// CSRF gate (#313): the pre-existing CORS logic above only ADDED a response
// header — it never blocked the request, so any web page a LAN user opened
// could drive a mutating form-POST (multipart triggers no preflight) at the
// board. Browsers attach `Origin` to every POST, so the enforced rule is:
// a state-changing request (POST) that carries an Origin which is NOT a LAN
// pane is cross-site forgery and must be refused before the handler runs.
// Server-to-server cluster traffic (the leader's esp_http_client) sends no
// Origin and passes; the board's own LAN web UI sends a LAN origin and
// passes; a public/https origin is refused. Method-based, so every mutating
// POST — present and future — is covered without a path allowlist to drift.
inline bool clusterCsrfRejectPost(bool isPost, bool hasOrigin,
                                  const String& origin) {
  return isPost && hasOrigin && !clusterCorsOriginAllowed(origin);
}
