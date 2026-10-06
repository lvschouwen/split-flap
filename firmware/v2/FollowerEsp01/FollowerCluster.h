#pragma once
// FollowerCluster.h — the row's tie to its master: the stored pairing, the
// phase machine (shared ClusterMemberPhase.h: Clustered → Grace → LeaderLost)
// and the text waiting for its flip instant. Decisions live in
// FollowerPolicy.h; the EEPROM record in FollowerSettings.h. Context rule: a
// web handler may pair (pure policy + staging only); loop()
// (clusterLoopTick) does the EEPROM commits and the blocking renders. On the
// single-core non-RTOS ESP8266 the async callbacks interleave with loop()
// only at yield points.

#include <Arduino.h>

#include "FollowerPolicy.h"

struct FollowerClusterView {
  ClusterFollowerPhase phase = ClusterFollowerPhase::Standalone;
  String leaderName;  // the master's id; "" = unpaired
  String leaderHost;  // its address when it paired
  bool sntpSynced = false;  // gates flip-instant timing
};

// setup(): EEPROM.begin + pairing load; starts the phase machine (Grace when
// a pairing is stored).
void clusterInit();

// The master's messages are arriving right now (#515).
bool clusterLeaderContactFresh();
// The master's POSIX tz rule from a Config message. An empty one keeps the
// zone already held; a new one is stored with the pairing. loop() only.
void clusterSetTz(const String& tz);
// #227: the master's quiet flag: a row of a quiet wall moves nothing by
// itself, also not once its master has gone silent.
void clusterNoteLeaderQuiet(bool quiet);

// loop(): ~1 Hz phase decay (blank or fallback clock once the master is
// written off), due render drain, staged EEPROM persist. Blocking I2C happens
// in here only.
void clusterLoopTick();

// Stores `masterId` at `masterHost` as this row's master (POST /pair, once
// FollowerPairPolicy.h said Store) and starts the contact window.
void clusterPair(const String& masterId, const String& masterHost);
// The link reached Welcome: contact, and a new render sequence under `epoch`.
void clusterMasterConnected(uint32_t epoch);
ClusterRenderVerdict clusterHandleRender(uint32_t epoch, uint32_t seq,
                                          const String& text, int speed,
                                          uint64_t commitAtMs);
bool clusterHandlePing();
// Release by the master: forgets the pairing and blanks the row. Idempotent.
void clusterHandleLeave();

FollowerClusterView clusterViewGet();

// True while a text waits for its flip instant — unit jobs and the unit
// facts wait behind it.
bool clusterRenderPending();
