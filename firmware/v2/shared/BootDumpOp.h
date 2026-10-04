#pragma once
// BootDumpOp.h — reading a unit's bootloader section over I2C (#511), as one
// sequence both row masters run: send the unit into twiboot, read the section,
// start its application again, wait for it to come back, home it and put the
// row's frame back. No flash write. A function template over the tree's
// hooks, like BootUpdateOp.h. Natively tested by test_boot_update_op.
//
//   int   enterBootloader(uint8_t addr)            0 = ACKed
//   void  unitLeftSketch(uint8_t addr)   reads are stale, hold the probes
//   void  holdProbes()                   keep runtime probes off the unit
//   void  pause(uint32_t ms)
//   UnitBootReadResult readBootSection(uint8_t addr, uint8_t* out)
//   bool  waitIdle(uint8_t addr, uint32_t timeoutMs)
//   int   home(uint8_t addr)                       0 = ACKed
//   void  reshow()                       put the row's last frame back

#include <stdint.h>

#include "BootDump.h"
#include "UnitBusTwiboot.h"  // UnitBootReadResult
#include "UnitTimings.h"

template <typename Hooks>
inline BootDumpOutcome bootDumpRun(Hooks& h, uint8_t addr, uint8_t* out) {
  if (h.enterBootloader(addr) != 0) {
    // A NACK does not prove the unit stayed in its sketch.
    h.holdProbes();
    return BootDumpOutcome::EnterFail;
  }
  h.unitLeftSketch(addr);
  h.pause(TWIBOOT_STARTUP_MS);
  BootDumpOutcome outcome = BootDumpOutcome::Ok;
  switch (h.readBootSection(addr, out)) {
    case UnitBootReadResult::Ok:
      break;
    case UnitBootReadResult::BootloaderSilent:
      outcome = BootDumpOutcome::BootloaderSilent;
      break;
    case UnitBootReadResult::ChipMismatch:
      outcome = BootDumpOutcome::ChipMismatch;
      break;
    case UnitBootReadResult::ReadFailed:
      outcome = BootDumpOutcome::ReadFail;
      break;
  }
  // The unit comes back unhomed. Wait for its sketch and home it — a blank
  // target would not, a frame write skips a unit already reporting its letter
  // — then re-show the frame.
  h.waitIdle(addr, UNIT_RETURN_TIMEOUT_MS);
  if (h.home(addr) == 0) h.waitIdle(addr, UNIT_HOME_TIMEOUT_MS);
  h.reshow();
  // Its reads were invalidated above; keep probes off until it has settled.
  h.holdProbes();
  return outcome;
}
