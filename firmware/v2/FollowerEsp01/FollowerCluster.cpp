// FollowerCluster.cpp — pairing, phase and render glue. Contract in
// FollowerCluster.h.

#include "FollowerCluster.h"

#include <EEPROM.h>
#include <time.h>

#include "FollowerBus.h"
#include "FollowerClock.h"  // #342: local clock fallback when the master is lost
#include "FollowerConfig.h"
#include "FollowerPrefs.h"
#include "PrintableAscii.h"
#include "FollowerRescue.h"  // #343: rescue mode never touches the bus
#include "FollowerSettings.h"

// #227: what the master last said about quiet mode. RAM only — it describes
// the master's state.
static volatile bool leaderQuiet = false;

static ClusterFollowerState policyState;
static String leaderName;
static String leaderHost;
// #342: the master's POSIX zone (a Config message, stored with the pairing)
// — fuels the clock fallback. "" = none known yet, no clock.
static String leaderTz;
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

// #342 clock fallback bookkeeping: what's on the row is OUR clock (not
// leader content), and which minute it shows (repaint only on change —
// one flap tick per minute, not per loop pass).
static bool clockShowing = false;
static int shownClockMinute = -1;
static bool clockShowsDate = false;  // shownClockMinute is then the day of the year

static uint64_t nowEpochMs(bool& synced) {
  time_t t = time(nullptr);
  // SNTP epoch-only sync (spec): anything before ~2001 is the unset RTC.
  synced = t > 1000000000;
  return (uint64_t)t * 1000ULL + (millis() % 1000);
}

// Where the row's time comes from and which zone it is read in (#566): the
// master answers time requests, so its address is the first server — rows
// flip at an instant the master names, and agreeing with the master matters
// more than agreeing with the internet. The public pool stays second, for a
// row whose master is gone and for one that is not paired. The zone is the
// master's (#342), for the fallback clock.
//
// configTime() restarts SNTP and must never run from an async handler
// (#362: a bare setenv+tzset is INERT for localtime_r here), so a change only
// marks the setup as due and clusterLoopTick() does it.
static volatile bool clockSetupDue = true;
static void clockSetupChanged() { clockSetupDue = true; }

static void installClock() {
  configTime(leaderTz.length() > 0 ? leaderTz.c_str() : "UTC0",
             leaderHost.length() > 0 ? leaderHost : String(F("pool.ntp.org")),
             leaderHost.length() > 0 ? String(F("pool.ntp.org")) : String());
}

void clusterInit() {
  // One mirror for both records: the pairing and, behind it, the operator
  // preferences (#513, read by prefsInit()).
  EEPROM.begin(FOLLOWER_EEPROM_LEN);
  uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
  for (int i = 0; i < FOLLOWER_MEMBERSHIP_BLOB_LEN; i++) {
    blob[i] = EEPROM.read(i);
  }
  char name[FOLLOWER_NAME_MAX + 1];
  char host[FOLLOWER_HOST_MAX + 1];
  char tz[FOLLOWER_TZ_MAX + 1];
  bool stored = followerMembershipDecode(blob, name, host, tz);
  if (stored) {
    leaderName = name;
    leaderHost = host;
    leaderTz = tz;
    clockSetupChanged();
    SerialPrint(F("Paired with "));
    SerialPrint(leaderName);
    SerialPrintln(F(" — starting in grace, waiting for the master"));
  }
  clusterFollowerBoot(policyState, millis(), stored);
}

static void persistMembership() {
  uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
  if (leaderHost.length() == 0 ||
      !followerMembershipEncode(leaderName.c_str(), leaderHost.c_str(),
                                leaderTz.c_str(), blob)) {
    followerMembershipClear(blob);
  }
  for (int i = 0; i < FOLLOWER_MEMBERSHIP_BLOB_LEN; i++) {
    EEPROM.write(i, blob[i]);
  }
  EEPROM.commit();
}

bool clusterLeaderContactFresh() {
  return clusterFollowerContactFresh(policyState, millis());
}

void clusterLoopTick() {
  if (membershipDirty) {
    membershipDirty = false;
    persistMembership();
  }
  if (clockSetupDue) {
    clockSetupDue = false;
    installClock();
  }

  static uint32_t lastPhaseTickMs = 0;
  if (millis() - lastPhaseTickMs >= 1000) {
    lastPhaseTickMs = millis();
    if (clusterFollowerTick(policyState, millis())) {
      SerialPrint(F("cluster: phase -> "));
      SerialPrintln(followerPhaseName(policyState.phase));
    }
  }

  // Rescue mode (#343): pairing and phase bookkeeping above stay live, but
  // the bus is untouchable — an accepted text is dropped unshown.
  if (rescueActive()) {
    renderPending = false;
    return;
  }

  // Blank rule: Standalone and Blank show nothing; Grace holds. One blank
  // frame per transition (rowIsBlank latches). #342: a Blank row with a
  // held membership, a known zone and synced time shows local HH:MM
  // instead — the wall keeps telling the time while the leader is down.
  if (followerPhaseShowsBlank(policyState.phase)) {
    // Quiet (#227): the leader said the wall is quiet and has since gone
    // silent. Neither blank the row nor start the fallback clock — both flap.
    if (leaderQuiet) return;
    bool synced = false;
    (void)nowEpochMs(synced);
    const FollowerFallback fallback = prefsFallback();
    if (followerClockEligible(policyState.phase, leaderHost.length() > 0,
                              leaderTz.length() > 0, synced,
                              fallback != FollowerFallback::Blank)) {
      time_t nowT = time(nullptr);
      struct tm lt;
      localtime_r(&nowT, &lt);
      const bool asDate = fallback == FollowerFallback::Date;
      int hour = lt.tm_hour, minute = asDate ? lt.tm_yday : lt.tm_min;
      if ((!clockShowing || minute != shownClockMinute || asDate != clockShowsDate) &&
          !renderPending && !reflashInProgress(reflashProgress)) {
        if (!clockShowing) {
          SerialPrintln(F("cluster: leader lost — local clock fallback"));
        }
        int width = displayWidth > 0 ? displayWidth : UNITS_AMOUNT;
        char text[UNITS_AMOUNT + 1];
        if (asDate) {
          followerDateText(lt.tm_mday, lt.tm_mon + 1, lt.tm_year % 100, width, text);
        } else {
          followerClockText(hour, minute, width, text);
        }
        busShowSegment(String(text), heldSpeed > 0 ? heldSpeed : 80);
        clockShowing = true;
        shownClockMinute = minute;
        clockShowsDate = asDate;
        rowIsBlank = false;
      }
      return;
    }
    if (!rowIsBlank && !renderPending &&
        !reflashInProgress(reflashProgress)) {
      SerialPrintln(F("cluster: blanking the row"));
      busShowSegment("", heldSpeed > 0 ? heldSpeed : 80);
      rowIsBlank = true;
      clockShowing = false;
    }
    return;
  }

  if (clockShowing && !leaderQuiet && !renderPending &&
      !reflashInProgress(reflashProgress)) {
    // The leader came back but hasn't re-rendered (its segment for this
    // row is empty): a frozen clock must not pose as leader content.
    busShowSegment("", heldSpeed > 0 ? heldSpeed : 80);
    rowIsBlank = true;
    clockShowing = false;
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
    clockShowing = false;  // leader content replaced the fallback (#342)
  }
}

void clusterPair(const String& masterId, const String& masterHost) {
  // The contact window starts now: the new master has that long to connect
  // before the row counts it as silent.
  clusterFollowerJoin(policyState, millis(), 0);
  if (leaderName != masterId) {
    // Another master: what the old one said goes with it, as on a Release.
    leaderTz = "";
    clockSetupChanged();
    leaderQuiet = false;
    renderPending = false;
  }
  if (leaderHost != masterHost) clockSetupChanged();  // the time server moved with it
  leaderName = masterId;
  leaderHost = masterHost;
  membershipDirty = true;
  SerialPrint(F("cluster: paired with "));
  SerialPrint(masterId);
  SerialPrint(F(" at "));
  SerialPrintln(masterHost);
}

void clusterMasterConnected(uint32_t epoch) {
  if (policyState.phase == ClusterFollowerPhase::Standalone) return;
  clusterFollowerJoin(policyState, millis(), epoch);
}

void clusterSetTz(const String& tz) {
  if (tz.length() == 0 || tz == leaderTz || leaderHost.length() == 0) return;
  // Stored and handed to the C library: printable, no spaces.
  if (tz.length() > FOLLOWER_TZ_MAX || !printableAscii(tz, 0x21)) return;
  leaderTz = tz;
  clockSetupChanged();
  membershipDirty = true;
}

ClusterRenderVerdict clusterHandleRender(uint32_t epoch, uint32_t seq,
                                          const String& text, int speed,
                                          uint64_t commitAtMs) {
  ClusterRenderVerdict verdict =
      clusterFollowerAcceptRender(policyState, millis(), epoch, seq);
  if (verdict == ClusterRenderVerdict::Apply) {
    bool synced = false;
    uint64_t nowMs = nowEpochMs(synced);
    heldSpeed = speed;
    renderText = text;
    renderSpeed = speed;
    renderDueMs = millis() + clusterRenderDelayMs(commitAtMs, nowMs, synced);
    renderPending = true;
  }
  return verdict;
}

void clusterNoteLeaderQuiet(bool quiet) { leaderQuiet = quiet; }

bool clusterHandlePing() {
  return clusterFollowerContact(policyState, millis());
}

void clusterHandleLeave() {
  if (policyState.phase == ClusterFollowerPhase::Standalone) return;
  clusterFollowerLeave(policyState);
  leaderName = "";
  leaderHost = "";
  leaderTz = "";  // #342: the zone leaves with the master that owned it
  leaderQuiet = false;  // #227: it described the master we left
  clockSetupChanged();
  renderPending = false;
  membershipDirty = true;  // persistMembership clears the record
  SerialPrintln(F("cluster: released — unpaired (blank)"));
}

FollowerClusterView clusterViewGet() {
  FollowerClusterView v;
  v.phase = policyState.phase;
  v.leaderName = leaderName;
  v.leaderHost = leaderHost;
  bool synced = false;
  (void)nowEpochMs(synced);
  v.sntpSynced = synced;
  return v;
}

bool clusterRenderPending() { return renderPending; }
