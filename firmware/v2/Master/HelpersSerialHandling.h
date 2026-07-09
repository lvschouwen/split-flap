#pragma once

#include <Arduino.h>
#ifndef UNIT_TEST
#include "WebLog.h"
#endif

// Centralised log output, v2 copy of the v1 helper. On the S3 the USB-CDC
// console coexists with I2C (no SERIAL_ENABLE tradeoff — v1's reason for
// gating Serial is gone), so everything goes to Serial unconditionally and
// is mirrored into the in-RAM web log ring (exposed at GET /log). The
// web-log tap is skipped in the native test environment so included sketch
// logic stays linkable there.

template <typename T>
void SerialPrint(T value) {
  Serial.print(value);
#ifndef UNIT_TEST
  webLogPrinter.print(value);
#endif
}

template <typename T>
void SerialPrintf(const char* message, T value) {
  Serial.printf(message, value);
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
  Serial.println(value);
#ifndef UNIT_TEST
  webLogPrinter.println(value);
#endif
}
