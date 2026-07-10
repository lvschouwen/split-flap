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
#include "UnitHealth.h"

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
};

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
    case DisplayOpcode::Probe:
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
