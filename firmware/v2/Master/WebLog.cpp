#include "WebLog.h"

#include "LargeAlloc.h"
// The flash-log tee is target-only (FlashLog.cpp needs LittleFS); native
// tests include this .cpp directly, same pattern as HelpersSerialHandling.
#ifndef UNIT_TEST
#include "FlashLog.h"
#endif

#ifndef WEBLOG_DISABLE

// Byte-oriented ring buffer. We capture raw output rather than framed lines
// so the SerialPrint / SerialPrintln helpers don't need to agree on where a
// line ends; readers simply get the last WEBLOG_SIZE bytes of serial-style
// output, newlines intact.
//
// Locking (#187): a FreeRTOS mutex, not v1's noInterrupts() and not the
// earlier spinlock — with a 32 KB ring the read copy is far too long for a
// critical section (interrupts off), and no writer runs in ISR context
// (SerialPrint is task-level everywhere). Under the native test env there
// is no concurrency — the lock compiles away.

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
static SemaphoreHandle_t webLogMutex = nullptr;
#define WEBLOG_LOCK() xSemaphoreTake(webLogMutex, portMAX_DELAY)
#define WEBLOG_UNLOCK() xSemaphoreGive(webLogMutex)
#else
#define WEBLOG_LOCK()
#define WEBLOG_UNLOCK()
#endif

// Allocated once by webLogInit() via largeAlloc() (PSRAM-preferred). Null
// until then: appends before init are dropped — SerialPrint must be safe to
// call from the first line of setup() without ordering surprises.
static char* webLogBuffer = nullptr;
static size_t webLogHead = 0;
static bool webLogWrapped = false;

WebLogPrinter webLogPrinter;

size_t WebLogPrinter::write(uint8_t b) {
  webLogAppend((const char*)&b, 1);
  return 1;
}

size_t WebLogPrinter::write(const uint8_t* buffer, size_t size) {
  webLogAppend((const char*)buffer, size);
  return size;
}

// Single-threaded by contract: called once from setup(), before tasksInit()
// spawns anything that could race the check-then-act guard below.
void webLogInit() {
  static bool attempted = false;
  if (attempted) return;  // idempotent, including after a failed attempt
  attempted = true;
#if defined(ARDUINO_ARCH_ESP32)
  webLogMutex = xSemaphoreCreateMutex();
  if (webLogMutex == nullptr) {
    // Degrade to serial-only logging: the buffer stays null, so every entry
    // point no-ops and never reaches WEBLOG_LOCK() on a null handle.
    Serial.println(F("WebLog: mutex allocation failed — GET /log disabled"));
    return;
  }
#endif
  // The mutex exists before the buffer pointer is published: every entry
  // point gates on the pointer, so no caller can reach WEBLOG_LOCK() first.
  webLogBuffer = (char*)largeAlloc(WEBLOG_SIZE);
#if defined(ARDUINO_ARCH_ESP32)
  if (webLogBuffer == nullptr) {
    // Loud on purpose: a silent failure here reads as "web log empty".
    Serial.printf("WebLog: %u-byte ring allocation failed — GET /log disabled\n",
                  (unsigned)WEBLOG_SIZE);
  }
#endif
}

void webLogAppend(const char* data, size_t len) {
  // Tee into the persistent flash log (#206) BEFORE the ring-buffer gate:
  // a failed ring allocation must not silence the flash log (and vice
  // versa — flashLogStage gates on its own init state).
#ifndef UNIT_TEST
  flashLogStage(data, len);
#endif
  if (webLogBuffer == nullptr || data == nullptr || len == 0) return;

  WEBLOG_LOCK();
  for (size_t i = 0; i < len; i++) {
    webLogBuffer[webLogHead] = data[i];
    webLogHead++;
    if (webLogHead >= WEBLOG_SIZE) {
      webLogHead = 0;
      webLogWrapped = true;
    }
  }
  WEBLOG_UNLOCK();
}

String webLogRead() {
  String out;
  if (webLogBuffer == nullptr) return out;
  // Reserve BEFORE taking the lock so the append loop below never
  // reallocates while other writers wait behind the mutex.
  if (!out.reserve(WEBLOG_SIZE + 1)) return out;

  WEBLOG_LOCK();
  if (webLogWrapped) {
    // Oldest slice lives from `head` to end of buffer; newest from 0 to `head`.
    for (size_t i = webLogHead; i < WEBLOG_SIZE; i++) out += webLogBuffer[i];
    for (size_t i = 0; i < webLogHead; i++)           out += webLogBuffer[i];
  } else {
    for (size_t i = 0; i < webLogHead; i++)           out += webLogBuffer[i];
  }
  WEBLOG_UNLOCK();

  return out;
}

void webLogReset() {
  if (webLogBuffer == nullptr) return;
  WEBLOG_LOCK();
  webLogHead = 0;
  webLogWrapped = false;
  WEBLOG_UNLOCK();
}

#else

WebLogPrinter webLogPrinter;
size_t WebLogPrinter::write(uint8_t)                     { return 1; }
size_t WebLogPrinter::write(const uint8_t*, size_t size) { return size; }
void   webLogInit()                                      {}
void   webLogAppend(const char*, size_t)                 {}
String webLogRead() { return String("(web log disabled at build time)"); }
void   webLogReset() {}

#endif
