#pragma once

//Pure logic for deriving the effective display width from the I2C probe
//results (#123). Header-only so `pio test -e native` can exercise it
//without Arduino deps.
//
//Width = highest responding unit index + 1. A dead unit mid-display must
//NOT shrink the layout — the per-slot skip logic in showMessage() already
//handles gaps — only trailing silence narrows the display. Units in
//bootloader mode (state 2) count as present: they are physically there and
//will be running the sketch once auto-install finishes.
//
//When NOTHING responds (bench ESP with no bus wired, SERIAL_ENABLE
//debugging where I2C is off) fall back to the compile-time ceiling so text
//layout still behaves like a full-width display.
inline int computeDisplayWidth(const int* unitStates, int maxUnits) {
  int width = 0;
  for (int i = 0; i < maxUnits; i++) {
    if (unitStates[i] != 0) {
      width = i + 1;
    }
  }
  return width == 0 ? maxUnits : width;
}
