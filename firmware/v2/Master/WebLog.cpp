#include "WebLog.h"

#ifndef WEBLOG_DISABLE

// Byte-oriented ring buffer. We capture raw output rather than framed lines
// so the SerialPrint / SerialPrintln helpers don't need to agree on where a
// line ends; readers simply get the last WEBLOG_SIZE bytes of serial-style
// output, newlines intact.
//
// Locking differs from v1: on the ESP8266 `noInterrupts()` sufficed because
// everything shares one core, but here the async web server runs in its own
// FreeRTOS task (possibly on the other core), so writers and readers take a
// spinlock instead. Under the native test env there is no concurrency —
// the lock compiles away.

#if defined(ARDUINO_ARCH_ESP32)
static portMUX_TYPE webLogMux = portMUX_INITIALIZER_UNLOCKED;
#define WEBLOG_LOCK() portENTER_CRITICAL(&webLogMux)
#define WEBLOG_UNLOCK() portEXIT_CRITICAL(&webLogMux)
#else
#define WEBLOG_LOCK()
#define WEBLOG_UNLOCK()
#endif

static char webLogBuffer[WEBLOG_SIZE];
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

void webLogAppend(const char* data, size_t len) {
  if (data == nullptr || len == 0) return;

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
  // Reserve BEFORE taking the lock: with capacity in place the append loop
  // below never allocates, and heap calls are not allowed inside
  // portENTER_CRITICAL. The copy itself stays inside the lock so a
  // concurrent writer can't tear the bytes mid-read; ~4 KB of plain char
  // appends is a few tens of µs at 240 MHz.
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
  WEBLOG_LOCK();
  webLogHead = 0;
  webLogWrapped = false;
  WEBLOG_UNLOCK();
}

#else

WebLogPrinter webLogPrinter;
size_t WebLogPrinter::write(uint8_t)                     { return 1; }
size_t WebLogPrinter::write(const uint8_t*, size_t size) { return size; }
void   webLogAppend(const char*, size_t)                 {}
String webLogRead() { return String("(web log disabled at build time)"); }
void   webLogReset() {}

#endif
