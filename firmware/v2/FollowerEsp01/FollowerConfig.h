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

// Serial-only trimmed copy of v1's HelpersSerialHandling.h — this firmware
// has no web log (/log is deliberately not served).
template <typename T>
inline void SerialPrint(T value) {
#if SERIAL_ENABLE == true
  Serial.print(value);
#else
  (void)value;
#endif
}

template <typename T>
inline void SerialPrintln(T value) {
#if SERIAL_ENABLE == true
  Serial.println(value);
#else
  (void)value;
#endif
}
