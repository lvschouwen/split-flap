#pragma once
// WifiTxPolicy.h — pure WiFi TX-power ladder (#507).
//
// TX bursts are the S3's own current peak on the 5 V rail it shares with the
// units; a boot that went straight to the SDK default died on a corrupted
// instruction fetch (#506). So every boot starts at the lowest real level and
// moves ONE level at a time: up only when the link needs it, down again after
// a long healthy period. Nothing is remembered across reboots — a step-up
// that browns the board out cannot become a boot loop.
//
// No radio types in here: WifiService.cpp feeds this from netTask and applies
// the index it returns. Natively tested (test/test_wifi_tx_policy).
//
// The board cannot measure how well the access point hears it. The received
// signal strength is the proxy (path loss is symmetric); link drops are the
// hard evidence.

#include <stdint.h>

// The real levels of esp_wifi_set_max_tx_power(), in its 0.25 dBm unit. The
// IDF rounds any other request DOWN onto one of these, so only this table
// makes "one step" mean one real level.
static const int8_t WIFI_TX_LEVELS_RAW[] = {8,  20, 28, 34, 44, 52,
                                            56, 60, 66, 72, 80};
static const uint8_t WIFI_TX_LEVEL_COUNT =
    sizeof(WIFI_TX_LEVELS_RAW) / sizeof(WIFI_TX_LEVELS_RAW[0]);

// 8.5 dBm: the level bench-proven on the shared supply (#506). The join never
// climbs past it — an access point that is simply off must not ramp the board
// up during boot, where the supply margin is thinnest.
static const uint8_t WIFI_TX_JOIN_CEILING_INDEX = 3;
// 13 dBm, until the supply current per level is measured (18 dBm crashed).
static const uint8_t WIFI_TX_ONLINE_CEILING_INDEX = 5;
static const uint8_t WIFI_TX_PORTAL_INDEX = 3;

// A join takes ~5 s on the bench whatever the power (scan + DHCP), so a level
// gets longer than that before it counts as too weak; three steps still reach
// the ceiling with time to join inside WifiPolicy's 30 s window.
static const uint32_t WIFI_TX_JOIN_STEP_MS = 7000UL;
// The first stretch online is still the boot window (units homing).
static const uint32_t WIFI_TX_SETTLE_MS = 60000UL;
static const uint32_t WIFI_TX_WEAK_MS = 60000UL;      // weak this long -> up
static const uint32_t WIFI_TX_UP_DWELL_MS = 60000UL;  // min between up-steps
static const uint32_t WIFI_TX_HEALTHY_MS = 600000UL;  // spare this long -> down

// Uplink estimate, 0.25 dB units: what the access point hears from us is
// what we hear from it, less the gap between its TX power (assumed 20 dBm)
// and ours.
static const int16_t WIFI_TX_AP_ASSUMED_RAW = 80;
static const int16_t WIFI_TX_WEAK_UPLINK_RAW = -80 * 4;
// A step down must leave this much above the weak floor, or the same signal
// that earned a level would hand it straight back.
static const int16_t WIFI_TX_DOWN_MARGIN_RAW = 6 * 4;

enum class WifiTxPhase : uint8_t { Joining, Online, Portal };

struct WifiTxInput {
  WifiTxPhase phase = WifiTxPhase::Joining;
  bool linkUp = false;    // Online only: WL_CONNECTED
  int rssiDbm = 0;        // Online + linkUp only
  bool unitsIdle = true;  // no unit is moving — up-steps wait for this
};

struct WifiTxState {
  uint8_t index = 0;  // into WIFI_TX_LEVELS_RAW; every boot starts lowest
  bool started = false;
  WifiTxPhase phase = WifiTxPhase::Joining;
  uint32_t joinLevelSinceMs = 0;
  uint32_t upAllowedAtMs = 0;  // settle / dwell gate for the next up-step
  bool wasLinkUp = false;
  bool dropPending = false;  // a link drop not yet answered with a step
  bool weakArmed = false;
  uint32_t weakSinceMs = 0;
  bool spareArmed = false;
  uint32_t spareSinceMs = 0;
  uint16_t stepsUp = 0;
  uint16_t stepsDown = 0;
};

static inline int8_t wifiTxLevelRaw(uint8_t index) {
  if (index >= WIFI_TX_LEVEL_COUNT) index = WIFI_TX_LEVEL_COUNT - 1;
  return WIFI_TX_LEVELS_RAW[index];
}

// Tenths of a dBm, for the status JSON.
static inline int wifiTxLevelDbm10(uint8_t index) {
  return wifiTxLevelRaw(index) * 10 / 4;
}

// Rollover-safe "now reached t".
static inline bool wifiTxReached(uint32_t nowMs, uint32_t tMs) {
  return (int32_t)(nowMs - tMs) >= 0;
}

static inline int16_t wifiTxUplinkRaw(int rssiDbm, uint8_t index) {
  return (int16_t)(rssiDbm * 4 + wifiTxLevelRaw(index) -
                   WIFI_TX_AP_ASSUMED_RAW);
}

static inline void wifiTxStepUp(WifiTxState& st, uint32_t nowMs) {
  st.index++;
  st.stepsUp++;
  st.upAllowedAtMs = nowMs + WIFI_TX_UP_DWELL_MS;
  st.weakArmed = false;
  st.spareArmed = false;
}

// One supervision step; returns the level index the radio should be at. The
// index changes by at most one per call.
static inline uint8_t wifiTxPolicyStep(WifiTxState& st, const WifiTxInput& in,
                                       uint32_t nowMs) {
  bool entered = !st.started || st.phase != in.phase;
  st.started = true;
  st.phase = in.phase;

  switch (in.phase) {
    case WifiTxPhase::Portal:
      st.index = WIFI_TX_PORTAL_INDEX;
      return st.index;

    case WifiTxPhase::Joining:
      if (entered) {
        st.joinLevelSinceMs = nowMs;
      } else if (st.index < WIFI_TX_JOIN_CEILING_INDEX &&
                 wifiTxReached(nowMs,
                               st.joinLevelSinceMs + WIFI_TX_JOIN_STEP_MS)) {
        st.index++;
        st.stepsUp++;
        st.joinLevelSinceMs = nowMs;
      }
      return st.index;

    case WifiTxPhase::Online:
      break;
  }

  if (entered) {
    st.upAllowedAtMs = nowMs + WIFI_TX_SETTLE_MS;
    st.wasLinkUp = in.linkUp;
    st.dropPending = false;
    st.weakArmed = false;
    st.spareArmed = false;
  }

  bool atCeiling = st.index >= WIFI_TX_ONLINE_CEILING_INDEX;
  bool canStepUp = !atCeiling && in.unitsIdle &&
                   wifiTxReached(nowMs, st.upAllowedAtMs);

  if (!in.linkUp) {
    // At the ceiling a drop has nothing to buy; left pending it would hold
    // the step-down off for the rest of the boot.
    if (st.wasLinkUp && !atCeiling) st.dropPending = true;
    st.wasLinkUp = false;
    st.weakArmed = false;
    st.spareArmed = false;
    if (st.dropPending && canStepUp) {
      st.dropPending = false;
      wifiTxStepUp(st, nowMs);
    }
    return st.index;
  }
  st.wasLinkUp = true;

  if (st.dropPending && canStepUp) {
    st.dropPending = false;
    wifiTxStepUp(st, nowMs);
    return st.index;
  }

  bool weak = wifiTxUplinkRaw(in.rssiDbm, st.index) < WIFI_TX_WEAK_UPLINK_RAW;
  if (!weak) {
    st.weakArmed = false;
  } else if (!st.weakArmed) {
    st.weakArmed = true;
    st.weakSinceMs = nowMs;
  } else if (canStepUp &&
             wifiTxReached(nowMs, st.weakSinceMs + WIFI_TX_WEAK_MS)) {
    wifiTxStepUp(st, nowMs);
    return st.index;
  }

  bool spare = st.index > 0 && !st.dropPending &&
               wifiTxUplinkRaw(in.rssiDbm, st.index - 1) >=
                   WIFI_TX_WEAK_UPLINK_RAW + WIFI_TX_DOWN_MARGIN_RAW;
  if (!spare) {
    st.spareArmed = false;
  } else if (!st.spareArmed) {
    st.spareArmed = true;
    st.spareSinceMs = nowMs;
  } else if (wifiTxReached(nowMs, st.spareSinceMs + WIFI_TX_HEALTHY_MS)) {
    st.index--;
    st.stepsDown++;
    st.spareArmed = false;
  }
  return st.index;
}
