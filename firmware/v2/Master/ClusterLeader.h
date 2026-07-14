#pragma once
// ClusterLeader.h — leader-side cluster service (#273, epic #270). The
// supervision decisions live in ClusterLeaderPolicy.h, the geometry in
// ClusterLayout.h and the fleet-rollout sequencing in
// ClusterRolloutPolicy.h (all natively tested); this module owns the live
// member table, the per-member runtimes, the grid content, the streaming
// firmware-convergence upload (#276), and the body of clusterTask — the
// ONLY place outbound cluster HTTP happens (esp_http_client, short LAN
// timeouts), so a dead follower can never stall netTask's SSE/WiFi/LED
// ticks.
//
// Producer contract (Hard-rule preserving): when clusterLeaderEnabled(),
// text/clock producers hand LOGICAL grid content to clusterLeaderSubmit*()
// instead of displayEnqueue — the layer slices segments, stages the
// leader's OWN row on the same commitAt clock the followers get, and ships
// the rest from clusterTask. Cluster disabled → producers take their
// untouched v1 paths (byte-identical passthrough).
//
// Lock ordering: the module mutex is leaf-level next to snapshotMutex
// (leaderMutex → snapshotMutex allowed, never the reverse); web-domain
// locked code is never called from inside it.

#include <Arduino.h>

#include "ClusterLayout.h"
#include "ClusterLeaderPolicy.h"
#include "SettingsStore.h"

struct ClusterLeaderMemberStatus {
  String host;  // "" = the leader's own row
  int row = 0;
  int col = 0;
  int width = 0;
  bool joined = false;
  bool degraded = false;
  int failures = 0;
  String rev;            // follower firmware rev from the join reply
  int reportedWidth = 0; // join-handshake width fact
  bool updating = false;      // fleet rollout (#276) is converging this member
  bool updateBlocked = false; // rollout gave up (attempt cap) on this member
};

struct ClusterLeaderStatus {
  bool enabled = false;
  uint32_t epoch = 0;
  uint32_t seq = 0;
  int memberCount = 0;
  ClusterLeaderMemberStatus members[CLUSTER_MAX_MEMBERS];
  // Fleet rollout (#276): phase name + live progress for the Cluster card.
  String rolloutPhase;   // "idle" / "uploading" / "waiting"
  String rolloutHost;    // target member while not idle
  uint32_t rolloutSent = 0;
  uint32_t rolloutTotal = 0;
  // The running image failed its verify/read pass — convergence is off
  // until reboot ("idle" alone would read as "nothing to do").
  bool rolloutImageFailed = false;
};

// setup(): loads the member table from NVS (invalid → leader disabled,
// loudly), mints the boot epoch. Before tasksInit() — clusterTask ticks
// this module and producers read the enabled gate.
void clusterLeaderInit(SettingsStore& store, const String& deviceName);

// Cheap producer gate — safe from any task without the mutex.
bool clusterLeaderEnabled();

// clusterTask body (Tasks.cpp owns the task itself): staged config swap,
// self-row staging/re-show, then the sequential HTTP fan-out.
void clusterLeaderTick();

// Producers hand LOGICAL grid content here when enabled. Identical
// content dedups internally; changed content re-slices, stamps a shared
// commitAt (~400 ms out) and marks the affected members dirty.
void clusterLeaderSubmitText(const String& text, const String& alignment,
                             int speed);
void clusterLeaderSubmitClock(const String& timeText, const String& dateText,
                              const String& alignment, int speed);

ClusterLeaderStatus clusterLeaderStatusGet();

// Web boundary (async task): validate + stage a new member table spec
// (ClusterLeaderPolicy wire format; "" disables). The swap itself — leave
// fan-out to dropped members, NVS persist, runtime reset — runs in
// clusterTask. httpStatus 200 = staged.
struct ClusterConfigVerdict {
  int httpStatus = 200;
  String message;
};
ClusterConfigVerdict clusterLeaderStageConfig(const String& membersSpec);
