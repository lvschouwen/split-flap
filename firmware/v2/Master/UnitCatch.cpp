// UnitCatch.cpp — glue for UnitCatchPolicy.h (#554): the armed address in
// NVS, and the questions asked of it before anything else at start-up.

#include "UnitCatch.h"

#include <Arduino.h>
#include <Preferences.h>

#include "HelpersSerialHandling.h"
#include "SplitFlapProtocol.h"
#include "UnitBus.h"
#include "UnitCatchPolicy.h"

static const char* kNamespace = "sfboot";  // with the boot guard's record
static const char* kKey = "catch";

static uint8_t armed = 0;
static bool loaded = false;

// What the start-up questions found, for unitCatchLogReport(): they are asked
// before the log exists.
static bool askedAtStart = false;
static bool caughtAtStart = false;
static uint32_t firstAskMs = 0;
static uint32_t endMs = 0;

static void load() {
  if (loaded) return;
  loaded = true;
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/true)) return;
  uint8_t stored = prefs.getUChar(kKey, 0);
  prefs.end();
  armed = unitCatchAddressValid(stored, SFP_I2C_ADDRESS_BASE, UNITS_AMOUNT) ? stored : 0;
}

static void store(uint8_t addr) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/false)) {
    SerialPrintln(F("unit catch: NVS open failed — not stored"));
    return;
  }
  prefs.putUChar(kKey, addr);
  prefs.end();
}

uint8_t unitCatchArmed() {
  load();
  return armed;
}

void unitCatchArm(uint8_t addr) {
  load();
  if (!unitCatchAddressValid(addr, SFP_I2C_ADDRESS_BASE, UNITS_AMOUNT) || armed == addr) return;
  armed = addr;
  store(addr);
  SerialPrintf("unit catch: unit 0x%02x will be asked for its bootloader at "
               "the next power-on of this row\n", addr);
}

void unitCatchDisarm() {
  load();
  if (armed == 0) return;
  SerialPrintf("unit catch: unit 0x%02x no longer needs catching\n", armed);
  armed = 0;
  store(0);
}

uint32_t unitCatchAtPowerOn() {
  load();
  if (armed == 0) return 0;
  askedAtStart = true;
  firstAskMs = millis();
  caughtAtStart =
      unitBusCatchAtPowerOn(armed, UNIT_CATCH_WINDOW_MS, UNIT_CATCH_GAP_MS);
  endMs = millis();
  return endMs - firstAskMs;
}

void unitCatchLogReport() {
  if (!askedAtStart) return;
  if (caughtAtStart) {
    SerialPrintf("unit catch: unit 0x%02x answered from its bootloader %lu ms "
                 "after this board started (first asked at %lu ms)\n",
                 armed, (unsigned long)endMs, (unsigned long)firstAskMs);
  } else {
    SerialPrintf("unit catch: unit 0x%02x did not answer from its bootloader "
                 "(asked from %lu to %lu ms after this board started) — still "
                 "armed for the next power-on\n",
                 armed, (unsigned long)firstAskMs, (unsigned long)endMs);
  }
}
