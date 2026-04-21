#pragma once

#include <Arduino.h>
#ifndef UNIT_TEST
#include "WebLog.h"
#include "MqttLogTap.h"
#endif

// Centralised serial output so each call site doesn't need its own
// `#if SERIAL_ENABLE` guard. Templates live in a header because the
// Arduino sketch preprocessor can't auto-prototype templates correctly.
//
// Output fans out to three sinks (#50):
//   1. USB serial if SERIAL_ENABLE (usually off — I2C shares the pins).
//   2. The in-RAM ring buffer (WebLog.h), used for pre-MQTT-connect
//      buffering. Flushed to MQTT on every MQTT reconnect.
//   3. The live MQTT log tap — publishes each newline-terminated line to
//      <base>/log when MQTT is connected. No-op otherwise.
// The native test env skips 2 and 3 so the sketch logic stays linkable.

template <typename T>
void SerialPrint(T value) {
#if SERIAL_ENABLE == true
  Serial.print(value);
#endif
#ifndef UNIT_TEST
  webLogPrinter.print(value);
  mqttLogTap.print(value);
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
    size_t clamped = (n > (int)sizeof(buf) - 1) ? sizeof(buf) - 1 : (size_t)n;
    webLogAppend(buf, clamped);
    mqttLogTap.write((const uint8_t*)buf, clamped);
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
  mqttLogTap.println(value);
#endif
}
