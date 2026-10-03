#pragma once
// WifiPolicy.h — pure join/portal supervision state machine (#188).
//
// The v2 master's WiFi bring-up policy: bounded join attempts on stored
// credentials, then the "<deviceName>-setup" portal, then a reboot-retry
// cycle. No radio types in here — WifiService.cpp owns the esp_wifi calls
// and feeds this step function from netTask; everything decision-shaped is
// natively tested (test/test_wifi_policy).
//
// A join window that closes without a link is retried by REBOOTING, up to
// WIFI_JOIN_ATTEMPTS boots in a row, before the portal opens (#524). The
// portal parks the board off the network for its whole window, which is the
// right answer to wrong credentials and the wrong first answer to one failed
// association on a wall nobody is standing at. A reboot rather than a second
// begin(): a fresh boot is what has been seen to un-stick this radio (#328).
//
// Once Connected, brief link drops belong to the SDK's auto-reconnect
// (WiFi.setAutoReconnect(true)) and never re-open the portal. But that path
// is known to wedge on the ESP32-S3 after an AP power-cycle (esp_wifi +
// WiFi.persistent(false) leaves nothing to un-stick the STA), stranding the
// board off-network until a manual power cycle (#328). So a bounded reconnect
// watchdog reboots after a sustained outage — the same "router may just have
// been down" retry the portal-timeout path already uses. We reboot; we still
// never re-open the portal.

#include <stdint.h>

static const uint32_t WIFI_JOIN_TIMEOUT_MS = 30000UL;
static const uint32_t WIFI_PORTAL_TIMEOUT_MS = 300000UL;
// Boots in a row that may spend a join window before the portal opens. Three
// windows plus two reboots reach the portal in about two minutes when the
// network is really gone.
static const uint8_t WIFI_JOIN_ATTEMPTS = 3;
// #328: continuous Connected-phase link loss tolerated before a recovery
// reboot. Long enough that the SDK auto-reconnect owns transient blips; short
// enough that an AP-reboot wedge self-heals in ~1.5 min instead of never.
static const uint32_t WIFI_RECONNECT_TIMEOUT_MS = 90000UL;

enum class WifiPhase : uint8_t { Boot, Joining, Portal, Connected };

enum class WifiAction : uint8_t {
  None,
  StartJoin,       // begin STA join with the stored credentials
  StartPortal,     // bring up SoftAP + DNS catch-all + web server
  StartOnline,     // join succeeded: web server + mDNS
  RetryJoin,       // join window closed with attempts left: restart, try again
  SaveAndReboot,   // persist the portal-submitted credentials, then restart
  Reboot,          // portal expired unconfigured: restart to retry the join
};

struct WifiPolicyState {
  WifiPhase phase = WifiPhase::Boot;
  uint32_t deadlineMs = 0;  // end of the current join/portal window
  // Boots right before this one whose join window closed without a link
  // (wifiJoinRetryDecode). Seeded once by the caller before the first step.
  uint8_t failedJoinBoots = 0;
  // #328: millis() at which a Connected-phase link loss began; 0 = link up
  // (or not yet tracking). The reconnect watchdog fires on continuous
  // downtime, so any link-up tick clears it.
  uint32_t linkDownSinceMs = 0;
};

// The failed-join tally between boots. It lives in RAM that a software reset
// keeps and a power cycle does not, so pulling the plug always starts over
// with a full set of attempts — and that RAM holds garbage after power-on,
// which is why the count travels with a magic and its own complement.
#define WIFI_JOIN_RETRY_MAGIC 0x524A4657UL  // "WFJR" LE

struct WifiJoinRetryRecord {
  uint32_t magic;
  uint8_t failed;
  uint8_t check;  // ~failed
};

inline void wifiJoinRetryEncode(WifiJoinRetryRecord& r, uint8_t failed) {
  r.magic = WIFI_JOIN_RETRY_MAGIC;
  r.failed = failed;
  r.check = (uint8_t)~failed;
}

// Anything that is not a record this code wrote decodes as 0 failed boots.
// Capped below the attempt count: a tally that says "no attempts left" before
// this boot has tried would send a board straight to the portal.
inline uint8_t wifiJoinRetryDecode(const WifiJoinRetryRecord& r) {
  if (r.magic != WIFI_JOIN_RETRY_MAGIC) return 0;
  if (r.check != (uint8_t)~r.failed) return 0;
  if (r.failed >= WIFI_JOIN_ATTEMPTS) return (uint8_t)(WIFI_JOIN_ATTEMPTS - 1);
  return r.failed;
}

// Rollover-safe "now reached deadline": valid while the window length stays
// far under 2^31 ms (ours are 30 s / 300 s).
static inline bool wifiDeadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
  return (int32_t)(nowMs - deadlineMs) >= 0;
}

// May the caller restart the board for this action right now? The two
// restarts that only exist to try the join again wait for a unit reflash to
// finish: cutting one leaves that unit in its bootloader with half an image,
// and the next boot's job would be cut the same way. Both actions are returned
// on every step past their deadline, so waiting needs no state. Every other
// restart goes ahead — the link-loss watchdog's included, which is the only
// way back onto the network.
inline bool wifiRestartMayProceed(WifiAction action, WifiPhase phase,
                                  bool unitReflashRunning) {
  if (!unitReflashRunning) return true;
  if (action == WifiAction::RetryJoin) return false;
  if (action == WifiAction::Reboot && phase == WifiPhase::Portal) return false;
  return true;
}

// One supervision step. `linkUp` = WL_CONNECTED, `credsStored` = a usable
// ssid is in settings, `portalSubmitted` = a validated portal config post
// is staged. Mutates `st`, returns the action the caller must execute.
static inline WifiAction wifiPolicyStep(WifiPolicyState& st, uint32_t nowMs,
                                        bool linkUp, bool credsStored,
                                        bool portalSubmitted) {
  switch (st.phase) {
    case WifiPhase::Boot:
      if (credsStored) {
        st.phase = WifiPhase::Joining;
        st.deadlineMs = nowMs + WIFI_JOIN_TIMEOUT_MS;
        return WifiAction::StartJoin;
      }
      // No credentials: a 30 s wait for a join that cannot happen would
      // just delay the portal (v1: tryJoinKnownWifi() -> immediate false).
      st.phase = WifiPhase::Portal;
      st.deadlineMs = nowMs + WIFI_PORTAL_TIMEOUT_MS;
      return WifiAction::StartPortal;

    case WifiPhase::Joining:
      // Link check first: connected-at-the-deadline must go online, not
      // throw away a live association for a portal.
      if (linkUp) {
        st.phase = WifiPhase::Connected;
        return WifiAction::StartOnline;
      }
      if (wifiDeadlineReached(nowMs, st.deadlineMs)) {
        // Returned on every step past the deadline, like the portal's Reboot:
        // the caller restarts and stops stepping.
        if ((uint8_t)(st.failedJoinBoots + 1) < WIFI_JOIN_ATTEMPTS) {
          return WifiAction::RetryJoin;
        }
        st.phase = WifiPhase::Portal;
        st.deadlineMs = nowMs + WIFI_PORTAL_TIMEOUT_MS;
        return WifiAction::StartPortal;
      }
      return WifiAction::None;

    case WifiPhase::Portal:
      // Submission wins over a simultaneous timeout: a save in the dying
      // tick must not be dropped for a plain retry reboot.
      if (portalSubmitted) {
        return WifiAction::SaveAndReboot;
      }
      if (wifiDeadlineReached(nowMs, st.deadlineMs)) {
        // Parked, not stuck: reboot and retry the stored credentials —
        // the router may just have been down (v1 portal-timeout path).
        return WifiAction::Reboot;
      }
      return WifiAction::None;

    case WifiPhase::Connected:
      // The portal page doubles as a "move to another network" page from
      // the LAN: a validated submission here persists + reboots exactly
      // like a portal one. It wins even mid-outage.
      if (portalSubmitted) {
        return WifiAction::SaveAndReboot;
      }
      // Healthy link: clear any armed outage clock and stay parked — the SDK
      // auto-reconnect owns transient drops.
      if (linkUp) {
        st.linkDownSinceMs = 0;
        return WifiAction::None;
      }
      // Link is down. Arm the outage clock on the first down tick (0 is the
      // sentinel for "up"; nudge an exact-0 timestamp to 1 so it never reads
      // as unarmed). Reboot only on CONTINUOUS downtime past the window —
      // any link-up tick above resets it. #328: this un-wedges the S3's
      // auto-reconnect after an AP power-cycle. We reboot; never re-open the
      // portal.
      if (st.linkDownSinceMs == 0) {
        st.linkDownSinceMs = nowMs ? nowMs : 1;
      }
      if (wifiDeadlineReached(nowMs,
                              st.linkDownSinceMs + WIFI_RECONNECT_TIMEOUT_MS)) {
        return WifiAction::Reboot;
      }
      return WifiAction::None;

    default:
      return WifiAction::None;
  }
}
