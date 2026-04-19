#include "WebLog.h"

#ifndef WEBLOG_DISABLE

// Byte-oriented ring buffer. We capture raw output rather than framed lines
// so the SerialPrint / SerialPrintf / SerialPrintln helpers don't need to
// agree on where a line ends; what readers see is simply the last WEBLOG_SIZE
// bytes of serial-style output, newlines intact.

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

  // The Wire library runs under interrupts on the ESP8266; protect the
  // shared pointer state even though the body is short.
  noInterrupts();
  for (size_t i = 0; i < len; i++) {
    webLogBuffer[webLogHead] = data[i];
    webLogHead++;
    if (webLogHead >= WEBLOG_SIZE) {
      webLogHead = 0;
      webLogWrapped = true;
    }
  }
  interrupts();
}

String webLogRead() {
  String out;

  noInterrupts();
  size_t head = webLogHead;
  bool wrapped = webLogWrapped;
  interrupts();

  if (wrapped) {
    out.reserve(WEBLOG_SIZE + 1);
    // Oldest slice lives from `head` to end of buffer; newest from 0 to `head`.
    for (size_t i = head; i < WEBLOG_SIZE; i++) out += webLogBuffer[i];
    for (size_t i = 0; i < head; i++)           out += webLogBuffer[i];
  } else {
    out.reserve(head + 1);
    for (size_t i = 0; i < head; i++)           out += webLogBuffer[i];
  }

  return out;
}

#else

WebLogPrinter webLogPrinter;
size_t WebLogPrinter::write(uint8_t)                     { return 1; }
size_t WebLogPrinter::write(const uint8_t*, size_t size) { return size; }
void   webLogAppend(const char*, size_t)                 {}
String webLogRead() { return String("(web log disabled at build time)"); }

#endif
