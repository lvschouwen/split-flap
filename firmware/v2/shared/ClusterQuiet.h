#pragma once
// ClusterQuiet.h — the quiet flag on the cluster ping (#227). The leader owns
// quiet mode (Master/QuietPolicy.h) and tells every member on each ping, so a
// row that loses its leader at night does not start its own clock.
//
// Additive and outside the signed canonical: a member that predates the key
// ignores it, and a leader that predates it sends nothing, which reads as
// "not quiet". Pure, shared by both member implementations.

#define CLUSTER_PING_QUIET_PARAM "quiet"

// What the leader appends to a ping body (before any ts/mac).
inline const char* clusterQuietPingSuffix(bool quiet) {
  return quiet ? "&" CLUSTER_PING_QUIET_PARAM "=1"
               : "&" CLUSTER_PING_QUIET_PARAM "=0";
}

// A member's reading of the param's value; absent or anything but "1" = off.
inline bool clusterQuietFromPing(const char* value) {
  return value != nullptr && value[0] == '1' && value[1] == '\0';
}
