// FollowerCluster.cpp — cluster glue (#298). Contract in FollowerCluster.h.

#include "FollowerCluster.h"

#include <EEPROM.h>
#include <time.h>

#include "FollowerBus.h"
#include "FollowerConfig.h"
#include "FollowerSettings.h"

static FollowerClusterState policyState;
static String leaderName;
static String leaderHost;
static int memberRow = 0;
static String heldSegment;
static int heldSpeed = 80;
static volatile bool membershipDirty = false;

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
  bool stored = followerMembershipDecode(blob, name, host, row);
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
                                (uint8_t)memberRow, blob)) {
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
                       uint32_t epoch) {
  followerClusterJoin(policyState, millis(), epoch);
  leaderName = name;
  leaderHost = host;
  memberRow = row;
  membershipDirty = true;
  SerialPrint(F("cluster: joined by "));
  SerialPrintln(name);
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
  membershipDirty = true;
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
