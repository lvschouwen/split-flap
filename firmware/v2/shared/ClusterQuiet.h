#pragma once
// ClusterQuiet.h — the quiet flag on the cluster ping (#227). The leader owns
// quiet mode (Master/QuietPolicy.h) and tells every member on each ping, so a
// row that loses its leader at night does not start its own clock.
//
// On the wire: `quiet=1|0`, and for a keyed member `qmac=` — an HMAC with that
// member's key over the flag and the ping's own timestamp. The ping's
// canonical message is left alone, so a member that predates the flag still
// verifies every ping and simply ignores the two new params. A keyed member
// accepts the flag only with a valid qmac: piggybacked state is bound to the
// leader like the rest of the ping, and an on-path host cannot flip it. The
// timestamp is the one the ping's own signature and replay mark already
// cover, so a recorded flag cannot be replayed onto a later ping.
//
// A flag that is absent or not accepted changes nothing: the member keeps
// what it last knew. Pure, shared by both member implementations.
//
// Accepted limit: absence is not signed. A host that can rewrite a ping in
// flight can strip both params and so hold a member on its last known value
// (it cannot set one). Closing that means putting the flag into the ping's
// canonical message, which every member of a mixed-revision cluster would
// have to agree on at once; the same host can already drop pings altogether.

#include <Arduino.h>

#include "ClusterHmac.h"  // clusterU64ToStr

#define CLUSTER_PING_QUIET_PARAM "quiet"
#define CLUSTER_PING_QUIET_MAC_PARAM "qmac"

// What the leader appends to a ping body for the flag itself.
inline const char* clusterQuietPingSuffix(bool quiet) {
  return quiet ? "&" CLUSTER_PING_QUIET_PARAM "=1"
               : "&" CLUSTER_PING_QUIET_PARAM "=0";
}

// The message qmac signs.
inline String clusterQuietMsg(uint64_t ts, bool quiet) {
  String m;
  m.reserve(32);
  m += "quiet\n";
  m += clusterU64ToStr(ts);
  m += quiet ? "\n1" : "\n0";
  return m;
}

// A member's reading of the param's value; anything but "1" = off.
inline bool clusterQuietFromPing(const char* value) {
  return value != nullptr && value[0] == '1' && value[1] == '\0';
}

// Does this ping say anything about quiet that the member may act on?
// `macOk` = qmac verified against clusterQuietMsg(ts, <the value sent>) with
// the member's key; only consulted for a keyed member. Returns true and sets
// `quietOut` when the flag is accepted, false when it must be ignored.
inline bool clusterQuietAccept(bool present, const char* value, bool keyed,
                               bool macOk, bool& quietOut) {
  if (!present) return false;
  if (keyed && !macOk) return false;
  quietOut = clusterQuietFromPing(value);
  return true;
}
