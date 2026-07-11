#pragma once

#include <Arduino.h>

#include <cstdarg>
#ifndef UNIT_TEST
#include "WebLog.h"
#endif

// Centralised log output, v2 copy of the v1 helper. On the S3 the USB-CDC
// console coexists with I2C (no SERIAL_ENABLE tradeoff — v1's reason for
// gating Serial is gone), so everything goes to Serial unconditionally and
// is mirrored into the in-RAM web log ring (exposed at GET /log). The
// web-log tap is skipped in the native test environment so included sketch
// logic stays linkable there.
//
// Since the task skeleton (#187) these helpers have genuinely concurrent
// callers on both cores, and one logical line is several writes (Serial
// body, Serial CRLF, web-log mirror). A helper-scope mutex keeps lines
// whole. Lock-order rule: this lock is taken around leaf operations only —
// nothing inside it may take webStateMutex/snapshotMutex. Two leaf mutexes
// nest inside it by design, one direction only and never held together:
// webLogMutex and FlashLog's stageMutex (both reached via webLogAppend,
// which takes them sequentially — see WebLog.cpp / FlashLog.cpp).

#if defined(ARDUINO_ARCH_ESP32) && !defined(UNIT_TEST)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct SerialPrintLock {
  // Magic-static init is thread-safe (C++11); tolerate a failed create by
  // skipping the lock — interleaved output beats a crashed logger.
  static SemaphoreHandle_t handle() {
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
  }
  SerialPrintLock() {
    if (handle()) xSemaphoreTake(handle(), portMAX_DELAY);
  }
  ~SerialPrintLock() {
    if (handle()) xSemaphoreGive(handle());
  }
  SerialPrintLock(const SerialPrintLock&) = delete;
  SerialPrintLock& operator=(const SerialPrintLock&) = delete;
};
#else
struct SerialPrintLock {};  // single-threaded environments: no-op
#endif

template <typename T>
void SerialPrint(T value) {
  SerialPrintLock lock;
  Serial.print(value);
#ifndef UNIT_TEST
  webLogPrinter.print(value);
#endif
}

template <typename T>
void SerialPrintf(const char* message, T value) {
  SerialPrintLock lock;
  Serial.printf(message, value);
#ifndef UNIT_TEST
  char buf[96];
  int n = snprintf(buf, sizeof(buf), message, value);
  if (n > 0) {
    webLogAppend(buf, n > (int)sizeof(buf) - 1 ? (int)sizeof(buf) - 1 : n);
  }
#endif
}

// Varargs overload for multi-value lines (#212). An exactly-two-argument
// call still resolves to the template above (a deduced exact match beats the
// ellipsis), so existing callers keep their code path.
inline void SerialPrintf(const char* message, ...)
    __attribute__((format(printf, 1, 2)));
inline void SerialPrintf(const char* message, ...) {
  char buf[192];
  va_list args;
  va_start(args, message);
  int n = vsnprintf(buf, sizeof(buf), message, args);
  va_end(args);
  if (n <= 0) return;
  if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
  SerialPrintLock lock;
  Serial.write(reinterpret_cast<const uint8_t*>(buf), (size_t)n);
#ifndef UNIT_TEST
  webLogAppend(buf, n);
#endif
}

template <typename T>
void SerialPrintln(T value) {
  SerialPrintLock lock;
  Serial.println(value);
#ifndef UNIT_TEST
  webLogPrinter.println(value);
#endif
}
