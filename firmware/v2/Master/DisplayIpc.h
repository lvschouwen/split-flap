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
#include "UnitSelfTest.h"  // SELFTEST_REASON_* + selfTestReasonName (#404)

// Execution result of the LAST maintenance op (#204) — the /unit/op-result
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

// Execution result of the LAST self-test op (#265) — same single-slot,
// best-effort contract as MaintResult (the UI serializes self-tests and
// polls /unit/self-test-result). Carries the unit's three measurements on
// success.
enum class SelfTestOutcome : uint8_t {
  Pending = 0,  // slot default; a real result always overwrites it
  Ok,
  WireFail,     // the START write was not ACKed
  Timeout,      // unit never reported done within the master's window
  UnitFailed,   // unit reported FAILED (hall/marker problem mid-test)
  Unsupported,  // every poll answered garbage — firmware predates #265
  Aborted,      // /stop arrived while waiting
};

struct SelfTestSlot {
  uint32_t seq = 0;
  uint8_t addr = 0;
  SelfTestOutcome outcome = SelfTestOutcome::Pending;
  uint16_t stepsPerRev = 0;
  uint16_t hallWindowSteps = 0;
  uint16_t revTimeMs = 0;
  // Which of the unit's three failure modes fired (#404), SELFTEST_REASON_*.
  // `outcome` above is the MASTER's view (wire-fail, timeout, unit-failed);
  // this is the unit's own account of why, which is what tells a person
  // whether to reseat a magnet, replace a sensor, or free a binding drum.
  uint8_t unitReason = SELFTEST_REASON_NONE;
};

struct DisplaySnapshot {
  // v1 probe-fallback parity: until a probe answers, assume the ceiling.
  uint8_t displayWidth = UNITS_AMOUNT;
  bool busy = false;
  uint32_t commandsProcessed = 0;
  char currentText[DISPLAY_CMD_TEXT_LEN + 1] = {0};
  // Who put currentText there and when (#403). Only ShowText and Stop move
  // these: the re-show opcodes restore text this snapshot already attributes,
  // so they must leave the attribution standing. sourceAtMs is a millis()
  // stamp — the console renders it as an age, never as a wall-clock time.
  DisplaySource source = DisplaySource::Unknown;
  uint32_t sourceAtMs = 0;
  // Failed unit writes on the most recent frame show (v1's
  // lastShowUnitWriteErrors) — an MQTT telemetry input (#224).
  uint8_t lastShowWriteErrors = 0;
  // Probe/health facts (#203). Derived fields are recomputed by
  // displayApplyUnitFacts(), never patched individually.
  uint8_t detectedUnitCount = 0;
  uint8_t faultyUnitCount = 0;
  // #329 headless mode: latched by displayTask's HeadlessDetector after N
  // consecutive 0-unit probes — NOT derived by displayApplyUnitFacts (a
  // single probe's 0 must not flip a real display). Drives /settings'
  // headlessSuggested nudge.
  bool headlessUnitless = false;
  UnitFacts units[UNITS_AMOUNT];
  MaintResult lastMaint;
  // Reflash job progress (#205) — published at unit boundaries and settle
  // transitions while the job runs; the producer gate keys off it.
  ReflashProgress reflash;
  SelfTestSlot lastSelfTest;  // single-slot self-test result (#265)
  // The letter indices of the last frame the master actually put on the
  // bus (#264) — the "intended" side of the displayed==intended check.
  // Valid after the first ShowText/Stop/ResetUnits; maintained by
  // displayTask at every frame-shaping site.
  uint8_t lastFrameLetters[UNITS_AMOUNT] = {0};
  bool lastFrameValid = false;
};

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
                                const DisplayCommand& cmd,
                                uint32_t nowMs = 0) {
  switch (cmd.opcode) {
    case DisplayOpcode::ShowText:
      memcpy(snap.currentText, cmd.text, sizeof(snap.currentText));
      snap.source = cmd.source;
      snap.sourceAtMs = nowMs;
      snap.commandsProcessed++;
      return true;
    case DisplayOpcode::Stop:
      // v1 parity: clearing the retained text makes the clock/event loop
      // re-send fresh content instead of dedup-suppressing forever.
      snap.currentText[0] = '\0';
      // Nothing is on the wall, so nothing drove it (#403). The console says
      // "Nothing" rather than naming whoever blanked it — the field answers
      // "what put this text here", and there is no text.
      snap.source = DisplaySource::Unknown;
      snap.sourceAtMs = nowMs;
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
// rules in DisplayWidth.h), responding-unit count, faulty count.
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
    snap.units[i] = facts[i];
    states[i] = facts[i].state;
    // displayed==intended verdict (#264), stamped HERE and only here (#267):
    // this fold runs right after a diag poll, in displayTask — the polled
    // phys and lastFrameLetters describe the same instant (frames and polls
    // are serialized). A render-time comparison would race newer frames
    // against stale phys and flag phantom mismatches. No verdict against a
    // rotating drum (self-resolving by definition) or
    // before any frame exists.
    UnitFacts& u = snap.units[i];
    bool physKnown = u.diagValid &&
                     (u.driftFlags & UNIT_DRIFT_FLAG_POSITION_KNOWN) &&
                     u.physLetter != 0xFF;
    bool moving = u.statusValid && (u.status.flags & UNIT_FLAG_MOVING);
    u.mismatch = physKnown && !moving && snap.lastFrameValid &&
                 u.physLetter != snap.lastFrameLetters[i];
  }
  int width = computeDisplayWidth(states, maxUnits);
  if (widthOverride == -1) {
    // #331 headless: deviceRole=headless-* forces displayWidth 0 — the board
    // renders nothing and reports no phantom row, overriding both the probe
    // ceiling fallback and the #289 unit-count override.
    width = 0;
  } else if (widthOverride >= 1 && widthOverride <= maxUnits) {
    width = widthOverride;
  }
  snap.displayWidth = (uint8_t)width;
  snap.detectedUnitCount = (uint8_t)countRespondingUnits(states, maxUnits);
  snap.faultyUnitCount = (uint8_t)computeFaultyUnitCount(snap.units, maxUnits);
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

// Publishes a self-test op's outcome + measurements into its single result
// slot (#265) — same overwrite contract as displayApplyMaintResult.
inline void displayApplySelfTestResult(DisplaySnapshot& snap,
                                       const SelfTestSlot& slot) {
  snap.lastSelfTest = slot;
}

// A successful SET_OFFSET is the only in-place offset mutation; everything
// else flows through a probe's wholesale fact rewrite.
inline void displayApplyOffsetWrite(DisplaySnapshot& snap, int i2cAddress,
                                    int16_t value) {
  int idx = i2cAddress - SFP_I2C_ADDRESS_BASE;
  if (idx < 0 || idx >= UNITS_AMOUNT) return;
  snap.units[idx].offset = value;
  snap.units[idx].offsetValid = true;
}

// A successful RESET_ODOMETER zeroes the unit's count; patch the fact in
// place like the offset write so the wear view doesn't show the stale
// count until the next probe (#231).
inline void displayApplyOdometerReset(DisplaySnapshot& snap, int i2cAddress) {
  int idx = i2cAddress - SFP_I2C_ADDRESS_BASE;
  if (idx < 0 || idx >= UNITS_AMOUNT) return;
  snap.units[idx].odometer = 0;
  snap.units[idx].odometerValid = true;
}

// A verified SET_GATES landed (#409) — patch the fact so /units/health shows
// the new gates immediately instead of the pre-write value until the next
// lifetime poll. Only ever called after the read-back confirmed it, so this
// cannot invent a gate the unit did not accept.
inline void displayApplyGatesWrite(DisplaySnapshot& snap, int i2cAddress,
                                   uint8_t gates) {
  int idx = i2cAddress - SFP_I2C_ADDRESS_BASE;
  if (idx < 0 || idx >= UNITS_AMOUNT) return;
  snap.units[idx].lifetime.featureGates = gates;
}

// A unit sent into twiboot forgets nothing, but the master must stop
// serving reads for it until the next probe confirms it is back in sketch.
inline void displayInvalidateUnitReads(DisplaySnapshot& snap, int i2cAddress) {
  int idx = i2cAddress - SFP_I2C_ADDRESS_BASE;
  if (idx < 0 || idx >= UNITS_AMOUNT) return;
  snap.units[idx].offsetValid = false;
  snap.units[idx].statusValid = false;
  snap.units[idx].odometerValid = false;
}

// --- /unit/op-result (#204) -------------------------------------------------------

enum class OpResultState : uint8_t { Pending, Found, Expired };

// The seq counter is monotonic, so ordering answers everything: the slot
// hasn't reached the queried op (pending), holds it (found), or moved past
// it (expired — the outcome is gone for good; UI treats it as unknown).
inline OpResultState opResultQuery(const MaintResult& slot, uint32_t seq) {
  if (slot.seq < seq) return OpResultState::Pending;
  if (slot.seq == seq && slot.outcome != MaintOutcome::Pending)
    return OpResultState::Found;
  if (slot.seq == seq) return OpResultState::Pending;
  return OpResultState::Expired;
}

inline const char* maintOutcomeName(MaintOutcome o) {
  switch (o) {
    case MaintOutcome::Ok:                 return "ok";
    case MaintOutcome::WireFail:           return "wire-fail";
    case MaintOutcome::ExecValidationFail: return "exec-validation-fail";
    case MaintOutcome::PostconditionFail:  return "postcondition-fail";
    default:                               return "pending";
  }
}

inline const char* maintReasonName(MaintReason r) {
  switch (r) {
    case MaintReason::UnitMissingAfterReprobe:
      return "unit-missing-after-reprobe";
    case MaintReason::TargetAddressOccupied:
      return "target-address-occupied";
    default:
      return "";
  }
}

// Renders the reflash progress JSON (#205) — spliced into the /units/health
// payload by the web layer (additive key; v1 clients ignore it). Fits well
// inside 96 bytes.
inline void buildReflashJson(char* buf, size_t cap,
                             const ReflashProgress& p) {
  snprintf(buf, cap,
           "{\"state\":\"%s\",\"total\":%u,\"done\":%u,\"failed\":%u,"
           "\"cur\":%u,\"halted\":%s}",
           reflashStateName(p.state), (unsigned)p.total, (unsigned)p.done,
           (unsigned)p.failed, (unsigned)p.currentAddr,
           p.halted ? "true" : "false");
}

inline const char* selfTestOutcomeName(SelfTestOutcome o) {
  switch (o) {
    case SelfTestOutcome::Ok:          return "ok";
    case SelfTestOutcome::WireFail:    return "wire-fail";
    case SelfTestOutcome::Timeout:     return "timeout";
    case SelfTestOutcome::UnitFailed:  return "unit-failed";
    case SelfTestOutcome::Unsupported: return "unsupported";
    case SelfTestOutcome::Aborted:     return "aborted";
    default:                           return "pending";
  }
}

// Renders the self-test result JSON for a queried seq (#265) — the same
// pending / found / expired ordering rules as opResultQuery, from the
// self-test's own slot. Fits well inside 128 bytes.
inline void buildSelfTestJson(char* buf, size_t cap, const SelfTestSlot& slot,
                              uint32_t seq) {
  if (slot.seq < seq ||
      (slot.seq == seq && slot.outcome == SelfTestOutcome::Pending)) {
    snprintf(buf, cap, "{\"state\":\"pending\"}");
    return;
  }
  if (slot.seq > seq) {
    snprintf(buf, cap, "{\"state\":\"expired\"}");
    return;
  }
  if (slot.outcome == SelfTestOutcome::Ok) {
    snprintf(buf, cap,
             "{\"state\":\"ok\",\"steps_per_rev\":%u,\"hall_window\":%u,"
             "\"rev_time_ms\":%u}",
             (unsigned)slot.stepsPerRev, (unsigned)slot.hallWindowSteps,
             (unsigned)slot.revTimeMs);
    return;
  }
  // #404: the failure branch used to be a bare state+reason with every
  // measurement zeroed, so /unit/self-test-result said exactly why the MASTER
  // gave up and nothing about what the unit found. Now it carries the unit's
  // own failure mode and whatever it managed to measure first.
  snprintf(buf, cap,
           "{\"state\":\"failed\",\"reason\":\"%s\",\"unit_reason\":\"%s\","
           "\"steps_per_rev\":%u,\"hall_window\":%u,\"rev_time_ms\":%u}",
           selfTestOutcomeName(slot.outcome),
           selfTestReasonName(slot.unitReason),
           (unsigned)slot.stepsPerRev, (unsigned)slot.hallWindowSteps,
           (unsigned)slot.revTimeMs);
}

// Renders the op-result JSON for a queried seq from the (mutex-copied)
// result slot. Fits well inside 96 bytes.
inline void buildOpResultJson(char* buf, size_t cap, const MaintResult& slot,
                              uint32_t seq) {
  switch (opResultQuery(slot, seq)) {
    case OpResultState::Pending:
      snprintf(buf, cap, "{\"state\":\"pending\"}");
      return;
    case OpResultState::Expired:
      snprintf(buf, cap, "{\"state\":\"expired\"}");
      return;
    case OpResultState::Found:
      break;
  }
  if (slot.outcome == MaintOutcome::Ok) {
    snprintf(buf, cap, "{\"state\":\"ok\"}");
  } else if (slot.reason == MaintReason::None) {
    snprintf(buf, cap, "{\"state\":\"failed\",\"reason\":\"%s\"}",
             maintOutcomeName(slot.outcome));
  } else {
    snprintf(buf, cap,
             "{\"state\":\"failed\",\"reason\":\"%s\",\"detail\":\"%s\"}",
             maintOutcomeName(slot.outcome), maintReasonName(slot.reason));
  }
}
