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
// post-flash homing current on a supply shared with the steppers.
#define REFLASH_BATCH_SIZE 2
#define REFLASH_BATCH_SETTLE_MS 15000UL
// Wait after CMD_ENTER_BOOTLOADER before talking to twiboot: watchdog reset
// (~15 ms) + twiboot init. 500 ms is generous (v1 value).
#define TWIBOOT_STARTUP_MS 500

// A sketch-mode unit whose rev is not the bundled one gets pushed into
// twiboot. UNKNOWN qualifies alongside OUTDATED (v1 #114: only units
// provably on the bundled rev are skipped). Bootloader units need no
// reboot — they are flash targets already.
inline bool reflashUnitNeedsReboot(const UnitFacts& u) {
  return u.state == 1 && u.fwStatus != 0;
}

inline int reflashCollectRebootTargets(const UnitFacts* facts, int maxUnits,
                                       int base, uint8_t* outAddrs) {
  int n = 0;
  for (int i = 0; i < maxUnits; i++) {
    if (reflashUnitNeedsReboot(facts[i])) outAddrs[n++] = (uint8_t)(base + i);
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
