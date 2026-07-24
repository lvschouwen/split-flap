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
  bool suspect = false;      // #385: failing contacts, not yet 30 s silent —
                             // quiet tier (no re-join, no HA alarm)
  bool renderStuck = false;  // #385: alive but segment undeliverable >= 30 s
  int failures = 0;
  String rev;            // follower firmware rev from the join reply
  String plat;           // reported platform, "" = same as leader (#297)
  String role;           // reported deviceRole, "" = pre-#332 peer (#332)
  int reportedWidth = 0; // join-handshake width fact
  bool updating = false;      // fleet rollout (#276) is converging this member
  bool updateBlocked = false; // rollout gave up (attempt cap) on this member
  bool rescue = false;        // #343: member is a boot-looping rescue beacon
  bool hmac = false;          // wire-auth key negotiated — leader signs to this
                              // member (#313 follow-on observability)
  // Per-row unit health (#294): the follower's last ping-reply health, or
  // the local snapshot for the self row. healthValid false = never
  // reported (old firmware) — the UI hides the strip, never reads zeros.
  bool healthValid = false;
  int faulty = 0;
  int detected = 0;
  String faultMask;  // hex bitmap, bit i = unit at position i faulty
  bool wear = false;
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
  // #344: the active rollout streams the STORED follower image at an esp01
  // row (additive "src":"esp01" on the wire; absent = the S3 slot).
  bool rolloutFollowerImage = false;
  // On-demand ESP-01 firmware relay (#304 Part B): the stored follower image
  // + the live on-demand push. followerImage* drive the Cluster card's upload
  // control; followerPush* mirror the rollout fields for the member panel's
  // per-row "Update firmware" button.
  bool followerImagePresent = false;
  String followerImageRev;
  String followerPushPhase;   // "idle" / "uploading"
  String followerPushHost;    // target member while uploading
  uint32_t followerPushSent = 0;
  uint32_t followerPushTotal = 0;
  String followerPushResult;  // last verdict: none / done / failed / rejected
  // Derived grid shape (#277): row count + total logical units — the
  // wall's text capacity as HA surfaces it. 0/0 while not leading.
  int gridRows = 0;
  int gridCapacity = 0;
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

// #321 graceful-reboot hold: before an intentional restart, announce a hold to
// followers so a designated backup doesn't take over during the reboot window.
// No-op when not leading. clusterTask fans it out; the reboot drain waits on
// clusterLeaderRebootHoldSent() (true immediately when not leading).
void clusterLeaderAnnounceRebootHold();
bool clusterLeaderRebootHoldSent();

// Producers hand LOGICAL grid content here when enabled. Identical
// content dedups internally; changed content re-slices, stamps a shared
// commitAt (~400 ms out) and marks the affected members dirty.
void clusterLeaderSubmitText(const String& text, const String& alignment,
                             int speed);
void clusterLeaderSubmitClock(const String& timeText, const String& dateText,
                              const String& alignment, int speed);
// #317: blank the follower rows (leader's own row = the local Stop). Leaves
// lastContentKey so the reroute stays blank until the clock content moves.
void clusterLeaderBlankWall();
// #332: this board's own deviceRole for the /cluster/status self row (the
// runtime table never sees it — the self row is not an HTTP peer). Seeded at
// webEndpointsInit, pushed by the settings drain on change.
void clusterLeaderSetSelfRole(const String& role);
// #342: tz for the join body (esp01 clock fallback) — seeded at init,
// pushed by the settings drain on a tz POST.
void clusterLeaderSetTz(const String& tzPosix);

ClusterLeaderStatus clusterLeaderStatusGet();

// Wall mirror rows (#277): reconstructed full row texts — segments plus
// `selfRowText` (this master's live currentText) overlaid on the own-row
// slot(s) with the display's alignment lead. Any task (mutex-copied).
// rows[] needs CLUSTER_MAX_MEMBERS entries; selfRowOut gets the own row's
// index (the browser anchors the health strip there; -1 = no own row).
// Returns the grid's row count, 0 while not leading.
int clusterLeaderMirrorRows(String* rows, int& selfRowOut,
                            const String& selfRowText,
                            const String& alignment);

// Cheap change signal for the SSE tick (#277): increments whenever the
// segments change (submit deltas, config swaps). Any task, lock-free —
// compare against the last seen value before paying for MirrorRows.
uint32_t clusterLeaderGridGeneration();

// Web boundary (async task): validate + stage a new member table spec
// (ClusterLeaderPolicy wire format; "" disables). The swap itself — leave
// fan-out to dropped members, NVS persist, runtime reset — runs in
// clusterTask. httpStatus 200 = staged.
struct ClusterConfigVerdict {
  int httpStatus = 200;
  String message;
};
ClusterConfigVerdict clusterLeaderStageConfig(const String& membersSpec);

// Web boundary (async task): stage an on-demand follower-image push to the
// given member host (#304 Part B). Validates the host is a known esp01
// member and a follower image is stored; the file stream to the follower's
// /firmware/master runs on clusterTask. httpStatus 200 = staged.
ClusterConfigVerdict clusterLeaderStageFollowerPush(const String& host);

// The stage's validation alone (pure, no staging): #295 promote
// pre-validates the transformed table BEFORE leaving the dead membership,
// so the post-leave stage is deterministic and cannot strand the board.
ClusterConfigVerdict clusterLeaderValidateSpec(const String& membersSpec);
