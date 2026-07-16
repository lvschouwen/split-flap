#include "WebLog.h"

#include "LargeAlloc.h"
#include "LogLinePrefixer.h"
// The flash-log tee is target-only (FlashLog.cpp needs LittleFS); native
// tests include this .cpp directly, same pattern as HelpersSerialHandling.
#ifndef UNIT_TEST
#include <time.h>

#include <cstdio>

#include "ClockPolicy.h"  // clockIsTimeSynced — same sync cutoff as FlashLog
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

#ifndef UNIT_TEST
// Timestamp prefixer (#318 E). One shared instance: every append arrives
// under HelpersSerialHandling's SerialPrintLock, so its cross-call line-start
// state needs no lock of its own.
static LogLinePrefixer webLogPrefixer;

// "HH:MM:SS" for the current line. Wall-clock once SNTP has synced (epoch
// past 2020-09), otherwise the uptime — labelled the same way so a boot-time
// line still sorts and reads sensibly before the clock is set.
static void webLogStamp(char* buf, size_t n) {
  time_t now = time(nullptr);
  struct tm ti;
  if (clockIsTimeSynced(now) && localtime_r(&now, &ti) != nullptr) {
    strftime(buf, n, "%H:%M:%S", &ti);
  } else {
    unsigned long s = millis() / 1000UL;
    snprintf(buf, n, "%02lu:%02lu:%02lu", (s / 3600UL) % 100UL,
             (s / 60UL) % 60UL, s % 60UL);
  }
}
#endif

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

// Copy `data`/`len` into the ring under the lock. Caller has already applied
// any timestamp expansion.
static void webLogRingWrite(const char* data, size_t len) {
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

#ifndef UNIT_TEST
void webLogAppend(const char* data, size_t len) {
  // Degenerate appends: keep the flash-log passthrough byte-for-byte (the
  // expander below would dereference a null `data`), then bail.
  if (data == nullptr || len == 0) {
    flashLogStage(data, len);
    return;
  }
  // Prefix "HH:MM:SS " to each line (#318 E), then tee the SAME expanded bytes
  // to both sinks so the flash log (#206) and the RAM ring stay identical.
  char stamp[9];
  webLogStamp(stamp, sizeof(stamp));
  String expanded;
  expanded.reserve(len + 16);
  webLogPrefixer.expand(stamp, data, len, expanded);
  // The flash log (#206) applies its OWN independent per-line stamper
  // (FlashLog.cpp), so it must get the RAW bytes — handing it the expanded
  // copy would double-stamp /log/flash. The #318E prefix is for the RAM ring
  // (GET /log) only, which carried no timestamps before. Flash tee still runs
  // BEFORE the ring gate so a failed ring allocation can't silence it.
  flashLogStage(data, len);
  webLogRingWrite(expanded.c_str(), expanded.length());
}
#else
// Native build: verbatim byte ring, no timestamps and no flash tee (both are
// target-only). Kept identical to the pre-#318 path so test_web_log's exact
// byte assertions stay orthogonal to the LogLinePrefixer's own tests.
void webLogAppend(const char* data, size_t len) {
  webLogRingWrite(data, len);
}
#endif

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
