#pragma once

#include <Arduino.h>
#ifndef UNIT_TEST
#include "WebLog.h"
#endif

// Centralised serial output so each call site doesn't need its own
// `#if SERIAL_ENABLE` guard. Templates live in a header because the
// Arduino sketch preprocessor can't auto-prototype templates correctly.
//
// Output is also mirrored to the in-RAM web log ring buffer (exposed at
// GET /log) so you can see firmware activity from the UI without needing
// a USB-serial connection. The web-log tap runs regardless of
// SERIAL_ENABLE; it's skipped in the native test environment so the
// sketch logic stays linkable there.

template <typename T>
void SerialPrint(T value) {
#if SERIAL_ENABLE == true
  Serial.print(value);
#endif
#ifndef UNIT_TEST
  webLogPrinter.print(value);
#endif
}

template <typename T>
void SerialPrintf(const char* message, T value) {
#if SERIAL_ENABLE == true
  Serial.printf(message, value);
#endif
#ifndef UNIT_TEST
  char buf[96];
  int n = snprintf(buf, sizeof(buf), message, value);
  if (n > 0) {
    webLogAppend(buf, n > (int)sizeof(buf) - 1 ? (int)sizeof(buf) - 1 : n);
  }
#endif
}

template <typename T>
void SerialPrintln(T value) {
#if SERIAL_ENABLE == true
  Serial.println(value);
#endif
#ifndef UNIT_TEST
  webLogPrinter.println(value);
#endif
}
