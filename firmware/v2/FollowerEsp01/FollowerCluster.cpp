// FollowerCluster.cpp — cluster glue (#298). Contract in FollowerCluster.h.

#include "FollowerCluster.h"

#include <EEPROM.h>
#include <time.h>

#include "ClusterHmac.h"  // cluster-wire auth: key storage + verify (#313 follow-on)
#include "FollowerBus.h"
#include "FollowerClock.h"  // #342: local clock fallback when the leader dies
#include "FollowerConfig.h"
#include "FollowerPrefs.h"
#include "FollowerRescue.h"  // #343: rescue beacon never touches the bus
#include "FollowerSettings.h"

// #227: what the leader last said about quiet mode (the ping's quiet flag).
// RAM only — it describes a leader's state.
static volatile bool leaderQuiet = false;

static ClusterFollowerState policyState;
static String leaderName;
static String leaderHost;
// #342: the leader's POSIX zone (join body, persisted with the membership)
// — fuels the Blank-phase clock fallback. "" = pre-#342 leader, no clock.
static String leaderTz;
static int memberRow = 0;
static String heldSegment;
static int heldSpeed = 80;
static volatile bool membershipDirty = false;
// Cluster-wire auth (#313 follow-on): the leader's negotiated key. Present ⇒
// every leader-wire request must carry a valid ts+mac; absent (pre-HMAC
// leader) ⇒ fall back to #313 source-IP binding.
static ClusterMemberAuth auth;  // key + replay mark (ClusterWireGuards.h)
// Monotonic replay high-water mark (#313 follow-on HIGH#1): every accepted
// signed request must carry a strictly newer ts. Persisted coarsely in the
// EEPROM membership blob (hmacLastPersistedTs = last written value) so a
// reboot reloads it; reset durably on (re)key at join and on leave so a
// rebooted leader's fresh signing epoch is re-accepted.

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

// #342/#362: the leader's zone is installed via the ESP8266 core's
// configTime(tz) (which drives newlib's __gettzinfo) from LOOP context —
// bench-proven that a bare setenv+tzset is INERT for localtime_r here, and
// that configTime re-inits SNTP so it must never run from an async handler.
// It also must run AFTER wifiServicesInit's configTime(0,0) (which resets the
// zone to UTC) — the loop is, clusterInit isn't. This just marks a (re)install
// as needed; clusterLoopTick does it lazily once SNTP is synced.
// volatile: cleared from the async-handler context (clusterHandleJoin ->
// applyLeaderTz), read+set from loop context — same cross-context idiom as
// membershipDirty/renderPending above.
static volatile bool tzInstalled = false;
static void applyLeaderTz() { tzInstalled = false; }

void clusterInit() {
  // One mirror for both records: the membership blob and, behind it, the
  // operator preferences (#513, read by prefsInit()).
  EEPROM.begin(FOLLOWER_EEPROM_LEN);
  uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
  for (int i = 0; i < FOLLOWER_MEMBERSHIP_BLOB_LEN; i++) {
    blob[i] = EEPROM.read(i);
  }
  char name[FOLLOWER_NAME_MAX + 1];
  char host[FOLLOWER_HOST_MAX + 1];
  char tz[FOLLOWER_TZ_MAX + 1];
  uint8_t row = 0;
  bool keyValid = false;
  uint64_t mark = 0;
  bool stored = followerMembershipDecode(blob, name, host, tz, row, keyValid,
                                         auth.key, mark);
  auth.restored(stored && keyValid, mark);
  if (stored) {
    leaderName = name;
    leaderHost = host;
    leaderTz = tz;
    applyLeaderTz();  // #342: a reboot into a dead-leader window still clocks
    memberRow = row;
    SerialPrint(F("Clustered by "));
    SerialPrint(leaderName);
    SerialPrintln(F(" — booting into grace, holding for the leader"));
  }
  clusterFollowerBoot(policyState, millis(), stored);
}

static void persistMembership() {
  uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
  if (leaderHost.length() == 0 ||
      !followerMembershipEncode(leaderName.c_str(), leaderHost.c_str(),
                                leaderTz.c_str(), (uint8_t)memberRow,
                                auth.keyed, auth.key, auth.lastAcceptedTs,
                                blob)) {
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

  static uint32_t lastPhaseTickMs = 0;
  if (millis() - lastPhaseTickMs >= 1000) {
    lastPhaseTickMs = millis();
    if (clusterFollowerTick(policyState, millis())) {
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
      // #362: install the leader's zone lazily, here in loop context, the
      // first time a fallback actually needs it (and after any zone change).
      // configTime installs it synchronously via setTZ before returning, so
      // the localtime_r below is already correct this same tick. Latched, so
      // the SNTP re-init cost is paid once per fallback episode, not per tick.
      if (!tzInstalled) {
        configTime(leaderTz.c_str(), "pool.ntp.org");
        tzInstalled = true;
      }
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

bool clusterJoinWouldConflict(const String& joiningLeaderHost,
                              String& currentName, String& currentHost) {
  bool sameLeader = leaderHost == joiningLeaderHost;
  if (!clusterFollowerJoinConflicts(policyState, millis(), sameLeader)) {
    return false;
  }
  currentName = leaderName;
  currentHost = leaderHost;
  return true;
}

void clusterHandleJoin(const String& name, const String& host, int row,
                       uint32_t epoch, const String& key, const String& tz) {
  clusterFollowerJoin(policyState, millis(), epoch);
  // Adopt the negotiated wire-auth key (#313 follow-on): a valid key turns
  // enforcement ON; a pre-HMAC leader sends none.
  // Fresh key material resets the replay mark (ClusterMemberAuth::adoptKey);
  // the `changed` persist below rewrites the blob with that reset mark, so a
  // follower reboot after the rekey won't reload a stale-high mark.
  bool keyChanged = auth.adoptKey(key);
  // NVS/EEPROM-flood guard (#313, parity with the S3 follower): persist the
  // membership blob only when a field actually moved — a leader re-joining
  // on its cadence must not burn EEPROM on every accepted join.
  // #342: tz is additive — a pre-#342 leader sends none and an emptied one
  // keeps the last known zone (a zone is better than a dark row).
  bool tzChanged = tz.length() > 0 && tz != leaderTz;
  bool changed = leaderName != name || leaderHost != host ||
                 memberRow != row || keyChanged || tzChanged;
  leaderName = name;
  leaderHost = host;
  if (tzChanged) {
    leaderTz = tz;
    applyLeaderTz();
  }
  memberRow = row;
  if (changed) {
    membershipDirty = true;
    SerialPrint(F("cluster: joined by "));
    SerialPrint(name);
    SerialPrintln(auth.keyed ? F(" [authenticated]") : F(""));
  }
}

void clusterSetTz(const String& tz) {
  if (tz.length() == 0 || tz == leaderTz || leaderHost.length() == 0) return;
  // Stored and handed to the C library: the same rule as the join's tz.
  if (tz.length() > FOLLOWER_TZ_MAX || !clusterWirePrintable(tz, 0x21)) return;
  leaderTz = tz;
  applyLeaderTz();
  membershipDirty = true;
}

bool clusterHmacEnforced() { return auth.keyed; }

bool clusterMacMatches(const String& msg, const String& macHex) {
  return auth.macMatches(msg, macHex);
}

bool clusterVerifySigned(const String& canonicalMsg, uint64_t ts,
                         const String& macHex) {
  bool synced = false;
  uint64_t nowMs = nowEpochMs(synced);
  bool markDue = false;
  bool ok = auth.accept(canonicalMsg, ts, macHex, nowMs, synced, markDue);
  if (markDue) {
    auth.markPersisted();
    membershipDirty = true;  // loop() rewrites the blob (mark included)
  }
  return ok;
}

ClusterRenderVerdict clusterHandleRender(uint32_t epoch, uint32_t seq,
                                          const String& text, int speed,
                                          uint64_t commitAtMs) {
  ClusterRenderVerdict verdict =
      clusterFollowerAcceptRender(policyState, millis(), epoch, seq);
  if (verdict == ClusterRenderVerdict::Apply) {
    bool synced = false;
    uint64_t nowMs = nowEpochMs(synced);
    heldSegment = text;
    heldSpeed = speed;
    renderText = text;
    renderSpeed = speed;
    renderDueMs = millis() + clusterRenderDelayMs(commitAtMs, nowMs, synced);
    renderPending = true;
    lastRenderMs = millis();
    haveRender = true;
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
  leaderTz = "";  // #342: the zone leaves with the leader that owned it
  leaderQuiet = false;  // #227: it described the leader we left
  applyLeaderTz();  // #362: re-arm the tz latch (defensive — eligibility also
                    // gates on leaderTz, but this survives a future refactor)
  memberRow = 0;
  heldSegment = "";
  renderPending = false;
  auth.drop();  // #313 follow-on: the wire-auth key and its replay mark
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
  // Total silence blanks the row at lastContactMs + CLUSTER_GRACE_MS (the
  // tick cascades Clustered->Grace->Blank on cumulative silence). Only
  // meaningful while still showing content.
  if (policyState.phase == ClusterFollowerPhase::Clustered ||
      policyState.phase == ClusterFollowerPhase::Grace) {
    int32_t remainMs =
        (int32_t)(policyState.lastContactMs + CLUSTER_GRACE_MS - millis());
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
