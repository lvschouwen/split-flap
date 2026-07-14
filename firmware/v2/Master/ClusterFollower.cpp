// ClusterFollower.cpp — follower-side cluster service (#272). Contract and
// lock ordering in ClusterFollower.h; decisions in ClusterFollowerPolicy.h.

#include "ClusterFollower.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sys/time.h>

#include "ClockPolicy.h"  // clockIsTimeSynced
#include "ClusterDigest.h"  // promote transform + digest field extraction
#include "ClusterLeader.h"  // clusterLeaderStageConfig — the promote handoff
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
// #294/#295: the digest-carried member table + this member's index in it —
// the promote inputs. Persisted so a takeover stays possible after a
// follower reboot; written only when the table actually changes.
#define CLUSTER_KEY_TABLE       "clTable"
#define CLUSTER_KEY_SELF_INDEX  "clSelfIdx"

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
// #294 ping-piggybacked digest: raw JSON held for GET /cluster/digest
// (RAM-only); the promote-critical table + self index persist via
// tableDirty when they change (rare — config edits).
static String digestRaw;
static uint32_t digestReceivedMs = 0;
static String digestTable;
static int digestSelfIndex = -1;
static bool tableDirty = false;

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
  digestTable = store.getString(CLUSTER_KEY_TABLE, "");
  digestSelfIndex = store.getInt(CLUSTER_KEY_SELF_INDEX, -1);
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
  bool persistTable = false;
  String persistTableSpec;
  int persistSelfIndex = -1;

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
    if (tableDirty) {
      tableDirty = false;
      persistTable = true;
      persistTableSpec = digestTable;
      persistSelfIndex = digestSelfIndex;
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
  if (persistTable) {
    if (persistTableSpec.length() > 0) {
      store.putString(CLUSTER_KEY_TABLE, persistTableSpec);
      store.putInt(CLUSTER_KEY_SELF_INDEX, persistSelfIndex);
    } else {
      store.remove(CLUSTER_KEY_TABLE);
      store.remove(CLUSTER_KEY_SELF_INDEX);
    }
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

bool clusterFollowerHandlePing(const String& digest, int youIndex,
                               const String& remoteIp) {
  ClusterLock lock;
  if (!clusterFollowerContact(policyState, millis())) return false;
  // Digest trust gate (review CRITICAL): the ping itself is any-LAN
  // contact (pre-existing), but the digest becomes served-back state and
  // the #295 promote input — accept it only from the joined leader's IP
  // (the join always carries WiFi.localIP), only as one balanced JSON
  // object, and persist the table only when it parses as a valid member
  // table. Everything else degrades to a plain keep-alive.
  if (digest.length() > 0 && remoteIp == leaderHost &&
      clusterDigestShapeOk(digest)) {
    digestRaw = digest;
    digestReceivedMs = millis();
    // The promote inputs persist only on change — the table moves on
    // config edits, not on the 10 s ping cadence.
    String tableSpec = clusterExtractJsonString(digest, "table");
    ClusterMemberTable parsed;
    if (tableSpec.length() > 0 && youIndex >= 0 &&
        youIndex < CLUSTER_MAX_MEMBERS &&
        clusterTableFromString(tableSpec, parsed) && parsed.count > 0 &&
        (tableSpec != digestTable || youIndex != digestSelfIndex)) {
      digestTable = tableSpec;
      digestSelfIndex = youIndex;
      tableDirty = true;
    }
  }
  return true;
}

// Lock HELD by the caller.
static void followerLeaveLocked() {
  clusterFollowerLeave(policyState);
  leaderName = "";
  leaderHost = "";
  memberRow = 0;
  heldSegment = "";
  heldSpeed = 0;
  renderPending = false;
  membershipDirty = true;
  digestRaw = "";
  digestTable = "";
  digestSelfIndex = -1;
  tableDirty = true;
  SerialPrintln(F("cluster: left — standalone again"));
}

void clusterFollowerHandleLeave() {
  ClusterLock lock;
  if (policyState.phase == ClusterFollowerPhase::Standalone) return;
  followerLeaveLocked();
}

String clusterFollowerDigestGet(uint32_t& ageMsOut) {
  ageMsOut = 0;
  if (clusterMutex == nullptr) return "";
  ClusterLock lock;
  if (digestRaw.length() > 0) ageMsOut = millis() - digestReceivedMs;
  return digestRaw;
}

bool clusterFollowerJoinWouldConflict(const String& joiningLeaderHost) {
  if (clusterMutex == nullptr) return false;
  ClusterLock lock;
  bool sameLeader = leaderHost == joiningLeaderHost;
  return clusterFollowerJoinConflicts(policyState, millis(), sameLeader);
}

ClusterPromoteVerdict clusterFollowerPromote() {
  if (clusterMutex == nullptr) return {500, "cluster service not running"};
  String tableSpec, oldLeaderHost;
  int selfIndex;
  {
    ClusterLock lock;
    if (!clusterFollowerCanPromote(policyState)) {
      return {409, "Promote requires local-fallback — the leader is still expected back"};
    }
    if (digestTable.length() == 0 || digestSelfIndex < 0) {
      return {409, "No cluster digest held — cannot take over"};
    }
    tableSpec = digestTable;
    selfIndex = digestSelfIndex;
    oldLeaderHost = leaderHost;
  }

  // Outside the lock: the leader module's calls take LeaderLock — the two
  // module locks are never held together (file lock contract).
  String newSpec;
  if (!clusterPromoteTransform(tableSpec, selfIndex, oldLeaderHost, newSpec)) {
    return {500, "Promote table transform failed"};
  }
  // Pre-validate so the post-leave stage below is deterministic — promote
  // must never leave the membership and THEN fail to become leader.
  ClusterConfigVerdict v = clusterLeaderValidateSpec(newSpec);
  if (v.httpStatus != 200) return {v.httpStatus, v.message};

  // Commit point (review HIGH — promote/reclaim TOCTOU): re-check the
  // phase and leave in ONE critical section. A leader ping landing after
  // this wins nothing: the board is Standalone (ping 409s) and about to
  // lead; the returning old leader demotes on the sticky-leadership 409s.
  {
    ClusterLock lock;
    if (!clusterFollowerCanPromote(policyState)) {
      return {409, "The leader came back — promote cancelled"};
    }
    followerLeaveLocked();
  }
  clusterLeaderStageConfig(newSpec);  // pre-validated: cannot fail
  SerialPrintln("cluster: PROMOTED — taking over the wall as leader (" +
                newSpec + ")");
  return {200, "Promoted — leading now; members join on the next tick"};
}
