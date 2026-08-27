// OdometerLog.cpp — glue for the odometer historian (#465); contract and
// ownership in OdometerLog.h, decisions in OdometerLogPolicy.h.

#include "OdometerLog.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <time.h>

#include "FlashLog.h"  // flashLogAvailable() — same storage mount
#include "HelpersSerialHandling.h"
#include "OdometerLogPolicy.h"
#include "SplitFlapProtocol.h"  // SFP_I2C_ADDRESS_BASE
#include "Tasks.h"              // displaySnapshotGet()

#define ODOLOG_CHECK_INTERVAL_MS 60000UL

static const char* ODOLOG_PATH = "/odolog.csv";
static const char* ODOLOG_PREV_PATH = "/odolog.prev.csv";

// netTask-only state (single caller, no locking needed).
static OdologSlotState slotState[UNITS_AMOUNT];
static uint32_t lastCheckMs = 0;

bool odometerLogAvailable() { return flashLogAvailable(); }
const char* odometerLogCurrentPath() { return ODOLOG_PATH; }
const char* odometerLogPreviousPath() { return ODOLOG_PREV_PATH; }

void odometerLogTick() {
  if (!flashLogAvailable()) return;
  uint32_t now = millis();
  if (lastCheckMs != 0 && (uint32_t)(now - lastCheckMs) < ODOLOG_CHECK_INTERVAL_MS) {
    return;
  }
  lastCheckMs = now;

  uint32_t epochS = (uint32_t)time(nullptr);
  DisplaySnapshot snap = displaySnapshotGet();

  File f;
  bool open = false;
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    const UnitFacts& u = snap.units[i];
    OdologAction action =
        odologDecide(slotState[i], epochS, u.odometer, u.odometerValid);
    if (action == ODOLOG_SKIP) continue;
    if (!open) {
      f = LittleFS.open(ODOLOG_PATH, "a");
      if (!f) {
        SerialPrintln(F("odolog: append open failed"));
        return;  // slot state untouched — the next tick retries
      }
      open = true;
    }
    char row[40];
    size_t n = odologFormatRow(row, sizeof(row), epochS,
                               (uint8_t)(SFP_I2C_ADDRESS_BASE + i),
                               u.odometer, action == ODOLOG_APPEND_RESET);
    if (n == 0 || f.write((const uint8_t*)row, n) != n) {
      SerialPrintln(F("odolog: row write failed"));
      break;  // state not advanced for this slot — retried next tick
    }
    odologApplyAppend(slotState[i], epochS, u.odometer);
  }
  if (!open) return;

  size_t size = f.size();
  f.close();
  if (odologShouldRotate(size)) {
    // Same bounded-footprint discipline as the flash log: prev is dropped,
    // current becomes prev, appends start a fresh file next tick.
    LittleFS.remove(ODOLOG_PREV_PATH);
    if (!LittleFS.rename(ODOLOG_PATH, ODOLOG_PREV_PATH)) {
      SerialPrintln(F("odolog: rotate rename failed"));
    }
  }
}
