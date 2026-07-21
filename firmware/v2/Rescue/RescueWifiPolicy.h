#pragma once
// RescueWifiPolicy.h — pure join-or-AP decision machine for the rescue app
// (#195). A trimmed COPY-derivative of Master's WifiPolicy.h (repo
// convention: rescue shares no compiled code with Master): a bounded join
// attempt on the NVS credentials, then the "<deviceName>-rescue" AP. No
// portal submission, no timeout-reboot — a rescue image must never reboot
// itself out from under a recovery in progress (with otadata erased it
// would only boot rescue again). #349: the AP is no longer terminal when
// credentials are stored — an idle AP retries the STA join every
// RESCUE_AP_RETRY_MS so the open flash-capable network doesn't outlive a
// transient LAN outage; any associated station holds it open, and with no
// stored credentials the AP is the only path and stays up. Online stays
// terminal; link drops there belong to the SDK's auto-reconnect. Natively
// tested (test/test_rescue_wifi_policy).

#include <stdint.h>

// Same 30 s bound as Master's join window (v1 timing).
static const uint32_t RESCUE_JOIN_TIMEOUT_MS = 30000UL;

// #349: idle-AP STA-retry window — same 300 s bound as the follower's
// portal. Counted from AP start or from the last moment a station was
// associated, so a recovery in progress is never interrupted.
static const uint32_t RESCUE_AP_RETRY_MS = 300000UL;

enum class RescuePhase : uint8_t { Boot, Joining, Ap, Online };

enum class RescueAction : uint8_t {
  None,
  StartJoin,    // begin STA join with the NVS credentials
  StartAp,      // bring up the "<deviceName>-rescue" AP + web server
  StartOnline,  // join succeeded: web server + mDNS
};

struct RescuePolicyState {
  RescuePhase phase = RescuePhase::Boot;
  uint32_t deadlineMs = 0;  // end of the join window / AP retry window (#349)
};

// Rollover-safe "now reached deadline" (valid while the window stays far
// under 2^31 ms; ours is 30 s).
static inline bool rescueDeadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
  return (int32_t)(nowMs - deadlineMs) >= 0;
}

// One supervision step. `linkUp` = WL_CONNECTED, `credsStored` = a usable
// ssid was read from NVS, `apStations` = currently associated AP clients
// (0 when the AP is down). Mutates `st`, returns the action to execute.
static inline RescueAction rescuePolicyStep(RescuePolicyState& st,
                                            uint32_t nowMs, bool linkUp,
                                            bool credsStored,
                                            int apStations) {
  switch (st.phase) {
    case RescuePhase::Boot:
      if (credsStored) {
        st.phase = RescuePhase::Joining;
        st.deadlineMs = nowMs + RESCUE_JOIN_TIMEOUT_MS;
        return RescueAction::StartJoin;
      }
      st.phase = RescuePhase::Ap;
      st.deadlineMs = nowMs + RESCUE_AP_RETRY_MS;
      return RescueAction::StartAp;

    case RescuePhase::Joining:
      // Link check first: connected-at-the-deadline must go online, not
      // throw away a live association for an AP.
      if (linkUp) {
        st.phase = RescuePhase::Online;
        return RescueAction::StartOnline;
      }
      if (rescueDeadlineReached(nowMs, st.deadlineMs)) {
        st.phase = RescuePhase::Ap;
        st.deadlineMs = nowMs + RESCUE_AP_RETRY_MS;
        return RescueAction::StartAp;
      }
      return RescueAction::None;

    case RescuePhase::Ap:
      // #349: idle-AP STA retry. No credentials -> the AP is the only
      // recovery path, park forever. Any associated station -> a recovery
      // may be in progress, slide the window. Idle past the window ->
      // re-attempt the join (a failed retry returns here with a fresh AP).
      if (!credsStored) return RescueAction::None;
      if (apStations > 0) {
        st.deadlineMs = nowMs + RESCUE_AP_RETRY_MS;
        return RescueAction::None;
      }
      if (rescueDeadlineReached(nowMs, st.deadlineMs)) {
        st.phase = RescuePhase::Joining;
        st.deadlineMs = nowMs + RESCUE_JOIN_TIMEOUT_MS;
        return RescueAction::StartJoin;
      }
      return RescueAction::None;

    case RescuePhase::Online:
      return RescueAction::None;

    default:
      return RescueAction::None;
  }
}
