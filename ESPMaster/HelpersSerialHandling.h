#pragma once

#include <Arduino.h>

// Centralised serial output so each call site doesn't need its own
// `#if SERIAL_ENABLE` guard. Templates live in a header because the
// Arduino sketch preprocessor can't auto-prototype templates correctly.

template <typename T>
void SerialPrint(T value) {
#if SERIAL_ENABLE == true
  Serial.print(value);
#endif
}

template <typename T>
void SerialPrintf(const char* message, T value) {
#if SERIAL_ENABLE == true
  Serial.printf(message, value);
#endif
}

template <typename T>
void SerialPrintln(T value) {
#if SERIAL_ENABLE == true
  Serial.println(value);
#endif
}
