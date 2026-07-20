#pragma once
// HeadlessPolicy.h — pure no-units detection debounce + the deviceRole wire
// vocabulary (#329). A physically unit-less S3 master should stop enrolling
// itself as an empty phantom display row and take a genuinely useful role
// instead; this header owns the single-sourced role strings, their validator,
// and the debounce that decides WHEN a board is unit-less enough to suggest a
// headless role. Every symbol is pure (String only) so it is natively tested
// (test/test_headless_policy) and shared by Settings.h and the /settings
// surface. The role BEHAVIOURS live elsewhere (later slices).

#include <Arduino.h>

// deviceRole wire values — mirror deviceMode's frozen /settings vocabulary.
// "display" is the default: a unit-driving wall row. The three headless roles
// are non-rendering and occupy no wall columns (width 0).
#define DEVICE_ROLE_DISPLAY          "display"
#define DEVICE_ROLE_HEADLESS_BACKUP  "headless-backup"   // warm-standby master
#define DEVICE_ROLE_HEADLESS_MONITOR "headless-monitor"  // cluster dashboard
#define DEVICE_ROLE_HEADLESS_SPARE   "headless-spare"    // idle clean spare

// deviceRole POST validator — exact match against the four wire values, like
// isValidDeviceModeValue. Rejects case variants, empty and unknowns so a raw
// POST can never persist garbage.
static inline bool isValidDeviceRoleValue(const String& v) {
  return v == DEVICE_ROLE_DISPLAY ||
         v == DEVICE_ROLE_HEADLESS_BACKUP ||
         v == DEVICE_ROLE_HEADLESS_MONITOR ||
         v == DEVICE_ROLE_HEADLESS_SPARE;
}

// True for any of the three non-rendering roles (i.e. not "display").
static inline bool isHeadlessRole(const String& role) {
  return role == DEVICE_ROLE_HEADLESS_BACKUP ||
         role == DEVICE_ROLE_HEADLESS_MONITOR ||
         role == DEVICE_ROLE_HEADLESS_SPARE;
}

// Consecutive 0-unit probes before a board is flagged unit-less. The display
// probe/health poll runs on a multi-second cadence, so a few in a row is a
// real "nothing on the bus" verdict — long enough that a transient bus glitch
// on a genuine display never trips it.
static constexpr int HEADLESS_ZERO_PROBE_THRESHOLD = 3;

// Debounce state — a plain counter; the owner (displayTask) keeps one and
// feeds every fresh probe through headlessObserveProbe.
struct HeadlessDetector {
  int zeroStreak = 0;
};

// Feed one probe's detected-unit count. Returns true once the board has seen
// `threshold` CONSECUTIVE zero-unit probes (unit-less). Any probe that finds a
// unit resets the streak, so a real display whose units blink out briefly is
// never flagged. The counter saturates at the threshold — no unbounded growth.
static inline bool headlessObserveProbe(
    HeadlessDetector& d, int detectedUnitCount,
    int threshold = HEADLESS_ZERO_PROBE_THRESHOLD) {
  if (detectedUnitCount > 0) {
    d.zeroStreak = 0;
    return false;
  }
  if (d.zeroStreak < threshold) d.zeroStreak++;
  return d.zeroStreak >= threshold;
}

// Whether the UI should surface the "you look unit-less — pick a headless
// role" suggestion: detection has latched AND the user is still on the default
// display role (a board already in a headless role needs no nudge). Detection
// only ever SUGGESTS — every role change is user-confirmed (#329 safety).
static inline bool headlessShouldSuggest(bool unitlessDetected,
                                         const String& role) {
  return unitlessDetected && role == DEVICE_ROLE_DISPLAY;
}
