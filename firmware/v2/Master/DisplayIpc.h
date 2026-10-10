#pragma once
// DisplayIpc.h — the display task's published state (#187, #203), pure logic.
//
// DisplaySnapshot is single-writer: only the display task mutates it, and
// consumers get a mutex-guarded copy via displaySnapshotGet() (Tasks.cpp) —
// web handlers build JSON from their copy, never from live state. POD for
// the same reason as DisplayCommand: copyable without heap or locks held
// long. The snapshot carries per-unit FACTS (UnitFacts, ~350 B total), not
// prebuilt JSON — v1 cached the health JSON in RAM as an ESP-01 memory
// tactic; here the web layer renders from its copy via buildUnitHealthJson()
// (cleaner ownership, no stale-cache invalidation).
//
// displayApplyCommand() is the pure per-command transition;
// displayApplyUnitFacts() folds the bus's probe/health results in and
// derives width + counts. The hardware side effects live in UnitBus.cpp,
// called from displayTask only.

#include "DisplayCommand.h"
#include "DisplayWidth.h"
#include "MaintenancePolicy.h"
#include "ReflashPlan.h"
#include "UnitHealth.h"
#include "BootDump.h"      // BootDumpSlot (#511)
#include "BootInfo.h"      // BootInfoSlot (#499)

// Execution result of the LAST maintenance op (#204) — the job result
// contract. A single slot, not a log: it is a best-effort acknowledgement
// channel for one active critical op (the UI serializes those and disables
// the Maintenance controls while awaiting); an older seq answering
// "expired" is the designed overwrite behavior, not data loss.
struct MaintResult {
  uint32_t seq = 0;  // 0 = nothing executed yet (real seqs start at 1)
  DisplayOpcode opcode = DisplayOpcode::None;
  uint8_t addr = 0;
  MaintOutcome outcome = MaintOutcome::Pending;
  MaintReason reason = MaintReason::None;
};

struct DisplaySnapshot {
  // v1 probe-fallback parity: until a probe answers, assume the ceiling.
  uint8_t displayWidth = UNITS_AMOUNT;
  // The unit facts below are a bus scan's, not the defaults of a display
  // task that has not looked yet (an unscanned row reads as sixteen empty
  // places).
  bool probed = false;
  bool busy = false;
  uint32_t commandsProcessed = 0;
  char currentText[DISPLAY_CMD_TEXT_LEN + 1] = {0};
  // Failed unit writes on the most recent frame show (v1's
  // lastShowUnitWriteErrors) — an MQTT telemetry input (#224).
  uint8_t lastShowWriteErrors = 0;
  // Probe/health facts (#203). Derived fields are recomputed by
  // displayApplyUnitFacts(), never patched individually.
  uint8_t detectedUnitCount = 0;
  UnitFacts units[UNITS_AMOUNT];
  MaintResult lastMaint;
  // Reflash job progress (#205) — published at unit boundaries and settle
  // transitions while the job runs; the producer gate keys off it.
  ReflashProgress reflash;
  SelfTestSlot lastSelfTest;  // single-slot self-test result (#265)
  BootDumpSlot lastBootDump;  // single-slot boot-section dump result (#511)
  BootInfoSlot lastBootInfo;  // single-slot read-only boot report (#499)
  // The letter indices of the last frame the master actually put on the
  // bus (#264) — the "intended" side of the displayed==intended check.
  // Valid after the first ShowText/Stop/ResetUnits; maintained by
  // displayTask at every frame-shaping site.
  uint8_t lastFrameLetters[UNITS_AMOUNT] = {0};
  bool lastFrameValid = false;
  // When that frame went out, and the units not read since (bit = slot): one
  // unit is read per heartbeat, so the others' facts are older than the frame
  // for a while and say nothing about it.
  uint32_t lastFrameAtMs = 0;
  uint32_t frameUnreadMask = 0;
};

static_assert(UNITS_AMOUNT <= 32, "frameUnreadMask holds one bit a unit");

// Records the frame that was just put on the bus as what the wall should show.
// Every frame-shaping site ends here. A unit whose flap the frame changes
// loses its wrong-letter verdict until it is read again; the others keep
// theirs. `nowMs` is on the clock UnitFacts::lastSeenMs counts on.
inline void displayApplyFrame(DisplaySnapshot& snap, const uint8_t* letters,
                              uint32_t nowMs) {
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    if (!snap.lastFrameValid || snap.lastFrameLetters[i] != letters[i]) {
      snap.units[i].mismatch = false;
    }
    snap.lastFrameLetters[i] = letters[i];
  }
  snap.lastFrameValid = true;
  snap.lastFrameAtMs = nowMs;
  snap.frameUnreadMask = 0xFFFFFFFFUL;
}

// The same frame was sent again because a job or a restart took drums off
// their flap: what was read of any unit before this says nothing any more.
inline void displayApplyReshow(DisplaySnapshot& snap, uint32_t nowMs) {
  for (int i = 0; i < UNITS_AMOUNT; i++) snap.units[i].mismatch = false;
  snap.lastFrameAtMs = nowMs;
  snap.frameUnreadMask = 0xFFFFFFFFUL;
}

// The producer gate (#205): while a reflash job runs, Stop is the ONLY
// command allowed into the display queue — it is the cancel. Everything
// else answers 409 at the web/MQTT boundary, and clockTask skips its tick.
inline bool displayAcceptsCommand(const DisplaySnapshot& snap,
                                  DisplayOpcode op) {
  if (!reflashInProgress(snap.reflash)) return true;
  return op == DisplayOpcode::Stop;
}

// Applies one command's state effects to the snapshot. Returns false (no
// mutation) for commands the worker can't execute. Probe only counts here —
// its facts arrive through displayApplyUnitFacts() after the bus scan.
inline bool displayApplyCommand(DisplaySnapshot& snap,
                                const DisplayCommand& cmd) {
  switch (cmd.opcode) {
    case DisplayOpcode::ShowText:
      memcpy(snap.currentText, cmd.text, sizeof(snap.currentText));
      snap.commandsProcessed++;
      return true;
    case DisplayOpcode::Stop:
      // v1 parity: clearing the retained text makes the clock/event loop
      // re-send fresh content instead of dedup-suppressing forever.
      snap.currentText[0] = '\0';
      snap.commandsProcessed++;
      return true;
    case DisplayOpcode::Probe:
    // Maintenance ops (#204) only count here: their effects arrive at
    // execution time via displayApplyOffsetWrite / displayApplyUnitFacts /
    // displayApplyMaintResult. ResetUnits re-shows its baked enqueue-time
    // text, which equals currentText by construction — nothing to patch.
    case DisplayOpcode::WriteOffset:
    case DisplayOpcode::Jog:
    case DisplayOpcode::Home:
    case DisplayOpcode::RebootToBootloader:
    case DisplayOpcode::Identify:
    case DisplayOpcode::SetAddress:
    case DisplayOpcode::ClearAddress:
    case DisplayOpcode::ResetOdometer:
    case DisplayOpcode::SelfTest:
    case DisplayOpcode::BootDump:
    case DisplayOpcode::BootUpdate:
    case DisplayOpcode::BootInfo:
    case DisplayOpcode::SetGates:
    case DisplayOpcode::ResetUnits:
    case DisplayOpcode::ReflashUnits:
      snap.commandsProcessed++;
      return true;
    default:
      return false;
  }
}

// Folds a bus scan's per-unit facts into the snapshot and recomputes the
// derived fields: width (highest responder + 1, ceiling fallback — #123
// rules in DisplayWidth.h), responding-unit count.
// widthOverride (#289 dummy mode): 1..maxUnits pins the width regardless of
// the probe (0/out-of-range = probe-derived); counts stay probe truth.
inline void displayApplyUnitFacts(DisplaySnapshot& snap,
                                  const UnitFacts* facts, int maxUnits,
                                  int widthOverride = 0) {
  // Clamp once: every fixed-size array below is UNITS_AMOUNT-bounded, so a
  // larger caller value must never reach the derive calls either.
  if (maxUnits > UNITS_AMOUNT) maxUnits = UNITS_AMOUNT;
  int states[UNITS_AMOUNT];
  for (int i = 0; i < maxUnits; i++) {
    const bool verdictBefore = snap.units[i].mismatch;
    snap.units[i] = facts[i];
    states[i] = facts[i].state;
    // displayed==intended verdict (#264), stamped HERE and only here (#267):
    // a render-time comparison would race newer frames against stale phys.
    // The fold takes every unit, but a heartbeat reads one: only a unit read
    // after the frame went out is judged against it (#582), the others keep
    // the verdict they had. No verdict against a rotating drum
    // (self-resolving by definition) or before any frame exists.
    UnitFacts& u = snap.units[i];
    const uint32_t bit = 1UL << i;
    // lastSeenMs 0 = never read: not a read after anything.
    if ((snap.frameUnreadMask & bit) && u.lastSeenMs != 0 &&
        (int32_t)(u.lastSeenMs - snap.lastFrameAtMs) > 0) {
      snap.frameUnreadMask &= ~bit;
    }
    if (snap.frameUnreadMask & bit) {
      u.mismatch = verdictBefore;
      continue;
    }
    bool physKnown = u.diagValid &&
                     (u.driftFlags & UNIT_DRIFT_FLAG_POSITION_KNOWN) &&
                     u.physLetter != 0xFF;
    bool moving = u.statusValid && (u.status.flags & UNIT_FLAG_MOVING);
    u.mismatch = physKnown && !moving && snap.lastFrameValid &&
                 u.physLetter != snap.lastFrameLetters[i];
  }
  int width = computeDisplayWidth(states, maxUnits);
  if (widthOverride >= 1 && widthOverride <= maxUnits) {
    width = widthOverride;
  }
  snap.displayWidth = (uint8_t)width;
  snap.detectedUnitCount = (uint8_t)countRespondingUnits(states, maxUnits);
  snap.probed = true;
}

// --- maintenance results (#204) --------------------------------------------------

// Publishes a maintenance op's execution outcome into the single result
// slot. Called by displayTask after the bus work (and its reprobe, for the
// compound address ops) finished.
inline void displayApplyMaintResult(DisplaySnapshot& snap,
                                    const DisplayCommand& cmd,
                                    MaintOutcome outcome, MaintReason reason) {
  snap.lastMaint.seq = cmd.seq;
  snap.lastMaint.opcode = cmd.opcode;
  snap.lastMaint.addr = cmd.unitAddress;
  snap.lastMaint.outcome = outcome;
  snap.lastMaint.reason = reason;
}

inline void displayApplyMaintResult(DisplaySnapshot& snap,
                                    const DisplayCommand& cmd,
                                    MaintGrade grade) {
  displayApplyMaintResult(snap, cmd, grade.outcome, grade.reason);
}

// Publishes a self-test op's outcome + measurements into its single result
// slot (#265) — same overwrite contract as displayApplyMaintResult.
inline void displayApplySelfTestResult(DisplaySnapshot& snap,
                                       const SelfTestSlot& slot) {
  snap.lastSelfTest = slot;
}

// Same overwrite contract for the boot-section dump (#511).
inline void displayApplyBootDumpResult(DisplaySnapshot& snap,
                                       const BootDumpSlot& slot) {
  snap.lastBootDump = slot;
}

// Same overwrite contract for the read-only boot report (#499).
inline void displayApplyBootInfoResult(DisplaySnapshot& snap,
                                       const BootInfoSlot& slot) {
  snap.lastBootInfo = slot;
}

// A successful SET_OFFSET is the only in-place offset mutation; everything
// else flows through a probe's wholesale fact rewrite. The bus facts take it
// too: every health poll copies them over the snapshot's.
inline void displayApplyOffsetWrite(DisplaySnapshot& snap, UnitFacts* busFacts,
                                    int i2cAddress, int16_t value) {
  int idx = i2cAddress - SFP_I2C_ADDRESS_BASE;
  if (idx < 0 || idx >= UNITS_AMOUNT) return;
  unitFactsApplyOffsetWrite(busFacts[idx], value);
  unitFactsApplyOffsetWrite(snap.units[idx], value);
}

// A successful RESET_ODOMETER zeroes the unit's count; patch the fact in
// place like the offset write so the wear view doesn't show the stale
// count until the next probe (#231).
inline void displayApplyOdometerReset(DisplaySnapshot& snap, UnitFacts* busFacts,
                                     int i2cAddress) {
  int idx = i2cAddress - SFP_I2C_ADDRESS_BASE;
  if (idx < 0 || idx >= UNITS_AMOUNT) return;
  unitFactsApplyOdometerReset(busFacts[idx]);
  unitFactsApplyOdometerReset(snap.units[idx]);
}

// A verified SET_GATES landed (#409) — patch the fact so the unit facts shows
// the new gates immediately instead of the pre-write value until the next
// lifetime poll. Only ever called after the read-back confirmed it, so this
// cannot invent a gate the unit did not accept.
inline void displayApplyGatesWrite(DisplaySnapshot& snap, int i2cAddress,
                                   uint8_t gates) {
  int idx = i2cAddress - SFP_I2C_ADDRESS_BASE;
  if (idx < 0 || idx >= UNITS_AMOUNT) return;
  unitFactsApplyGatesWrite(snap.units[idx], gates);
}

// A unit sent into twiboot forgets nothing, but the master must stop
// serving reads for it until the next probe confirms it is back in sketch.
inline void displayInvalidateUnitReads(DisplaySnapshot& snap, int i2cAddress) {
  int idx = i2cAddress - SFP_I2C_ADDRESS_BASE;
  if (idx < 0 || idx >= UNITS_AMOUNT) return;
  unitFactsInvalidateReads(snap.units[idx]);
}
