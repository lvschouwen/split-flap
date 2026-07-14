// ClusterFollower.cpp — follower-side cluster service (#272). Contract and
// lock ordering in ClusterFollower.h; decisions in ClusterFollowerPolicy.h.

#include "ClusterFollower.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sys/time.h>

#include "ClockPolicy.h"  // clockIsTimeSynced
#include "DisplayCommand.h"
#include "HelpersSerialHandling.h"
#include "ReflashPlan.h"
#include "Tasks.h"

// NVS membership record (15-char key limit). leaderHost doubles as the
// stored/not-stored sentinel — a membership without a reachable leader
// host is not a membership.
#define CLUSTER_KEY_LEADER_NAME "clLeaderName"
#define CLUSTER_KEY_LEADER_HOST "clLeaderHost"
#define CLUSTER_KEY_ROW         "clRow"

static SemaphoreHandle_t clusterMutex = nullptr;

struct ClusterLock {
  ClusterLock() { xSemaphoreTake(clusterMutex, portMAX_DELAY); }
  ~ClusterLock() { xSemaphoreGive(clusterMutex); }
  ClusterLock(const ClusterLock&) = delete;
  ClusterLock& operator=(const ClusterLock&) = delete;
};

// All guarded by clusterMutex.
static ClusterFollowerState policyState;
static String leaderName;
static String leaderHost;
static int memberRow = 0;
static String heldSegment;
static int heldSpeed = 0;
static bool membershipDirty = false;  // staged NVS persist/clear

// Single staged render slot — a newer accepted render simply replaces an
// undelivered older one (seq acceptance upstream keeps ordering honest).
static bool renderPending = false;
static String renderText;
static int renderSpeed = 0;
static uint32_t renderDueMs = 0;

static uint64_t nowEpochMs(bool& synced) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  synced = clockIsTimeSynced(tv.tv_sec);
  return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

void clusterFollowerInit(SettingsStore& store) {
  clusterMutex = xSemaphoreCreateMutex();
  if (clusterMutex == nullptr) {
    Serial.println(F("FATAL: clusterMutex allocation failed"));
    abort();
  }
  leaderName = store.getString(CLUSTER_KEY_LEADER_NAME, "");
  leaderHost = store.getString(CLUSTER_KEY_LEADER_HOST, "");
  memberRow = store.getInt(CLUSTER_KEY_ROW, 0);
  clusterFollowerBoot(policyState, millis(), leaderHost.length() > 0);
  if (policyState.phase == ClusterFollowerPhase::Grace) {
    SerialPrintln("cluster: booted clustered by " + leaderName +
                  " — holding in grace for the leader");
  }
}

void clusterFollowerServiceTick(SettingsStore& store) {
  if (clusterMutex == nullptr) return;

  bool persistMembership = false;
  bool clearMembership = false;
  String persistName, persistHost;
  int persistRow = 0;

  // Staged work is decided under the lock; the NVS writes run outside it
  // so a slow flash commit never stalls an async handler on the mutex.
  {
    ClusterLock lock;
    if (clusterFollowerTick(policyState, millis())) {
      SerialPrintln(String("cluster: phase -> ") +
                    clusterFollowerPhaseName(policyState.phase));
    }
    if (membershipDirty) {
      membershipDirty = false;
      if (leaderHost.length() > 0) {
        persistMembership = true;
        persistName = leaderName;
        persistHost = leaderHost;
        persistRow = memberRow;
      } else {
        clearMembership = true;
      }
    }
    if (renderPending && (int32_t)(millis() - renderDueMs) >= 0) {
      // The reflash producer gate outranks renders too — the job owns the
      // display; the slot retries until it ends (idempotent, latest wins).
      if (!reflashInProgress(displaySnapshotGet().reflash)) {
        if (displayEnqueue(
                makeShowTextCommand(renderText, "left", renderSpeed))) {
          renderPending = false;
        }
        // Queue full: keep the slot, next tick retries.
      }
    }
  }

  if (persistMembership) {
    store.putString(CLUSTER_KEY_LEADER_NAME, persistName);
    store.putString(CLUSTER_KEY_LEADER_HOST, persistHost);
    store.putInt(CLUSTER_KEY_ROW, persistRow);
  } else if (clearMembership) {
    store.remove(CLUSTER_KEY_LEADER_NAME);
    store.remove(CLUSTER_KEY_LEADER_HOST);
    store.remove(CLUSTER_KEY_ROW);
  }
}

ClusterFollowerView clusterFollowerViewGet() {
  ClusterFollowerView v;
  if (clusterMutex == nullptr) return v;
  ClusterLock lock;
  v.phase = policyState.phase;
  v.gated = clusterFollowerGatesProducers(policyState);
  v.forcesLocalClock = clusterFollowerForcesLocalClock(policyState);
  v.renderPending = renderPending;
  v.leaderName = leaderName;
  v.leaderHost = leaderHost;
  v.row = memberRow;
  v.epoch = policyState.epoch;
  v.lastSeq = policyState.lastSeq;
  v.heldSegment = heldSegment;
  v.heldSpeed = heldSpeed;
  return v;
}

void clusterFollowerHandleJoin(const ClusterJoinRequest& req) {
  ClusterLock lock;
  clusterFollowerJoin(policyState, millis(), req.epoch);
  leaderName = req.leaderName;
  leaderHost = req.leaderHost;
  memberRow = req.row;
  membershipDirty = true;
  SerialPrintln("cluster: joined by " + req.leaderName + " (" + req.leaderHost +
                ") as row " + String(req.row));
}

ClusterRenderVerdict clusterFollowerHandleRender(uint32_t epoch, uint32_t seq,
                                                 const String& text, int speed,
                                                 uint64_t commitAtMs) {
  // Segments live in the display domain: truncate like every other text
  // producer or the clockTask re-show dedup can wedge (ClockPolicy H1).
  String segment = truncateForDisplay(text);
  bool synced = false;
  uint64_t nowMs = nowEpochMs(synced);

  ClusterLock lock;
  ClusterRenderVerdict verdict =
      clusterFollowerAcceptRender(policyState, millis(), epoch, seq);
  if (verdict == ClusterRenderVerdict::Apply) {
    heldSegment = segment;
    heldSpeed = speed;
    renderPending = true;
    renderText = segment;
    renderSpeed = speed;
    renderDueMs = millis() + clusterRenderDelayMs(commitAtMs, nowMs, synced);
  }
  return verdict;
}

bool clusterFollowerHandlePing() {
  ClusterLock lock;
  return clusterFollowerContact(policyState, millis());
}

void clusterFollowerHandleLeave() {
  ClusterLock lock;
  if (policyState.phase == ClusterFollowerPhase::Standalone) return;
  clusterFollowerLeave(policyState);
  leaderName = "";
  leaderHost = "";
  memberRow = 0;
  heldSegment = "";
  heldSpeed = 0;
  renderPending = false;
  membershipDirty = true;
  SerialPrintln(F("cluster: left — standalone again"));
}
