#pragma once
// ClusterFollower.h — follower-side cluster service glue (#272, epic #270).
// The decisions live in ClusterFollowerPolicy.h (natively tested); this
// module owns the state instance, its mutex, the NVS membership record and
// the commitAt-delayed render staging.
//
// Task/lock contract: async handlers call the clusterFollowerHandle*()
// entries — POD/String staging under the module mutex only (the
// webEndpointsLoop discipline); NVS writes and displayEnqueue happen in
// netTask's clusterFollowerServiceTick(). clockTask reads the mutex-copied
// view for its content gate. Lock ordering: never call into web-domain
// locked code from inside this module — webStateMutex may wrap calls INTO
// it (webEndpointsLoop drain re-check), never the reverse.

#include <Arduino.h>

#include "ClusterFollowerPolicy.h"
#include "SettingsStore.h"

struct ClusterFollowerView {
  ClusterFollowerPhase phase = ClusterFollowerPhase::Standalone;
  bool gated = false;            // producer gate (any membership)
  bool forcesLocalClock = false; // LocalFallback: show own clock
  bool renderPending = false;    // a commitAt render is staged, in flight
  String leaderName;
  String leaderHost;
  int row = 0;
  uint32_t epoch = 0;
  uint32_t lastSeq = 0;
  String heldSegment;  // last accepted render text ("" until one arrives)
  int heldSpeed = 0;   // web-scale speed the segment arrived with
};

// setup(): loads the persisted membership and boots the policy (a stored
// membership boots into Grace — the display never flashes stale standalone
// content). Must run before tasksInit().
void clusterFollowerInit(SettingsStore& store);

// netTask, every ~10 ms: policy decay tick, staged NVS persist/clear,
// due-render enqueue (waits out the reflash producer gate and a full
// display queue by retrying next tick).
void clusterFollowerServiceTick(SettingsStore& store);

ClusterFollowerView clusterFollowerViewGet();

// --- async-handler entries -------------------------------------------------------

struct ClusterJoinRequest {
  String leaderName;
  String leaderHost;
  int row = 0;
  uint32_t epoch = 0;
};

void clusterFollowerHandleJoin(const ClusterJoinRequest& req);

// Verdict per ClusterFollowerPolicy.h; on Apply the segment is staged for
// the commitAt-synchronized enqueue (unsynced clock → immediate).
ClusterRenderVerdict clusterFollowerHandleRender(uint32_t epoch, uint32_t seq,
                                                 const String& text, int speed,
                                                 uint64_t commitAtMs);

// false = Standalone: the reply tells the leader to re-join.
bool clusterFollowerHandlePing();

void clusterFollowerHandleLeave();
