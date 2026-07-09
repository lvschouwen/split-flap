#pragma once
// DisplayIpc.h — the display task's published state (#187), pure logic.
//
// DisplaySnapshot is single-writer: only the display task mutates it, and
// consumers get a mutex-guarded copy via displaySnapshotGet() (Tasks.cpp) —
// web handlers build JSON from their copy, never from live state. POD for
// the same reason as DisplayCommand: copyable without heap or locks held
// long. displayApplyCommand() is the worker's state transition; the stub
// worker of this slice and the real I2C worker of a later slice share this
// contract, only the hardware side effects differ.

#include "DisplayCommand.h"

struct DisplaySnapshot {
  // v1 probe-fallback parity: until a probe answers, assume the ceiling.
  uint8_t displayWidth = UNITS_AMOUNT;
  bool busy = false;
  uint32_t commandsProcessed = 0;
  char currentText[DISPLAY_CMD_TEXT_LEN + 1] = {0};
};

// Applies one command's state effects to the snapshot. Returns false (no
// mutation) for commands the worker can't execute.
inline bool displayApplyCommand(DisplaySnapshot& snap,
                                const DisplayCommand& cmd) {
  switch (cmd.opcode) {
    case DisplayOpcode::ShowText:
      memcpy(snap.currentText, cmd.text, sizeof(snap.currentText));
      snap.commandsProcessed++;
      return true;
    case DisplayOpcode::Probe:
      // Stub worker: no bus yet, width stays at the fallback ceiling. The
      // I2C slice replaces this with the real highest-responder+1 scan.
      snap.commandsProcessed++;
      return true;
    default:
      return false;
  }
}
