#pragma once
// StatusLedPolicy.h — pure state→color decision for the onboard WS2812
// status LED (#199). No LED/RMT types in here — StatusLed.cpp owns
// rgbLedWrite(); this header only answers "what color is the device's
// state right now", natively tested (test/test_status_led_policy).
//
// Priority: an in-flight OTA upload (flash writes — the thing to never
// interrupt) outranks an unconfirmed PENDING_VERIFY image outranks
// connectivity. A Portal despite stored credentials means the join failed
// and the device is cycling toward the reboot-retry — that is the fault
// red; the first-boot portal (no credentials) is a calm setup blue.

#include <stdint.h>

#include "WifiPolicy.h"

// Attention states must be visible across a room; the healthy heartbeat is
// deliberately dim — this device lives in a living room (user call
// 2026-07-10), so "all good" may not double as a night light.
static const uint8_t STATUS_LED_ATTENTION = 64;
static const uint8_t STATUS_LED_HEARTBEAT_MIN = 1;  // never fully dark:
                                                    // off must keep meaning
                                                    // "no power/firmware"
static const uint8_t STATUS_LED_HEARTBEAT_MAX = 12;
static const uint32_t STATUS_LED_BREATH_PERIOD_MS = 4000;

struct StatusLedColor {
  uint8_t r = 0, g = 0, b = 0;
};

static inline bool statusLedColorEq(const StatusLedColor& a,
                                    const StatusLedColor& b) {
  return a.r == b.r && a.g == b.g && a.b == b.b;
}

struct StatusLedInput {
  WifiPhase wifiPhase = WifiPhase::Boot;
  bool credsStored = false;       // Portal + stored creds = failed join
  bool otaUploadActive = false;   // Update.isRunning()
  bool otaPendingVerify = false;  // this boot unconfirmed, rollback armed
  uint32_t nowMs = 0;             // drives the healthy breathing ramp
};

// Symmetric triangle wave between the heartbeat bounds; nowMs % period is
// well-defined across the uint32 millis rollover.
static inline uint8_t statusLedBreathLevel(uint32_t nowMs) {
  const uint32_t half = STATUS_LED_BREATH_PERIOD_MS / 2;
  const uint32_t phase = nowMs % STATUS_LED_BREATH_PERIOD_MS;
  const uint32_t rise = phase < half ? phase : STATUS_LED_BREATH_PERIOD_MS - phase;
  return (uint8_t)(STATUS_LED_HEARTBEAT_MIN +
                   rise * (STATUS_LED_HEARTBEAT_MAX - STATUS_LED_HEARTBEAT_MIN) /
                       half);
}

static inline StatusLedColor decideStatusLed(const StatusLedInput& in) {
  if (in.otaUploadActive) {
    return {STATUS_LED_ATTENTION, 0, STATUS_LED_ATTENTION};  // purple
  }
  if (in.otaPendingVerify) {
    return {STATUS_LED_ATTENTION, 12, 0};  // orange
  }
  switch (in.wifiPhase) {
    case WifiPhase::Boot:
      return {STATUS_LED_ATTENTION, STATUS_LED_ATTENTION,
              STATUS_LED_ATTENTION};  // white
    case WifiPhase::Joining:
      return {STATUS_LED_ATTENTION, 48, 0};  // yellow
    case WifiPhase::Portal:
      if (in.credsStored) {
        return {STATUS_LED_ATTENTION, 0, 0};  // red: join failed
      }
      return {0, 0, STATUS_LED_ATTENTION};  // blue: first-boot setup
    case WifiPhase::Connected:
    default:
      return {0, statusLedBreathLevel(in.nowMs), 0};  // dim green heartbeat
  }
}
