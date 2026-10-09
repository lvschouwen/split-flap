#pragma once
// UnitCatchPolicy.h — catching a unit in its bootloader at power-on (#554).
// Pure, natively tested by test_unit_catch; the glue is UnitCatch.cpp.
//
// A unit that runs a program which no longer listens can only be flashed
// from its bootloader, and gets there only by a reset. After any reset the
// bootloader listens for TIMEOUT_MS (1000 ms, UnitBootloader/main.c) and then
// starts the program again; one question inside that second keeps it there
// for SF_PIN_TIMEOUT_MS.
//
// A power cycle of the row resets the unit and this board together, and this
// board's first look at the bus comes seconds later. So a board that could
// not get a unit into its bootloader remembers the address (NVS, it has to
// survive the power cycle), and at its next start asks that one address
// before it does anything else. The display task then finds the unit in its
// bootloader and flashes it.
//
// Whether the first question lands inside the second is a race between this
// board's start and the unit's: nothing here can guarantee it, and a miss
// leaves everything as it was, still armed for the next power cycle.

#include <stdint.h>

#include "UnitHealth.h"  // UnitFacts

// How long setup() keeps asking, and how often. The window is longer than
// the bootloader's second (the unit may come up after this board); the gap
// puts dozens of questions into it.
#define UNIT_CATCH_WINDOW_MS 1500UL
#define UNIT_CATCH_GAP_MS 10UL

// 0 = not armed. Anything else is the unit's bus address.
inline bool unitCatchAddressValid(uint8_t addr, int base, int maxUnits) {
  return addr >= base && addr < base + maxUnits;
}

// After an update run for one unit an operator asked for: a unit that would
// not enter its bootloader, and was not reset by hand while the run waited,
// is the one to catch at the next power-on. A stopped run arms nothing.
inline uint8_t unitCatchAfterForcedRun(uint8_t armed, bool forcedOne,
                                       bool cancelled, uint8_t notEntered,
                                       uint8_t addr) {
  if (!forcedOne || cancelled || notEntered == 0) return armed;
  return addr;
}

enum class UnitCatchStep : uint8_t {
  Nothing,  // not armed, or the unit is not there to act on: stay as is
  Flash,    // the unit sits in its bootloader: flash it now
  Disarm,   // the unit runs a program this board can read: done
};

// After a probe of the bus, at start-up and again after the flash.
inline UnitCatchStep unitCatchAfterProbe(uint8_t armed, const UnitFacts& unit) {
  if (armed == 0) return UnitCatchStep::Nothing;
  if (unit.state == 2) return UnitCatchStep::Flash;
  if (unit.state == 1 && unit.version[0] != '\0') return UnitCatchStep::Disarm;
  return UnitCatchStep::Nothing;
}
