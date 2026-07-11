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

struct DisplaySnapshot {
  // v1 probe-fallback parity: until a probe answers, assume the ceiling.
  uint8_t displayWidth = UNITS_AMOUNT;
  bool busy = false;
  uint32_t commandsProcessed = 0;
  char currentText[DISPLAY_CMD_TEXT_LEN + 1] = {0};
  // Probe/health facts (#203). Derived fields are recomputed by
  // displayApplyUnitFacts(), never patched individually.
  uint8_t detectedUnitCount = 0;
  uint8_t faultyUnitCount = 0;
  UnitFacts units[UNITS_AMOUNT];
  MaintResult lastMaint;
  // Reflash job progress (#205) — published at unit boundaries and settle
  // transitions while the job runs; the producer gate keys off it.
  ReflashProgress reflash;
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
inline void displayApplyUnitFacts(DisplaySnapshot& snap,
                                  const UnitFacts* facts, int maxUnits) {
  // Clamp once: every fixed-size array below is UNITS_AMOUNT-bounded, so a
  // larger caller value must never reach the derive calls either.
  if (maxUnits > UNITS_AMOUNT) maxUnits = UNITS_AMOUNT;
  int states[UNITS_AMOUNT];
  for (int i = 0; i < maxUnits; i++) {
    snap.units[i] = facts[i];
    states[i] = facts[i].state;
  }
  snap.displayWidth = (uint8_t)computeDisplayWidth(states, maxUnits);
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

// A successful SET_OFFSET is the only in-place offset mutation; everything
// else flows through a probe's wholesale fact rewrite.
inline void displayApplyOffsetWrite(DisplaySnapshot& snap, int i2cAddress,
                                    int16_t value) {
  int idx = i2cAddress - SFP_I2C_ADDRESS_BASE;
  if (idx < 0 || idx >= UNITS_AMOUNT) return;
  snap.units[idx].offset = value;
  snap.units[idx].offsetValid = true;
}

// A unit sent into twiboot forgets nothing, but the master must stop
// serving reads for it until the next probe confirms it is back in sketch.
inline void displayInvalidateUnitReads(DisplaySnapshot& snap, int i2cAddress) {
  int idx = i2cAddress - SFP_I2C_ADDRESS_BASE;
  if (idx < 0 || idx >= UNITS_AMOUNT) return;
  snap.units[idx].offsetValid = false;
  snap.units[idx].statusValid = false;
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
           "\"cur\":%u}",
           reflashStateName(p.state), (unsigned)p.total, (unsigned)p.done,
           (unsigned)p.failed, (unsigned)p.currentAddr);
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
