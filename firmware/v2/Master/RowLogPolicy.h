#pragma once
// RowLogPolicy.h — a row board's log as the master keeps it (#559/#572).
// Pure, natively tested by test_row_log. A row sends its log lines over the
// wall link only while the master asks for them (LogCtl): first what its
// 4 KB ring still holds and has not sent, then each new line. The master
// asks while someone has read the log lately, and keeps what arrived in a
// ring of its own.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// What the master keeps of one row's log: twice the row's own ring, so its
// whole backlog and as much again of what follows fit.
#define ROW_LOG_KEEP_BYTES 8192
// How long after the last read the master goes on asking: a page that shows
// the log reads it every few seconds; one that was closed stops costing the
// row anything after this.
#define ROW_LOG_HOLD_MS 60000UL

struct RowLogWant {
  bool ever = false;
  uint32_t askedMs = 0;

  void asked(uint32_t nowMs) {
    ever = true;
    askedMs = nowMs;
  }
  // `nowMs` may be a little older than the ask (the reader stamps its own
  // moment, the link task judges by the start of its pass): an ask from just
  // ahead counts as fresh.
  bool wanted(uint32_t nowMs) const {
    return ever && (int32_t)(nowMs - askedMs) < (int32_t)ROW_LOG_HOLD_MS;
  }
};

// Lines in arrival order, each ended by '\n'. When full, whole lines go from
// the old end. The memory is the caller's.
struct RowLogRing {
  char* buf = nullptr;
  size_t cap = 0;
  size_t len = 0;  // bytes held, always from buf[0]

  void begin(char* memory, size_t size) {
    buf = memory;
    cap = size;
    len = 0;
  }
  void clear() { len = 0; }

  void append(const uint8_t* line, size_t n) {
    if (buf == nullptr || cap < 2) return;
    // A line too long for the ring keeps its end.
    if (n > cap - 1) {
      line += n - (cap - 1);
      n = cap - 1;
    }
    while (len + n + 1 > cap) dropOldest();
    for (size_t i = 0; i < n; i++) {
      // What is inside a line stays inside it, and reads as text.
      const uint8_t c = line[i];
      buf[len++] = (c < 0x20 || c == 0x7F) ? ' ' : (char)c;
    }
    buf[len++] = '\n';
  }

  // The newest whole lines that fit `out`; returns their length.
  size_t read(char* out, size_t outCap) const {
    size_t from = 0;
    while (len - from > outCap) {
      const char* end = (const char*)memchr(buf + from, '\n', len - from);
      if (end == nullptr) return 0;
      from = (size_t)(end - buf) + 1;
    }
    memcpy(out, buf + from, len - from);
    return len - from;
  }

 private:
  void dropOldest() {
    const char* end = (const char*)memchr(buf, '\n', len);
    const size_t gone = end == nullptr ? len : (size_t)(end - buf) + 1;
    memmove(buf, buf + gone, len - gone);
    len -= gone;
  }
};
