#pragma once
// OdometerLogPolicy.h — pure decision logic for the odometer historian
// (#465). The unit's own EEPROM stays authoritative for its revolution
// count; this only decides when a master-read value is worth persisting to
// the shared LittleFS so a sweep/resize/EEPROM wipe stops erasing the wear
// history with it. Glue (netTask tick, LittleFS writes) in OdometerLog.cpp.
//
// Record format: CSV rows "epoch,address,revolutions[,R]\n". R marks a
// backwards jump observed while the master was up (reset sweep, ring
// resize, a replaced Nano) — a reset is a fact, not a gap, and lifetime
// wear is the sum across the epochs the R rows delimit. Discontinuities
// that happen while the master is down carry no R; they are still visible
// in the series because every boot appends a baseline row per unit.

#include <stdint.h>
#include <stdio.h>

// One row per unit per interval once the value moves — 21 rows/day on the
// current wall, so even years of history stay far under the file cap.
#define ODOLOG_INTERVAL_S (24UL * 60UL * 60UL)

// Rows are ordered by wall-clock epoch, so an unsynced clock (SNTP not up
// yet) must not write: a 1970 row would sort the series wrong forever.
// Anything before 2020 cannot be a synced reading on this project.
#define ODOLOG_MIN_EPOCH 1577836800UL

// Current-file cap; rotation (current -> prev) bounds the storage-partition
// footprint at 2x, same discipline as FlashLogPolicy's log files.
#define ODOLOG_FILE_CAP (48 * 1024)

// Per-slot RAM state, zeroed at boot — "haveLogged" is per boot on purpose:
// the first valid read after every boot appends a baseline row (see R note
// above).
struct OdologSlotState {
  uint32_t lastLogged = 0;
  uint32_t lastEpochS = 0;
  bool haveLogged = false;
};

enum OdologAction : uint8_t {
  ODOLOG_SKIP = 0,
  ODOLOG_APPEND,
  ODOLOG_APPEND_RESET,  // value went backwards vs the last row this boot
};

inline OdologAction odologDecide(const OdologSlotState& s, uint32_t nowEpochS,
                                 uint32_t odometer, bool odometerValid) {
  if (!odometerValid) return ODOLOG_SKIP;  // silent/bootloader/probe gap
  if (nowEpochS < ODOLOG_MIN_EPOCH) return ODOLOG_SKIP;
  if (!s.haveLogged) return ODOLOG_APPEND;
  if (odometer < s.lastLogged) return ODOLOG_APPEND_RESET;
  if (odometer != s.lastLogged &&
      (uint32_t)(nowEpochS - s.lastEpochS) >= ODOLOG_INTERVAL_S) {
    return ODOLOG_APPEND;
  }
  return ODOLOG_SKIP;
}

inline void odologApplyAppend(OdologSlotState& s, uint32_t nowEpochS,
                              uint32_t odometer) {
  s.lastLogged = odometer;
  s.lastEpochS = nowEpochS;
  s.haveLogged = true;
}

// One CSV row into out; returns bytes written (no NUL counted), 0 when it
// would not fit.
inline size_t odologFormatRow(char* out, size_t cap, uint32_t epochS,
                              uint8_t address, uint32_t odometer, bool reset) {
  int n = snprintf(out, cap, "%lu,%u,%lu%s\n", (unsigned long)epochS,
                   (unsigned)address, (unsigned long)odometer,
                   reset ? ",R" : "");
  if (n <= 0 || (size_t)n >= cap) return 0;
  return (size_t)n;
}

inline bool odologShouldRotate(size_t fileSize) {
  return fileSize >= ODOLOG_FILE_CAP;
}
