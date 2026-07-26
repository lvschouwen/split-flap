#pragma once

// ReflashPlan.h — pure planning + progress core of the unit reflash job
// (#205, slice C of the I2C port). Who gets the enter-bootloader opcode,
// who gets flashed, the per-unit progress the display task publishes, and
// the job-level MaintResult grading. No Wire, no RTOS — natively tested by
// test_reflash_plan. The hardware execution lives in UnitBus.cpp/Tasks.cpp.

#include "MaintenancePolicy.h"
#include "UnitHealth.h"

// v1 #138 brownout throttle: flash at most this many units per batch, then
// wait for the batch to come back online + finish homing before the next —
// post-flash homing current on a supply shared with the steppers. 4 (#250)
// is bench-gated: a full-display reflash must leave every unit's lifetime
// brownout counter (#139) flat, else revert to 2.
#define REFLASH_BATCH_SIZE 4
#define REFLASH_BATCH_SETTLE_MS 15000UL
// Wait after CMD_ENTER_BOOTLOADER before talking to twiboot: watchdog reset
// (~15 ms) + twiboot init. 500 ms is generous (v1 value).
#define TWIBOOT_STARTUP_MS 500

// Halt a run after this many CONSECUTIVE unit failures (#412). The job used to
// log a failed unit and walk straight on to the next one, so an image that
// cannot flash — or flashes and does not boot — took the whole row down one
// unit at a time with nobody stopping it.
//
// Consecutive rather than total, because the two failure shapes need opposite
// answers and their signatures differ:
//   A BAD IMAGE fails on every unit it touches. Two in a row is already
//       conclusive, and the run stops having burned two rather than 21.
//   A DEAD UNIT fails alone. a15 is hall-dead today; a first-failure halt would
//       let it wedge every fleet converge from now on, including the unattended
//       boot auto-install. One success resets the count and the sweep continues.
// Applies to both sweeps. The boot path is where nobody is watching, which is
// where an unbounded failure walk is worst.
#define REFLASH_MAX_CONSECUTIVE_FAILURES 2

inline bool reflashShouldHalt(uint8_t consecutiveFailures) {
  return consecutiveFailures >= REFLASH_MAX_CONSECUTIVE_FAILURES;
}

// A unit that reports a protocol version we do not speak (#405). KNOWN
// different, not merely unreadable — the version read succeeded and carried a
// number that is not ours. We cannot drive such a unit at all (see
// unitOpcodeAllowedWhenUnsupported: GET_VERSION and ENTER_BOOTLOADER only), so
// converging it is the only way it becomes useful again.
//
// Deliberately EQUALITY-based: a higher version is exactly as un-drivable as a
// lower one, because the master cannot speak a contract it has no code for.
inline bool reflashUnitProtocolMismatch(const UnitFacts& u) {
  return u.protocolKnown && !unitProtocolSupported(u.protocolVersion);
}

// A sketch-mode unit whose rev is not the bundled one gets pushed into
// twiboot. UNKNOWN qualifies alongside OUTDATED (v1 #114: only units
// provably on the bundled rev are skipped). Bootloader units need no
// reboot — they are flash targets already.
inline bool reflashUnitNeedsReboot(const UnitFacts& u) {
  return u.state == 1 && (u.fwStatus != 0 || reflashUnitProtocolMismatch(u));
}

inline int reflashCollectRebootTargets(const UnitFacts* facts, int maxUnits,
                                       int base, uint8_t* outAddrs) {
  int n = 0;
  for (int i = 0; i < maxUnits; i++) {
    if (reflashUnitNeedsReboot(facts[i])) outAddrs[n++] = (uint8_t)(base + i);
  }
  return n;
}

// Boot auto-update predicate (v1 semantics, deliberately narrower than
// reflashUnitNeedsReboot): only units PROVABLY not on our build are
// force-rebooted at boot — an unreadable rev must not trigger a reflash cycle
// every power-up. The operator's web job sweeps unknowns too (v1 #114).
//
// A protocol mismatch qualifies as proof: the read SUCCEEDED and reported a
// contract that is not ours, which is a definite fact about the unit rather
// than an absence of information. An unreadable version stays excluded.
inline int reflashCollectOutdatedTargets(const UnitFacts* facts, int maxUnits,
                                         int base, uint8_t* outAddrs) {
  int n = 0;
  for (int i = 0; i < maxUnits; i++) {
    if (facts[i].state == 1 &&
        (facts[i].fwStatus == 1 || reflashUnitProtocolMismatch(facts[i]))) {
      outAddrs[n++] = (uint8_t)(base + i);
    }
  }
  return n;
}

inline int reflashCollectFlashTargets(const UnitFacts* facts, int maxUnits,
                                      int base, uint8_t* outAddrs) {
  int n = 0;
  for (int i = 0; i < maxUnits; i++) {
    if (facts[i].state == 2) outAddrs[n++] = (uint8_t)(base + i);
  }
  return n;
}

// Narrow any collected target list to a single address; 0 means "no filter"
// and returns the list untouched (0 is the general-call address, never a
// unit's, so it is free to use as the sentinel).
//
// A filter rather than three extra parameters: all three collectors above
// answer "who matches this predicate", and "…and is this one unit" is a
// separate question that composes with each of them identically. It is
// applied to BOTH the reboot sweep and the post-rescan flash list, so a unit
// stranded in twiboot by an earlier attempt is not swept up by a run aimed at
// a different address.
//
// Exists for the #407 campaign (#412): a day-0 EEPROM erase on a wire contract
// that has never run on hardware is not something to hand a 21-unit sweep. The
// operator flashes one, inspects it, and decides.
inline int reflashFilterToAddress(uint8_t* addrs, int n, uint8_t onlyAddr) {
  if (onlyAddr == 0) return n;
  for (int i = 0; i < n; i++) {
    if (addrs[i] == onlyAddr) {
      addrs[0] = onlyAddr;
      return 1;
    }
  }
  return 0;
}

// --- progress (published in the DisplaySnapshot, rendered on the web) ---------

enum class ReflashState : uint8_t {
  Idle = 0,   // no job since boot (snapshot default)
  Entering,   // enter-bootloader sweep + twiboot settle + rescan
  Flashing,   // streaming pages to currentAddr
  Settling,   // waiting for a flashed batch to come back online + home
  Done,       // finished, every planned unit flashed
  Cancelled,  // aborted via /stop — in-flight unit left in twiboot
  Failed,     // finished with per-unit failures (failed > 0)
};

struct ReflashProgress {
  ReflashState state = ReflashState::Idle;
  uint8_t total = 0;        // planned flash targets
  uint8_t done = 0;         // flashed + verified
  uint8_t failed = 0;       // left in twiboot for the next attempt
  uint8_t currentAddr = 0;  // unit being flashed (0 outside Flashing)
};

// The producer gate (#205 design rule) keys off this: while true, only Stop
// may enter the display queue.
inline bool reflashInProgress(const ReflashProgress& p) {
  return p.state == ReflashState::Entering ||
         p.state == ReflashState::Flashing ||
         p.state == ReflashState::Settling;
}

inline void reflashProgressBegin(ReflashProgress& p, int total) {
  p.state = ReflashState::Entering;
  p.total = (uint8_t)total;
  p.done = 0;
  p.failed = 0;
  p.currentAddr = 0;
}

inline void reflashProgressUnitStart(ReflashProgress& p, uint8_t addr) {
  p.state = ReflashState::Flashing;
  p.currentAddr = addr;
}

inline void reflashProgressUnitResult(ReflashProgress& p, bool ok) {
  if (ok) p.done++; else p.failed++;
}

inline void reflashProgressSettling(ReflashProgress& p) {
  p.state = ReflashState::Settling;
  p.currentAddr = 0;
}

// Cancel wins over per-unit failures: the operator pulled the plug, so the
// counters describe an interrupted job, not a graded one.
inline void reflashProgressFinish(ReflashProgress& p, bool cancelled) {
  p.currentAddr = 0;
  if (cancelled) {
    p.state = ReflashState::Cancelled;
  } else if (p.failed > 0) {
    p.state = ReflashState::Failed;
  } else {
    p.state = ReflashState::Done;
  }
}

inline const char* reflashStateName(ReflashState s) {
  switch (s) {
    case ReflashState::Entering:  return "entering";
    case ReflashState::Flashing:  return "flashing";
    case ReflashState::Settling:  return "settling";
    case ReflashState::Done:      return "done";
    case ReflashState::Cancelled: return "cancelled";
    case ReflashState::Failed:    return "failed";
    default:                      return "idle";
  }
}

// Job-level grade for the /unit/op-result contract: ok only when the job
// ran to completion with zero failures; the progress JSON carries the
// detail (state + counters) for everything else.
inline MaintOutcome classifyReflashOutcome(const ReflashProgress& p,
                                           MaintReason& reason) {
  reason = MaintReason::None;
  if (p.state == ReflashState::Done) return MaintOutcome::Ok;
  return MaintOutcome::PostconditionFail;
}
