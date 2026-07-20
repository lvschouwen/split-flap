// FollowerCluster.cpp — cluster glue (#298). Contract in FollowerCluster.h.

#include "FollowerCluster.h"

#include <EEPROM.h>
#include <time.h>

#include "ClusterHmac.h"  // cluster-wire auth: key storage + verify (#313 follow-on)
#include "FollowerBus.h"
#include "FollowerConfig.h"
#include "FollowerRescue.h"  // #343: rescue beacon never touches the bus
#include "FollowerSettings.h"

static FollowerClusterState policyState;
static String leaderName;
static String leaderHost;
static int memberRow = 0;
static String heldSegment;
static int heldSpeed = 80;
static volatile bool membershipDirty = false;
// Cluster-wire auth (#313 follow-on): the leader's negotiated key. Present ⇒
// every leader-wire request must carry a valid ts+mac; absent (pre-HMAC
// leader) ⇒ fall back to #313 source-IP binding.
static uint8_t hmacKey[FOLLOWER_HMAC_KEY_LEN] = {0};
static bool hmacKeyValid = false;
// Monotonic replay high-water mark (#313 follow-on HIGH#1): every accepted
// signed request must carry a strictly newer ts. Persisted coarsely in the
// EEPROM membership blob (hmacLastPersistedTs = last written value) so a
// reboot reloads it; reset durably on (re)key at join and on leave so a
// rebooted leader's fresh signing epoch is re-accepted.
static uint64_t hmacLastAcceptedTs = 0;
static uint64_t hmacLastPersistedTs = 0;

// Single staged render slot — a newer accepted render replaces an
// undelivered older one (seq acceptance upstream keeps ordering honest).
static volatile bool renderPending = false;
static String renderText;
static int renderSpeed = 80;
static uint32_t renderDueMs = 0;

// The row is blanked by rendering a full-width space frame; track what the
// row currently shows so blank transitions don't re-flap a blank row.
static bool rowIsBlank = true;

// millis() when a leader render was last APPLIED (#306 diagnostics). Distinct
// from policyState.lastContactMs, which any ping also bumps.
static uint32_t lastRenderMs = 0;
static bool haveRender = false;

static uint64_t nowEpochMs(bool& synced) {
  time_t t = time(nullptr);
  // SNTP epoch-only sync (spec): anything before ~2001 is the unset RTC.
  synced = t > 1000000000;
  return (uint64_t)t * 1000ULL + (millis() % 1000);
}

void clusterInit() {
  EEPROM.begin(FOLLOWER_MEMBERSHIP_BLOB_LEN);
  uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
  for (int i = 0; i < FOLLOWER_MEMBERSHIP_BLOB_LEN; i++) {
    blob[i] = EEPROM.read(i);
  }
  char name[FOLLOWER_NAME_MAX + 1];
  char host[FOLLOWER_HOST_MAX + 1];
  uint8_t row = 0;
  bool stored = followerMembershipDecode(blob, name, host, row, hmacKeyValid,
                                         hmacKey, hmacLastAcceptedTs);
  if (!stored) {
    hmacKeyValid = false;
    hmacLastAcceptedTs = 0;
  }
  hmacLastPersistedTs = hmacLastAcceptedTs;
  if (stored) {
    leaderName = name;
    leaderHost = host;
    memberRow = row;
    SerialPrint(F("Clustered by "));
    SerialPrint(leaderName);
    SerialPrintln(F(" — booting into grace, holding for the leader"));
  }
  followerClusterBoot(policyState, millis(), stored);
}

static void persistMembership() {
  uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
  if (leaderHost.length() == 0 ||
      !followerMembershipEncode(leaderName.c_str(), leaderHost.c_str(),
                                (uint8_t)memberRow, hmacKeyValid, hmacKey,
                                hmacLastAcceptedTs, blob)) {
    followerMembershipClear(blob);
  }
  for (int i = 0; i < FOLLOWER_MEMBERSHIP_BLOB_LEN; i++) {
    EEPROM.write(i, blob[i]);
  }
  EEPROM.commit();
}

void clusterLoopTick() {
  if (membershipDirty) {
    membershipDirty = false;
    persistMembership();
  }

  static uint32_t lastPhaseTickMs = 0;
  if (millis() - lastPhaseTickMs >= 1000) {
    lastPhaseTickMs = millis();
    if (followerClusterTick(policyState, millis())) {
      SerialPrint(F("cluster: phase -> "));
      SerialPrintln(followerPhaseName(policyState.phase));
    }
  }

  // Rescue beacon (#343): membership/phase bookkeeping above stays live so
  // the leader sees us and the marker, but the bus is untouchable — drop
  // any accepted render unshown (the leader's re-push is the point).
  if (rescueActive()) {
    renderPending = false;
    return;
  }

  // Blank rule: Standalone and Blank show nothing; Grace holds. One blank
  // frame per transition (rowIsBlank latches).
  if (followerPhaseShowsBlank(policyState.phase)) {
    if (!rowIsBlank && !renderPending &&
        !reflashInProgress(reflashProgress)) {
      SerialPrintln(F("cluster: blanking the row"));
      busShowSegment("", heldSpeed > 0 ? heldSpeed : 80);
      rowIsBlank = true;
    }
    return;
  }

  if (renderPending && (int32_t)(millis() - renderDueMs) >= 0 &&
      !reflashInProgress(reflashProgress)) {
    // Copy-then-clear: an accepted render landing mid-show simply leaves
    // the flag set for the next pass (latest wins).
    String text = renderText;
    int speed = renderSpeed;
    renderPending = false;
    busShowSegment(text, speed);
    rowIsBlank = text.length() == 0;
  }
}

bool clusterJoinWouldConflict(const String& joiningLeaderHost,
                              String& currentName, String& currentHost) {
  bool sameLeader = leaderHost == joiningLeaderHost;
  if (!followerClusterJoinConflicts(policyState, millis(), sameLeader)) {
    return false;
  }
  currentName = leaderName;
  currentHost = leaderHost;
  return true;
}

void clusterHandleJoin(const String& name, const String& host, int row,
                       uint32_t epoch, const String& key) {
  followerClusterJoin(policyState, millis(), epoch);
  // Adopt the negotiated wire-auth key (#313 follow-on): a valid key turns
  // enforcement ON; a pre-HMAC leader sends none.
  uint8_t newKey[FOLLOWER_HMAC_KEY_LEN];
  bool newKeyValid = key.length() > 0 && clusterKeyFromHex(key, newKey);
  bool keyChanged = newKeyValid != hmacKeyValid ||
                    (newKeyValid && memcmp(newKey, hmacKey, 32) != 0);
  if (newKeyValid) {
    memcpy(hmacKey, newKey, 32);
    hmacKeyValid = true;
  } else {
    hmacKeyValid = false;
  }
  // Fresh key material ⇒ a fresh signing epoch (a leader reboot re-mints):
  // reset the monotonic mark so post-reboot lower ts values are re-accepted.
  // The `changed` persist below rewrites the blob with this reset mark, so a
  // follower reboot after the rekey won't reload a stale-high mark.
  if (keyChanged) {
    hmacLastAcceptedTs = 0;
    hmacLastPersistedTs = 0;
  }
  // NVS/EEPROM-flood guard (#313, parity with the S3 follower): persist the
  // membership blob only when a field actually moved — a leader re-joining
  // on its cadence must not burn EEPROM on every accepted join.
  bool changed = leaderName != name || leaderHost != host ||
                 memberRow != row || keyChanged;
  leaderName = name;
  leaderHost = host;
  memberRow = row;
  if (changed) {
    membershipDirty = true;
    SerialPrint(F("cluster: joined by "));
    SerialPrint(name);
    SerialPrintln(hmacKeyValid ? F(" [authenticated]") : F(""));
  }
}

bool clusterHmacEnforced() { return hmacKeyValid; }

bool clusterVerifySigned(const String& canonicalMsg, uint64_t ts,
                         const String& macHex) {
  if (!hmacKeyValid) return false;
  bool synced = false;
  uint64_t nowMs = nowEpochMs(synced);
  bool ok = clusterHmacAccept(hmacKey, canonicalMsg, ts, macHex, nowMs, synced,
                              hmacLastAcceptedTs);
  if (ok && clusterHmacMarkNeedsPersist(hmacLastAcceptedTs, hmacLastPersistedTs)) {
    hmacLastPersistedTs = hmacLastAcceptedTs;
    membershipDirty = true;  // loop() rewrites the blob (mark included)
  }
  return ok;
}

FollowerRenderVerdict clusterHandleRender(uint32_t epoch, uint32_t seq,
                                          const String& text, int speed,
                                          uint64_t commitAtMs) {
  FollowerRenderVerdict verdict =
      followerClusterAcceptRender(policyState, millis(), epoch, seq);
  if (verdict == FollowerRenderVerdict::Apply) {
    bool synced = false;
    uint64_t nowMs = nowEpochMs(synced);
    heldSegment = text;
    heldSpeed = speed;
    renderText = text;
    renderSpeed = speed;
    renderDueMs = millis() + followerRenderDelayMs(commitAtMs, nowMs, synced);
    renderPending = true;
    lastRenderMs = millis();
    haveRender = true;
  }
  return verdict;
}

bool clusterHandlePing() {
  return followerClusterContact(policyState, millis());
}

void clusterHandleLeave() {
  if (policyState.phase == FollowerPhase::Standalone) return;
  followerClusterLeave(policyState);
  leaderName = "";
  leaderHost = "";
  memberRow = 0;
  heldSegment = "";
  renderPending = false;
  hmacKeyValid = false;  // #313 follow-on: drop the wire-auth key on leave
  hmacLastAcceptedTs = 0;  // and reset the replay mark alongside the key
  hmacLastPersistedTs = 0;
  membershipDirty = true;  // persistMembership clears the blob (mark included)
  SerialPrintln(F("cluster: left — standalone (blank)"));
}

FollowerClusterView clusterViewGet() {
  FollowerClusterView v;
  v.phase = policyState.phase;
  v.leaderName = leaderName;
  v.leaderHost = leaderHost;
  v.row = memberRow;
  v.epoch = policyState.epoch;
  v.lastSeq = policyState.lastSeq;
  v.heldSegment = heldSegment;
  // Diagnostics (#306).
  v.msSinceRender = haveRender ? (int32_t)(millis() - lastRenderMs) : -1;
  // Total silence blanks the row at lastContactMs + FOLLOWER_GRACE_MS (the
  // tick cascades Clustered->Grace->Blank on cumulative silence). Only
  // meaningful while still showing content.
  if (policyState.phase == FollowerPhase::Clustered ||
      policyState.phase == FollowerPhase::Grace) {
    int32_t remainMs =
        (int32_t)(policyState.lastContactMs + FOLLOWER_GRACE_MS - millis());
    v.secsUntilBlank = remainMs > 0 ? remainMs / 1000 : 0;
  } else {
    v.secsUntilBlank = -1;  // already blank / standalone
  }
  bool synced = false;
  (void)nowEpochMs(synced);
  v.sntpSynced = synced;
  return v;
}

bool clusterRenderPending() { return renderPending; }
