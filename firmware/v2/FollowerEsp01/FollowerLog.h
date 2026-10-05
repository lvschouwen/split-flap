#pragma once

#include <stdio.h>
#include <Arduino.h>
#include <Print.h>

// In-RAM log ring for the ESP-01 row (#318 E). The board has NO serial
// console — GPIO1/3 are the unit bus (FollowerConfig.h), so this ring is the
// ONLY way to see what the row is doing. It goes up the wall link as LogLine
// messages while the master asks for it (FollowerLink.cpp).
//
// 4 KB: it has to hold what a row writes while no master is reading — at
// boot, the banner plus the scans of a full row. Lines carry the board's own
// uptime in seconds (#503), not a wall clock (the ESP-01's own clock is often
// unset); the stamp costs up to 13 bytes a line out of that budget.

#ifndef FOLLOWER_LOG_SIZE
#define FOLLOWER_LOG_SIZE 4096
#endif

// Byte ring with a monotonic write cursor, so a reader takes only the bytes
// it has not had yet. Pure logic — no Print/Wire dependency — so it is
// exercised host-side (test_follower_log).
struct FollowerLogRing {
  char buf[FOLLOWER_LOG_SIZE];
  size_t head = 0;         // next write position
  bool wrapped = false;    // has the ring filled at least once
  uint32_t written = 0;    // total bytes ever appended (monotonic cursor)

  void append(const char* data, size_t len) {
    if (data == nullptr) return;
    for (size_t i = 0; i < len; i++) {
      buf[head++] = data[i];
      if (head >= FOLLOWER_LOG_SIZE) {
        head = 0;
        wrapped = true;
      }
      written++;
    }
  }

  // Append with an "[<uptime seconds>] " stamp opening every line (#503): the
  // ring is read long after the fact, and without it a line cannot be placed
  // against a reset or a bus-death episode.
  bool atLineStart = true;
  void appendStamped(const char* data, size_t len, uint32_t seconds) {
    if (data == nullptr) return;
    for (size_t i = 0; i < len; i++) {
      if (atLineStart) {
        char stamp[14];
        int n = snprintf(stamp, sizeof(stamp), "[%lu] ", (unsigned long)seconds);
        append(stamp, (size_t)n);
        atLineStart = false;
      }
      append(&data[i], 1);
      if (data[i] == '\n') atLineStart = true;
    }
  }

  // Bytes currently retained in the ring.
  size_t fill() const { return wrapped ? (size_t)FOLLOWER_LOG_SIZE : head; }

  // Oldest cursor value still recoverable from the ring.
  uint32_t oldestCursor() const { return written - (uint32_t)fill(); }

  // The next whole line at `cursor` into out, without its newline; `cursor`
  // moves past it. A cursor older than the ring holds starts at the oldest
  // byte; one past `written` (a reader that outlived a restart) at `written`.
  // A line longer than cap comes in pieces. False, with nothing changed,
  // while no whole line is waiting.
  bool nextLine(uint32_t& cursor, char* out, size_t cap, size_t& len) const {
    uint32_t start = cursor;
    if (start < oldestCursor()) start = oldestCursor();
    if (start > written) start = written;
    const size_t waiting = (size_t)(written - start);
    size_t idx = (head + (size_t)FOLLOWER_LOG_SIZE - waiting) % FOLLOWER_LOG_SIZE;
    size_t n = 0;
    for (size_t i = 0; i < waiting; i++) {
      const char c = buf[idx++];
      if (idx >= FOLLOWER_LOG_SIZE) idx = 0;
      if (c == '\n') {
        cursor = start + (uint32_t)i + 1;
        len = n;
        return true;
      }
      if (n == cap) {
        cursor = start + (uint32_t)i;
        len = n;
        return true;
      }
      out[n++] = c;
    }
    return false;
  }
};

// Print-derived sink so every type SerialPrint can format lands in the ring
// without a pile of overloads (mirrors the master's WebLogPrinter).
class FollowerLogPrinter : public Print {
 public:
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};

extern FollowerLogPrinter followerLogPrinter;

// The ring itself, for the reader that sends it up the link.
const FollowerLogRing& followerLogRing();
