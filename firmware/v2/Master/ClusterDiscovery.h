#pragma once
// ClusterDiscovery.h — pure logic for cluster board discovery (#274, epic
// #270): every v2 master advertises `_splitflap._tcp` (TXT: name, rev,
// width) and the leader's Cluster card browses for candidates via the
// staged POST/GET `/cluster/discover` pair (same contract as
// /mqtt/discover). No mDNS stack and no networking — everything here is
// exercised by `pio test -e native` (test_cluster_discovery). The netTask
// drain fills ClusterDiscoveredBoard structs from MDNS.queryService()
// answers and hands them here.

#include <Arduino.h>

#include "MdnsDiscovery.h"  // normalizeMdnsHostname, mdnsAppendJsonString

#define CLUSTER_DISCOVER_MAX_BOARDS 8

struct ClusterDiscoveredBoard {
  String name;  // mDNS hostname, without ".local"
  String ip;    // dotted quad, empty when the answer carried none
  String rev;   // TXT rev — firmware short-hash, "" when absent
  int width;    // TXT width — 0 when absent/unparseable ("?" in the UI)
};

// TXT values come off the wire: strictly-decimal parse, anything else is 0
// (unknown). Widths ride uint8_t everywhere else in the cluster layer.
inline int clusterParseTxtWidth(const String& value) {
  if (value.length() == 0) return 0;
  const char* text = value.c_str();
  char* end = nullptr;
  long parsed = strtol(text, &end, 10);
  if (end == text || *end != '\0') return 0;
  if (parsed < 0 || parsed > 255) return 0;
  return (int)parsed;
}

// The member table stores this host verbatim. A raw IP always resolves
// for esp_http_client; "<name>.local" is the fallback when the mDNS
// answer carried no address.
inline String clusterDiscoveredHost(const ClusterDiscoveredBoard& board) {
  if (board.ip.length() > 0) return board.ip;
  return board.name + ".local";
}

// The GET /cluster/discover "done" payload:
// {"status":"done","boards":[{"name":…,"host":…,"rev":…,"width":N},…]}
// The browsing master sees its own advertisement — filter it out (mDNS
// names are case-insensitive), along with nameless answers.
inline String buildClusterDiscoverJson(const ClusterDiscoveredBoard* boards,
                                       size_t count, const String& selfName) {
  String out;
  out.reserve(40 + count * 112);
  out += "{\"status\":\"done\",\"boards\":[";
  bool first = true;
  for (size_t i = 0; i < count; i++) {
    const ClusterDiscoveredBoard& b = boards[i];
    if (b.name.length() == 0) continue;
    if (b.name.equalsIgnoreCase(selfName)) continue;
    if (!first) out += ',';
    first = false;
    out += "{\"name\":";
    mdnsAppendJsonString(out, b.name);
    out += ",\"host\":";
    mdnsAppendJsonString(out, clusterDiscoveredHost(b));
    out += ",\"rev\":";
    mdnsAppendJsonString(out, b.rev);
    out += ",\"width\":";
    out += String(b.width);
    out += '}';
  }
  out += "]}";
  return out;
}
