#pragma once
// FollowerCluster.h — cluster membership + render staging glue (#298).
// Decisions live in FollowerPolicy.h; the EEPROM record in
// FollowerSettings.h. Context rule (v1 verbatim): async handlers run the
// pure policy + stage work; loop() (clusterLoopTick) does the EEPROM
// commits and the blocking segment renders. On the single-core non-RTOS
// ESP8266 the async callbacks interleave with loop() only at yield points,
// which is the same contract every v1 flag handoff relies on.

#include <Arduino.h>

#include "FollowerPolicy.h"

struct FollowerClusterView {
  FollowerPhase phase = FollowerPhase::Standalone;
  String leaderName;
  String leaderHost;
  int row = 0;
  uint32_t epoch = 0;
  uint32_t lastSeq = 0;
  String heldSegment;
  // Diagnostics for /cluster/health (#306) so a curl-only operator can see
  // WHY a row is blank/stale. -1 = not applicable (no render yet / already
  // blank/standalone). sntpSynced gates commitAt flip timing.
  int32_t msSinceRender = -1;
  int32_t secsUntilBlank = -1;
  bool sntpSynced = false;
};

// setup(): EEPROM.begin + membership load; boots the phase machine (Grace
// when a membership is stored).
void clusterInit();

// loop(): ~1 Hz phase decay (blank on Blank/Standalone transitions), due
// render drain, staged EEPROM persist. Blocking I2C happens in here only.
void clusterLoopTick();

// Handler-facing (async context — pure policy + staging only).
bool clusterJoinWouldConflict(const String& joiningLeaderHost,
                              String& currentName, String& currentHost);
void clusterHandleJoin(const String& leaderName, const String& leaderHost,
                       int row, uint32_t epoch, const String& key);
FollowerRenderVerdict clusterHandleRender(uint32_t epoch, uint32_t seq,
                                          const String& text, int speed,
                                          uint64_t commitAtMs);
bool clusterHandlePing();
void clusterHandleLeave();

// Cluster-wire auth (#313 follow-on). Enforced == a key is held (negotiated
// at join): every leader-wire request must then carry a valid ts+mac; the web
// handlers rebuild the canonical message and call verify, 403 on failure.
bool clusterHmacEnforced();
bool clusterVerifySigned(const String& canonicalMsg, uint64_t ts,
                         const String& macHex);

FollowerClusterView clusterViewGet();

// True while a staged render is waiting for its commitAt — the ops drain
// defers behind it (mutual 409 discipline lives at the web boundary).
bool clusterRenderPending();
