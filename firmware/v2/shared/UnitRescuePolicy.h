#pragma once
// Runtime rescue of lost units (#498). A sketch unit that stops answering
// (heartbeat stale, #310) is never re-probed on its own: the bus layer keeps
// its boot-scan state and every render keeps writing to it. This policy
// decides when the row master spends a bounded probe on it and what happens
// after. Transport stays in UnitBus.cpp / FollowerBus.cpp:
//
//   no ACK            -> still lost; retry after UNIT_RESCUE_RETRY_MS
//   ACK + twiboot     -> CMD_SWITCH_APPLICATION exits to the sketch
//   ACK, sketch mute  -> still lost; retry (the unit hardening in #502 is what
//                        heals a deaf TWI from the inside)
//
// However a loss episode ends — twiboot exit, a unit that was unplugged or
// browned out and rebooted — the unit most likely comes back unhomed and
// blank, so its first good read re-shows the last frame (a letter command
// homes it first; units already on their letter don't move).
//
// A twiboot exit is evidence of a spontaneous unit reset the unit may not be
// able to report itself: a unit left sitting in twiboot never ran the sketch
// that counts its brownout/watchdog resets (shared/UnitResetCause.h).

#include <stdint.h>

#include "UnitHealth.h"  // UnitFacts, unitIsLost

// Minimum gap between rescue attempts on one unit. Each attempt costs a few
// bus transactions; a unit that is physically gone should not tax the bus.
#define UNIT_RESCUE_RETRY_MS 60000UL

enum class UnitRescueProbe : uint8_t {
  NoAck,         // address does not ACK
  Bootloader,    // twiboot answered and was told to start the application
  SketchSilent,  // address ACKs, not twiboot, but status reads still fail
};

struct UnitRescueState {
  bool attempted = false;       // an attempt happened in this loss episode
  uint32_t lastAttemptMs = 0;
  bool restorePending = false;  // loss episode seen, frame not re-shown yet
  uint16_t attempts = 0;        // since boot, saturating
  uint16_t exits = 0;           // twiboot exits since boot, saturating
};

inline bool unitRescueDue(const UnitFacts& u, const UnitRescueState& r,
                          uint32_t nowMs) {
  if (!unitIsLost(u)) return false;
  if (!r.attempted) return true;
  return nowMs - r.lastAttemptMs >= UNIT_RESCUE_RETRY_MS;
}

inline void unitRescueNoteAttempt(UnitRescueState& r, uint32_t nowMs,
                                  UnitRescueProbe probe) {
  r.attempted = true;
  r.lastAttemptMs = nowMs;
  if (r.attempts < 0xFFFF) r.attempts++;
  if (probe == UnitRescueProbe::Bootloader && r.exits < 0xFFFF) r.exits++;
  r.restorePending = true;
}

// Folds the unit's latest heartbeat outcome. Returns true exactly once, on
// the first good read after a loss episode: the caller re-shows the frame.
// Any recovery ends the loss episode, so the next loss is rescued at once.
inline bool unitRescueObserve(UnitRescueState& r, const UnitFacts& u) {
  if (unitIsLost(u) || !u.statusValid) return false;
  r.attempted = false;
  if (!r.restorePending) return false;
  r.restorePending = false;
  return true;
}
