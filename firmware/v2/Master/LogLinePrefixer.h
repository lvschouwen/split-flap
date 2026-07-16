#pragma once

#include <Arduino.h>

// Stateful line-start tracker that prefixes a timestamp to every log line
// flowing through the web/flash log tee (#318 E). Pure and natively tested:
// the caller supplies the already-formatted stamp string, so the clock
// source (time()/millis()) stays out of this header and out of the native
// build. WebLog.cpp owns the target-only glue that produces the stamp.
//
// The log ring is byte-oriented (WebLog.cpp) — one logical line arrives as
// several appends (SerialPrintln writes the body, then a separate "\r\n").
// So the tracker must survive across calls: `atLineStart` carries whether
// the previous chunk ended on a newline. Callers hold the SerialPrintLock
// helper mutex (HelpersSerialHandling.h), which serializes every append, so
// a single shared instance needs no lock of its own.
struct LogLinePrefixer {
  bool atLineStart = true;

  // Expand `data`/`len` into `out`, inserting `stamp` followed by a space at
  // the start of every non-blank line. Blank lines (a line whose first byte
  // is '\r' or '\n') are left unstamped so the boot banner's spacer lines
  // stay clean. An empty `stamp` disables prefixing entirely (`out` becomes a
  // verbatim copy) — the escape hatch the native WebLog path relies on.
  void expand(const char* stamp, const char* data, size_t len, String& out) {
    const bool stampEnabled = stamp != nullptr && stamp[0] != '\0';
    for (size_t i = 0; i < len; i++) {
      const char c = data[i];
      if (stampEnabled && atLineStart && c != '\r' && c != '\n') {
        out += stamp;
        out += ' ';
      }
      out += c;
      atLineStart = (c == '\n');
    }
  }
};
