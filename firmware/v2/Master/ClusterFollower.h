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
  String key;  // #313 follow-on: 64-char hex wire-auth key ("" = pre-HMAC leader)
};

void clusterFollowerHandleJoin(const ClusterJoinRequest& req);

// Cluster-wire auth (#313 follow-on). Enforced == a key is held (negotiated
// at join): every leader-wire request must then carry a valid ts+mac. The
// web handlers rebuild the canonical message from the wire params and call
// verify; a false verdict is a 403. When not enforced, the caller falls back
// to #313 source-IP binding.
bool clusterFollowerHmacEnforced();
bool clusterFollowerVerifySigned(const String& canonicalMsg, uint64_t ts,
                                 const String& macHex);

// Verdict per ClusterFollowerPolicy.h; on Apply the segment is staged for
// the commitAt-synchronized enqueue (unsynced clock → immediate).
ClusterRenderVerdict clusterFollowerHandleRender(uint32_t epoch, uint32_t seq,
                                                 const String& text, int speed,
                                                 uint64_t commitAtMs);

// false = Standalone: the reply tells the leader to re-join. The #294
// piggybacked digest (raw JSON, RAM-only) and this member's table index
// ride in — accepted only when remoteIp matches the joined leaderHost and
// the digest is one balanced JSON object (it is re-served raw and feeds
// #295 promote); the promote-critical bits (table spec + selfIndex)
// persist to NVS only when they change, so #295 survives a reboot.
bool clusterFollowerHandlePing(const String& digest, int youIndex,
                               const String& remoteIp);

void clusterFollowerHandleLeave();

// The stored digest for GET /cluster/digest ("" = none held); ageMsOut =
// ms since it arrived.
String clusterFollowerDigestGet(uint32_t& ageMsOut);

// #295 sticky leadership: true = this join must 409 (clustered to a
// DIFFERENT leader that is still demonstrably alive).
bool clusterFollowerJoinWouldConflict(const String& leaderHost);

// #295 promote: LocalFallback + a held digest-table → stage the
// transformed member table as this board's own leader config and leave the
// dead membership. The actual swap runs in clusterTask (staged config).
struct ClusterPromoteVerdict {
  int httpStatus = 200;
  String message;
};
ClusterPromoteVerdict clusterFollowerPromote();
