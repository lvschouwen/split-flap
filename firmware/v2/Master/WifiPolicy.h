#pragma once
// WifiPolicy.h — pure join/portal supervision state machine (#188).
//
// The v2 master's WiFi bring-up policy, v1-parity by construction
// (ServiceWifiFunctions.ino initWiFi()): a bounded join attempt on stored
// credentials, then the "<deviceName>-setup" portal, then a reboot-retry
// cycle. No radio types in here — WifiService.cpp owns the esp_wifi calls
// and feeds this step function from netTask; everything decision-shaped is
// natively tested (test/test_wifi_policy).
//
// Once Connected the machine is parked for good: link drops belong to the
// SDK's auto-reconnect (WiFi.setAutoReconnect(true)), never a portal
// re-entry. Recovery/quiet-OTA boot modes (later #58 slice) branch BEFORE
// the first step call — those contexts must never open a portal.

#include <stdint.h>

// v1 timings verbatim (user decision 2026-07-09 on #58).
static const uint32_t WIFI_JOIN_TIMEOUT_MS = 30000UL;
static const uint32_t WIFI_PORTAL_TIMEOUT_MS = 300000UL;

enum class WifiPhase : uint8_t { Boot, Joining, Portal, Connected };

enum class WifiAction : uint8_t {
  None,
  StartJoin,       // begin STA join with the stored credentials
  StartPortal,     // bring up SoftAP + DNS catch-all + web server
  StartOnline,     // join succeeded: web server + mDNS
  SaveAndReboot,   // persist the portal-submitted credentials, then restart
  Reboot,          // portal expired unconfigured: restart to retry the join
};

struct WifiPolicyState {
  WifiPhase phase = WifiPhase::Boot;
  uint32_t deadlineMs = 0;  // end of the current join/portal window
};

// Rollover-safe "now reached deadline": valid while the window length stays
// far under 2^31 ms (ours are 30 s / 300 s).
static inline bool wifiDeadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
  return (int32_t)(nowMs - deadlineMs) >= 0;
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
      // like a portal one. Link drops still never re-open the portal.
      if (portalSubmitted) {
        return WifiAction::SaveAndReboot;
      }
      return WifiAction::None;

    default:
      return WifiAction::None;
  }
}
