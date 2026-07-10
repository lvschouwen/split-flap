#pragma once
// RescueWifiPolicy.h — pure join-or-AP decision machine for the rescue app
// (#195). A trimmed COPY-derivative of Master's WifiPolicy.h (repo
// convention: rescue shares no compiled code with Master): a bounded join
// attempt on the NVS credentials, then the "<deviceName>-rescue" AP. No
// portal submission, no timeout-reboot — Ap and Online are terminal; a
// rescue image must never reboot itself out from under a recovery in
// progress (with otadata erased it would only boot rescue again). Link
// drops after Online belong to the SDK's auto-reconnect. Natively tested
// (test/test_rescue_wifi_policy).

#include <stdint.h>

// Same 30 s bound as Master's join window (v1 timing).
static const uint32_t RESCUE_JOIN_TIMEOUT_MS = 30000UL;

enum class RescuePhase : uint8_t { Boot, Joining, Ap, Online };

enum class RescueAction : uint8_t {
  None,
  StartJoin,    // begin STA join with the NVS credentials
  StartAp,      // bring up the "<deviceName>-rescue" AP + web server
  StartOnline,  // join succeeded: web server + mDNS
};

struct RescuePolicyState {
  RescuePhase phase = RescuePhase::Boot;
  uint32_t deadlineMs = 0;  // end of the join window
};

// Rollover-safe "now reached deadline" (valid while the window stays far
// under 2^31 ms; ours is 30 s).
static inline bool rescueDeadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
  return (int32_t)(nowMs - deadlineMs) >= 0;
}

// One supervision step. `linkUp` = WL_CONNECTED, `credsStored` = a usable
// ssid was read from NVS. Mutates `st`, returns the action to execute.
static inline RescueAction rescuePolicyStep(RescuePolicyState& st,
                                            uint32_t nowMs, bool linkUp,
                                            bool credsStored) {
  switch (st.phase) {
    case RescuePhase::Boot:
      if (credsStored) {
        st.phase = RescuePhase::Joining;
        st.deadlineMs = nowMs + RESCUE_JOIN_TIMEOUT_MS;
        return RescueAction::StartJoin;
      }
      st.phase = RescuePhase::Ap;
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
        return RescueAction::StartAp;
      }
      return RescueAction::None;

    case RescuePhase::Ap:
    case RescuePhase::Online:
      return RescueAction::None;

    default:
      return RescueAction::None;
  }
}
