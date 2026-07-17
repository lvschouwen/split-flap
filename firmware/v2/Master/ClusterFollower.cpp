// ClusterFollower.cpp — follower-side cluster service (#272). Contract and
// lock ordering in ClusterFollower.h; decisions in ClusterFollowerPolicy.h.

#include "ClusterFollower.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sys/time.h>

#include "ClockPolicy.h"  // clockIsTimeSynced
#include "ClusterDigest.h"  // promote transform + digest field extraction
#include "ClusterHmac.h"  // cluster-wire auth: key storage + verify (#313 follow-on)
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
// Cluster-wire auth key (#313 follow-on): the 64-char hex the leader minted
// at join. Persisted so a rebooted follower keeps enforcing in Grace.
#define CLUSTER_KEY_HMAC        "clHmacKey"
// Persisted replay high-water mark (#313 follow-on HIGH#1): written coarsely so
// a reboot reloads it instead of resetting to 0 (see ClusterHmac.h rationale).
#define CLUSTER_KEY_LASTTS      "clHmacTs"

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
// Cluster-wire auth (#313 follow-on): the negotiated key, held with the
// membership. hmacKeyValid gates ENFORCEMENT — present ⇒ every leader-wire
// request must carry a valid ts+mac; absent (pre-HMAC leader) ⇒ fall back to
// #313 source-IP binding.
static uint8_t hmacKey[32] = {0};
static bool hmacKeyValid = false;
static String hmacKeyHex;  // staged for the NVS persist
// Monotonic replay high-water mark (#313 follow-on HIGH#1): every accepted
// signed request must carry a strictly newer ts. Persisted coarsely to NVS
// (hmacLastPersistedTs tracks the last written value; hmacTsDirty stages the
// next write) so a reboot reloads it; reset durably on (re)key at join and on
// leave so a rebooted leader's fresh signing epoch is re-accepted.
static uint64_t hmacLastAcceptedTs = 0;
static uint64_t hmacLastPersistedTs = 0;
static bool hmacTsDirty = false;
// #294 ping-piggybacked digest: raw JSON held for GET /cluster/digest
// (RAM-only); the promote-critical table + self index persist via
// tableDirty when they change (rare — config edits).
static String digestRaw;
static uint32_t digestReceivedMs = 0;
static String digestTable;
static int digestSelfIndex = -1;
static bool tableDirty = false;
// #321 ranked auto-takeover: this board's rank in the leader's `succ` list
// (-1 = not a designated successor), and the deadline until which an announced
// leader hold suppresses takeover. Both live under the ClusterLock.
static int successorRank = -1;
static uint32_t autoTakeoverHoldUntilMs = 0;

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
  hmacKeyHex = store.getString(CLUSTER_KEY_HMAC, "");
  hmacKeyValid = clusterKeyFromHex(hmacKeyHex, hmacKey);
  if (!hmacKeyValid) hmacKeyHex = "";
  hmacLastAcceptedTs = clusterU64FromStr(store.getString(CLUSTER_KEY_LASTTS, "0"));
  hmacLastPersistedTs = hmacLastAcceptedTs;
  clusterFollowerBoot(policyState, millis(), leaderHost.length() > 0);
  if (policyState.phase == ClusterFollowerPhase::Grace) {
    SerialPrintln("cluster: booted clustered by " + leaderName +
                  " — holding in grace for the leader");
  }
}

static ClusterPromoteVerdict clusterFollowerPromoteImpl(bool autoPath);

void clusterFollowerServiceTick(SettingsStore& store) {
  if (clusterMutex == nullptr) return;

  bool persistMembership = false;
  bool clearMembership = false;
  bool doAutoPromote = false;  // #321: decided under the lock, run after it
  String persistName, persistHost;
  int persistRow = 0;
  String persistKeyHex;  // "" = remove the key alongside the membership
  bool persistTable = false;
  String persistTableSpec;
  int persistSelfIndex = -1;
  bool persistTs = false;
  uint64_t persistTsVal = 0;

  // Staged work is decided under the lock; the NVS writes run outside it
  // so a slow flash commit never stalls an async handler on the mutex.
  {
    ClusterLock lock;
    if (clusterFollowerTick(policyState, millis())) {
      SerialPrintln(String("cluster: phase -> ") +
                    clusterFollowerPhaseName(policyState.phase));
    }
    // #321: a designated successor takes over once leader silence passes its
    // ranked threshold (fires in Grace, before the wall blanks). Decided here
    // under the lock; the promote runs after it releases (it re-locks + takes
    // the leader mutex, and re-checks this gate — TOCTOU-safe).
    doAutoPromote = clusterFollowerAutoPromoteDue(
        policyState, successorRank, autoTakeoverHoldUntilMs, millis());
    if (membershipDirty) {
      membershipDirty = false;
      if (leaderHost.length() > 0) {
        persistMembership = true;
        persistName = leaderName;
        persistHost = leaderHost;
        persistRow = memberRow;
        persistKeyHex = hmacKeyValid ? hmacKeyHex : String();
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
    // Coarse replay-mark persist: fold the current mark into a membership
    // write when one is pending, else stage a standalone mark write. On a
    // leave (clearMembership) the mark key is removed alongside the key.
    if (hmacTsDirty && !clearMembership) {
      hmacTsDirty = false;
      persistTs = true;
      persistTsVal = hmacLastAcceptedTs;
      hmacLastPersistedTs = hmacLastAcceptedTs;  // optimistic; drives next gate
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

  // ORDER IS LOAD-BEARING: Preferences commits per put, so key and mark are
  // two independent flash transactions. Write the mark FIRST, then the key —
  // on a rekey the mark is reset to 0, so a crash between the two writes
  // leaves {old key + reset mark}, which self-heals (the next join's new key
  // differs from the persisted old key ⇒ keyChanged re-fires the reset). The
  // reverse order could strand {new key + stale-high mark}: keyChanged would
  // be false on retry (key already matches) and the follower would wedge,
  // rejecting the rebooted leader's fresh lower ts. (Coarse-advance persists
  // fire alone — no membership write — so this only matters on rekey.)
  if (persistTs) {
    store.putString(CLUSTER_KEY_LASTTS, clusterU64ToStr(persistTsVal));
  }
  if (persistMembership) {
    store.putString(CLUSTER_KEY_LEADER_NAME, persistName);
    store.putString(CLUSTER_KEY_LEADER_HOST, persistHost);
    store.putInt(CLUSTER_KEY_ROW, persistRow);
    if (persistKeyHex.length() > 0) {
      store.putString(CLUSTER_KEY_HMAC, persistKeyHex);
    } else {
      store.remove(CLUSTER_KEY_HMAC);
    }
  } else if (clearMembership) {
    store.remove(CLUSTER_KEY_LEADER_NAME);
    store.remove(CLUSTER_KEY_LEADER_HOST);
    store.remove(CLUSTER_KEY_ROW);
    store.remove(CLUSTER_KEY_HMAC);
    store.remove(CLUSTER_KEY_LASTTS);
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

  // #321: run the ranked auto-takeover outside the lock (it re-locks + takes
  // the leader mutex). The impl re-checks the gate, so a leader that returned
  // between the decision and here cleanly no-ops the promote.
  if (doAutoPromote) {
    ClusterPromoteVerdict v = clusterFollowerPromoteImpl(true);
    if (v.httpStatus != 200) {
      SerialPrintln(String("cluster: auto-takeover stood down (") + v.message +
                    ")");
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
  // Adopt the negotiated wire-auth key (#313 follow-on). A join with a valid
  // key turns enforcement ON; a pre-HMAC leader sends none, so we stay on the
  // #313 source-IP binding. (The key rides the same NVS record — a change
  // marks the membership dirty below.)
  uint8_t newKey[32];
  bool newKeyValid = req.key.length() > 0 && clusterKeyFromHex(req.key, newKey);
  bool keyChanged = newKeyValid != hmacKeyValid ||
                    (newKeyValid && memcmp(newKey, hmacKey, 32) != 0);
  if (newKeyValid) {
    memcpy(hmacKey, newKey, 32);
    hmacKeyValid = true;
    hmacKeyHex = req.key;
  } else {
    hmacKeyValid = false;
    hmacKeyHex = "";
  }
  // Fresh key material ⇒ a fresh signing epoch (a leader reboot re-mints):
  // reset the monotonic mark so post-reboot lower ts values are re-accepted,
  // and stage a durable reset of the persisted mark so a follower reboot right
  // after the rekey doesn't reload a stale-high mark and reject the new leader.
  if (keyChanged) {
    hmacLastAcceptedTs = 0;
    hmacLastPersistedTs = 0;
    hmacTsDirty = true;
  }
  // NVS-flood guard (#313): a leader (re)joins on every membership change,
  // but the follower persists only when a field actually moved — a steady
  // cluster re-joining does not burn flash on every accepted join.
  bool changed = leaderName != req.leaderName || leaderHost != req.leaderHost ||
                 memberRow != req.row || keyChanged;
  leaderName = req.leaderName;
  leaderHost = req.leaderHost;
  memberRow = req.row;
  if (changed) {
    membershipDirty = true;
    SerialPrintln("cluster: joined by " + req.leaderName + " (" +
                  req.leaderHost + ") as row " + String(req.row) +
                  (hmacKeyValid ? " [authenticated]" : ""));
  }
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
  // Source-IP binding (#313): only the joined leader keeps us alive. A
  // foreign LAN host's ping must not refresh the contact-fresh window — that
  // would mask a dead leader and hold the row hostage. (When Standalone
  // leaderHost is "", so contact below still 409s the ping.)
  if (leaderHost.length() > 0 && remoteIp != leaderHost) return false;
  if (!clusterFollowerContact(policyState, millis())) return false;
  // The digest becomes served-back state and the #295 promote input — the
  // IP is already bound to the leader above, so accept it only as one
  // balanced JSON object and persist the table only when it parses as a
  // valid member table. Everything else degrades to a plain keep-alive.
  if (digest.length() > 0 && clusterDigestShapeOk(digest)) {
    digestRaw = digest;
    digestReceivedMs = millis();
    // #321: refresh our auto-takeover rank from the leader's `succ` list vs our
    // own `you` index — the leader recomputes it as the fleet changes; -1 means
    // we're not a designated successor (ESP-01/foreign, or absent) → never fire.
    successorRank = clusterSuccessorRank(
        clusterExtractJsonString(digest, "succ").c_str(), youIndex);
    // #321 graceful-reboot hold: a leader about to restart on purpose stamps a
    // hold (ms) into its (signed) digest — push our takeover deadline past the
    // reboot window so a planned reboot doesn't look like death. A swapped/
    // injected hold can't spoof it: the digest is HMAC-signed above (#313).
    long holdMs = clusterExtractJsonInt(digest, "hold", 0);
    if (holdMs > 0) {
      autoTakeoverHoldUntilMs =
          clusterAutoTakeoverHoldDeadline((uint32_t)holdMs, millis());
    }
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

bool clusterFollowerHmacEnforced() {
  if (clusterMutex == nullptr) return false;
  ClusterLock lock;
  return hmacKeyValid;
}

bool clusterFollowerVerifySigned(const String& canonicalMsg, uint64_t ts,
                                 const String& macHex) {
  if (clusterMutex == nullptr) return false;
  bool synced = false;
  uint64_t nowMs = nowEpochMs(synced);  // no lock needed (gettimeofday only)
  ClusterLock lock;
  if (!hmacKeyValid) return false;
  bool ok = clusterHmacAccept(hmacKey, canonicalMsg, ts, macHex, nowMs, synced,
                              hmacLastAcceptedTs);
  if (ok && clusterHmacMarkNeedsPersist(hmacLastAcceptedTs, hmacLastPersistedTs)) {
    hmacTsDirty = true;  // drained in clusterFollowerServiceTick (outside lock)
  }
  return ok;
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
  hmacKeyValid = false;  // #313 follow-on: drop the wire-auth key on leave
  hmacKeyHex = "";
  hmacLastAcceptedTs = 0;  // and reset the replay mark alongside the key
  hmacLastPersistedTs = 0;
  hmacTsDirty = false;  // clearMembership removes CLUSTER_KEY_LASTTS from NVS
  digestRaw = "";
  digestTable = "";
  digestSelfIndex = -1;
  successorRank = -1;          // #321: no longer a successor once we leave
  autoTakeoverHoldUntilMs = 0;
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

// Gate for a promote attempt — MUST be evaluated under the ClusterLock. Manual
// (#295, web button) requires LocalFallback; auto (#321) requires the ranked
// takeover trigger (rank + staggered silence, no active hold). The commit-point
// re-check uses this same gate so a leader returning mid-promote cancels it.
static bool promoteGatePasses(bool autoPath) {
  return autoPath ? clusterFollowerAutoPromoteDue(policyState, successorRank,
                                                  autoTakeoverHoldUntilMs,
                                                  millis())
                  : clusterFollowerCanPromote(policyState);
}

static ClusterPromoteVerdict clusterFollowerPromoteImpl(bool autoPath) {
  if (clusterMutex == nullptr) return {500, "cluster service not running"};
  String tableSpec, oldLeaderHost;
  int selfIndex;
  {
    ClusterLock lock;
    if (!promoteGatePasses(autoPath)) {
      return {409, autoPath
                       ? "auto-takeover not due"
                       : "Promote requires local-fallback — the leader is "
                         "still expected back"};
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
  // gate and leave in ONE critical section. A leader ping landing after
  // this wins nothing: the board is Standalone (ping 409s) and about to
  // lead; the returning old leader demotes on the sticky-leadership 409s.
  {
    ClusterLock lock;
    if (!promoteGatePasses(autoPath)) {
      return {409, autoPath ? "auto-takeover cancelled — leader returned"
                            : "The leader came back — promote cancelled"};
    }
    followerLeaveLocked();
  }
  clusterLeaderStageConfig(newSpec);  // pre-validated: cannot fail
  SerialPrintln(String("cluster: ") +
                (autoPath ? "AUTO-PROMOTED" : "PROMOTED") +
                " — taking over the wall as leader (" + newSpec + ")");
  return {200, "Promoted — leading now; members join on the next tick"};
}

ClusterPromoteVerdict clusterFollowerPromote() {
  return clusterFollowerPromoteImpl(false);
}
