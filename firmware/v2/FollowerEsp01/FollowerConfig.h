#pragma once
// FollowerConfig.h — build knobs + serial helpers for the ESP-01 follower
// (#298). One rule carried over from v1 verbatim: SERIAL_ENABLE trades I2C
// for serial — the ESP-01's only free pins are TX/RX (GPIO1/GPIO3), which
// double as the unit bus (Wire.begin(1, 3), v1 hardware truth). A debug
// build talks on the console and drives no units.

#define SERIAL_ENABLE false
#define SERIAL_BAUDRATE 115200

// Hardware ceiling: max units the DIP-switch addressing supports. Array
// bound only — the effective row width comes from the boot I2C probe
// (DisplayWidth.h), exactly like v1.
#ifndef UNITS_AMOUNT
#define UNITS_AMOUNT 16
#endif

// Unit speed byte range (v1 values; the wire contract's rpm scale).
#define MIN_SPEED 1
#define MAX_SPEED 12

#include <Arduino.h>

#ifndef UNIT_TEST
#include "FollowerLog.h"
#endif

// Trimmed copy of v1's HelpersSerialHandling.h. SERIAL_ENABLE trades I2C for
// the console (v1 hardware truth), so on a real row Serial is a no-op — but
// every line is ALSO mirrored into the in-RAM log ring (FollowerLog.h, #318 E)
// regardless, which is the row's only observability window (GET /log, pulled
// by the leader). The mirror is skipped in the native test env so the pure
// policy headers stay linkable without the ring's target-only sink.
template <typename T>
inline void SerialPrint(T value) {
#if SERIAL_ENABLE == true
  Serial.print(value);
#endif
#ifndef UNIT_TEST
  followerLogPrinter.print(value);
#endif
  (void)value;  // native (UNIT_TEST, SERIAL_ENABLE off): both bodies compile out
}

template <typename T>
inline void SerialPrintln(T value) {
#if SERIAL_ENABLE == true
  Serial.println(value);
#endif
#ifndef UNIT_TEST
  followerLogPrinter.println(value);
#endif
  (void)value;
}
