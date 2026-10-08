#pragma once

// ReflashPlan.h — pure planning + progress core of the unit reflash job
// (#205, slice C of the I2C port). Who gets flashed, when a unit is sent into
// its bootloader, the per-unit progress a row master publishes, and the
// job-level op grading. No Wire, no RTOS — natively tested by
// test_reflash_plan (Master) and test_follower_ops (FollowerEsp01). The
// hardware execution lives in each row master's bus file.

#include <string.h>

#include "MaintenancePolicy.h"
#include "UnitHealth.h"
#include "UnitTimings.h"  // TWIBOOT_STARTUP_MS

// v1 #138 brownout throttle: flash at most this many units per batch, then
// wait for the batch to come back online + finish homing before the next —
// post-flash homing current on a supply shared with the steppers. The size is
// bench-tuned per supply, so each row master sets it in its platformio.ini.
#ifndef REFLASH_BATCH_SIZE
#error "REFLASH_BATCH_SIZE is per row master: set it in the tree's platformio.ini"
#endif
#define REFLASH_BATCH_SETTLE_MS 15000UL
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

// The job's flash list, in address order: the sweep's sketch units and
// whoever already sits in twiboot. Nobody is sent anywhere here — a unit
// enters its bootloader when its own turn comes (reflashEnterUnit).
inline int reflashPlanTargets(const UnitFacts* facts, int maxUnits, int base,
                              const uint8_t* sweep, int sweepCount,
                              uint8_t* outAddrs) {
  int n = 0;
  for (int i = 0; i < maxUnits; i++) {
    uint8_t addr = (uint8_t)(base + i);
    bool planned = facts[i].state == 2;
    for (int k = 0; k < sweepCount && !planned; k++) planned = sweep[k] == addr;
    if (planned) outAddrs[n++] = addr;
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
// applied to the planned flash list, so a unit stranded in twiboot by an
// earlier attempt is not swept up by a run aimed at a different address.
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

// /reflash-units?address=N&force=1: the named unit is sent into its bootloader
// whatever revision it reports — an image can be damaged under a version
// string that still matches. One unit by construction: without an address it
// plans nothing, so a forced job can never turn into a whole-row reflash. A
// unit already in twiboot is a flash target without this, and an absent one
// stays absent.
inline int reflashCollectForcedTarget(const UnitFacts* facts, int maxUnits,
                                      int base, uint8_t onlyAddr,
                                      uint8_t* outAddrs) {
  if (onlyAddr < base || onlyAddr >= base + maxUnits) return 0;
  if (facts[onlyAddr - base].state != 1) return 0;
  outAddrs[0] = onlyAddr;
  return 1;
}

// The `force` query value: 1/true or 0/false, nothing else. It decides
// whether a healthy unit is erased, so a value that is not understood is
// refused rather than read as either.
inline bool reflashParseForce(const char* raw, bool& out) {
  if (raw == nullptr) return false;
  if (strcmp(raw, "1") == 0 || strcmp(raw, "true") == 0) {
    out = true;
    return true;
  }
  if (strcmp(raw, "0") == 0 || strcmp(raw, "false") == 0) {
    out = false;
    return true;
  }
  return false;
}

// /reflash-units?address=N bound: the managed range only. Deliberately not
// maintValidateAddress — a unit in twiboot or on a protocol we do not speak
// must still be reflashable; converging it is the point. A target that is
// current, silent or absent plans nothing: the op still reports ok and the
// progress object's total is 0, which is what a caller must check (what
// commission-units.sh reads).
inline bool reflashAddressInRange(long addr, int base, int maxUnits) {
  return addr >= base && addr < (long)base + maxUnits;
}

// Decimal digits only, 1..3 of them. This picks which unit gets erased, so no
// base guessing ("010" is not 8) and no trailing garbage ("3abc" is not 3).
inline bool reflashParseAddress(const char* raw, long& out) {
  if (raw == nullptr || raw[0] == '\0') return false;
  long v = 0;
  int n = 0;
  for (; raw[n] != '\0'; n++) {
    if (raw[n] < '0' || raw[n] > '9' || n >= 3) return false;
    v = v * 10 + (raw[n] - '0');
  }
  if (n > 1 && raw[0] == '0') return false;
  out = v;
  return true;
}

// --- progress (published by the row master, rendered on the web) --------------

enum class ReflashState : uint8_t {
  Idle = 0,   // no job since boot (snapshot default)
  Entering,   // waiting for the row to stand still before the first unit
  Flashing,   // streaming pages to currentAddr
  Settling,   // waiting for a flashed batch to come back online + home
  BootUpdate, // bringing boot sections to the current image (UnitUpdateJob.h)
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
  // #412: the run stopped itself on consecutive failures rather than reaching
  // the end of its plan. Distinct from `failed > 0`, which a completed run also
  // shows — an operator reading /units/health cannot otherwise tell "the image
  // is suspect and the remaining units were never touched" from "this job
  // finished and these are the real failures", and giving a human that signal
  // is the entire point of the halt.
  bool halted = false;
  // Boot sections brought to the current image by this job, and the ones
  // that refused or failed (UnitUpdateJob.h). Units already current count in
  // neither.
  uint8_t bootDone = 0;
  uint8_t bootFailed = 0;
};

// The producer gate (#205 design rule) keys off this: while true, only Stop
// may enter the display queue.
inline bool reflashInProgress(const ReflashProgress& p) {
  return p.state == ReflashState::Entering ||
         p.state == ReflashState::Flashing ||
         p.state == ReflashState::Settling ||
         p.state == ReflashState::BootUpdate;
}

inline void reflashProgressBegin(ReflashProgress& p, int total) {
  p.state = ReflashState::Entering;
  p.total = (uint8_t)total;
  p.done = 0;
  p.failed = 0;
  p.currentAddr = 0;
  p.halted = false;
  p.bootDone = 0;
  p.bootFailed = 0;
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
inline void reflashProgressFinish(ReflashProgress& p, bool cancelled,
                                  bool halted) {
  p.currentAddr = 0;
  // A halt always implies failures (it takes REFLASH_MAX_CONSECUTIVE_FAILURES
  // to trigger), so the state below is already Failed — this flag adds the WHY,
  // not the severity. A cancel outranks it: the operator pulled the plug.
  p.halted = halted && !cancelled;
  if (cancelled) {
    p.state = ReflashState::Cancelled;
  } else if (p.failed > 0 || p.bootFailed > 0) {
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
    case ReflashState::BootUpdate: return "bootloader";
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

// --- the flash loop ---------------------------------------------------------------

enum class ReflashUnitOutcome : uint8_t {
  Flashed = 0,
  Failed,   // this unit did not take the image; it stays in twiboot
  Stopped,  // the flash was aborted mid-unit — the run ends here
  NotEntered,  // the unit is not in its bootloader: nothing was sent to it
};

struct ReflashRunEnd {
  bool cancelled = false;  // the tree asked to stop
  bool halted = false;     // consecutive failures stopped the run (#412)
  uint8_t flashed = 0;
  uint8_t notEntered = 0;
};

// Gets one unit into twiboot for its own pages. A unit waits there only for
// its own flash: the bootloader goes back to the unit's firmware once it has
// not been addressed for SF_PIN_TIMEOUT_MS (UnitBootloader/main.c), which is
// shorter than the flash of a row. A unit already there gets no order. Both
// answers are asked of the unit, never read from the facts, so pages are
// never sent to a unit that is running its firmware.
//
// A unit that took the order may reach its reset late, so it is asked a few
// times. The last question still falls inside the bootloader's start window
// (TIMEOUT_MS, 1000 ms from the reset) of a unit that entered on time: asked
// later, such a unit would be back in its firmware and read as never entered.
#define REFLASH_ENTER_PROBES 4
#define REFLASH_ENTER_PROBE_GAP_MS 100UL
template <typename Hooks>
inline bool reflashEnterUnit(Hooks& h, uint8_t addr) {
  if (h.inBootloader(addr)) return true;
  if (h.enterBootloader(addr) != 0) return false;
  h.pause(TWIBOOT_STARTUP_MS);
  for (int attempt = 1;; attempt++) {
    if (h.inBootloader(addr)) return true;
    if (attempt >= REFLASH_ENTER_PROBES) return false;
    h.pause(REFLASH_ENTER_PROBE_GAP_MS);
  }
}

// Flashes the planned targets in batches and keeps the progress object
// current. v1 #138 brownout throttle: once REFLASH_BATCH_SIZE units have been
// flashed, wait for them to come back online and finish homing before
// flashing more — post-flash homing current shares a supply with the
// steppers. Two failures back to back end the run (#412); one success in
// between resets the count. A unit that is not in its bootloader when its
// turn comes is not flashed and counts as failed for the job, but says
// nothing about the image: it neither adds to that count nor resets it.
//
// The trailing settle runs on EVERY exit: it is brownout pacing and is never
// shortened, so even a cancelled or halted run waits out the homing of the
// units it already flashed before the row is handed back.
//
//   bool stopRequested()                        checked before each unit
//   bool imageFits()                            the image leaves twiboot alone
//   bool inBootloader(uint8_t addr)             asks the unit
//   int enterBootloader(uint8_t addr)           0 = the unit took the order
//   void pause(uint32_t ms)
//   void unitNotEntered(uint8_t addr)           worth a log line
//   ReflashUnitOutcome flashUnit(uint8_t addr)
//   void unitFlashed(uint8_t addr)              the unit runs its sketch again
//   void progressChanged()                      publish the progress object
//   void settleBatch(const uint8_t* addrs, int n)
//   void runHalted(uint8_t consecutiveFailures, int unitsLeftUntouched)
template <typename Hooks>
inline ReflashRunEnd reflashRunTargets(Hooks& h, const uint8_t* targets,
                                       int total, ReflashProgress& progress) {
  ReflashRunEnd end;
  uint8_t consecutiveFailures = 0;
  uint8_t batch[REFLASH_BATCH_SIZE];
  int inBatch = 0;
  for (int k = 0; k < total; k++) {
    if (h.stopRequested()) {
      end.cancelled = true;
      break;
    }
    uint8_t addr = targets[k];
    reflashProgressUnitStart(progress, addr);
    h.progressChanged();

    // An image that cannot be flashed costs no unit a trip through its
    // bootloader: flashUnit refuses it before anything is sent.
    ReflashUnitOutcome outcome =
        (h.imageFits() && !reflashEnterUnit(h, addr))
            ? ReflashUnitOutcome::NotEntered
            : h.flashUnit(addr);
    bool ok = outcome == ReflashUnitOutcome::Flashed;
    reflashProgressUnitResult(progress, ok);
    if (outcome == ReflashUnitOutcome::Stopped) {
      h.progressChanged();
      end.cancelled = true;
      break;
    }
    if (ok) {
      consecutiveFailures = 0;  // an isolated dead unit must not wedge a sweep
      h.unitFlashed(addr);
      batch[inBatch++] = addr;
      end.flashed++;
    } else if (outcome == ReflashUnitOutcome::NotEntered) {
      h.unitNotEntered(addr);
      end.notEntered++;
    } else if (consecutiveFailures < 0xFF) {
      consecutiveFailures++;
    }
    h.progressChanged();

    if (inBatch >= REFLASH_BATCH_SIZE) {
      reflashProgressSettling(progress);
      h.progressChanged();
      h.settleBatch(batch, inBatch);
      inBatch = 0;
    }
    if (reflashShouldHalt(consecutiveFailures)) {
      h.runHalted(consecutiveFailures, total - (k + 1));
      end.halted = true;
      break;
    }
  }
  if (inBatch > 0) {
    reflashProgressSettling(progress);
    h.progressChanged();
    h.settleBatch(batch, inBatch);
  }
  return end;
}

// Renders the reflash progress JSON (#205) — spliced into the /units/health
// payload by the web layer (additive key). REFLASH_JSON_CAP holds the widest
// object the fields can produce.
#define REFLASH_JSON_CAP 128
inline void buildReflashJson(char* buf, size_t cap,
                             const ReflashProgress& p) {
  snprintf(buf, cap,
           "{\"state\":\"%s\",\"total\":%u,\"done\":%u,\"failed\":%u,"
           "\"cur\":%u,\"halted\":%s,\"boot\":%u,\"bootFailed\":%u}",
           reflashStateName(p.state), (unsigned)p.total, (unsigned)p.done,
           (unsigned)p.failed, (unsigned)p.currentAddr,
           p.halted ? "true" : "false", (unsigned)p.bootDone,
           (unsigned)p.bootFailed);
}
