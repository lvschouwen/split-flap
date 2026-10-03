#pragma once

#include <stdio.h>
#include <Arduino.h>
#include <Print.h>

// In-RAM log ring for the ESP-01 follower (#318 E). The follower has NO
// serial console — GPIO1/3 are the unit bus (FollowerConfig.h), so this ring
// is the ONLY way to see what the row is doing. It is served at GET /log and,
// more usefully, pulled by the S3 leader into the fleet-wide log so the whole
// wall's activity lands in one place (/log/flash on the master).
//
// v1's ESP-01 web log was 2 KB (#133); we keep that budget. Lines carry the
// board's own uptime in seconds (#503), not a wall clock — the leader stamps
// each line on ingest, giving the fleet log one coherent clock (the ESP-01's
// own clock is SNTP-epoch-only and often unset). The stamp costs up to 13
// bytes a line out of that budget.

#ifndef FOLLOWER_LOG_SIZE
#define FOLLOWER_LOG_SIZE 2048
#endif

// Byte ring with a monotonic write cursor so the leader can fetch only the
// bytes it has not ingested yet (GET /log?after=<cursor>). Pure logic — no
// Print/Wire dependency — so it is exercised host-side (test_follower_log).
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
  // ring is pulled into the fleet log long after the fact, and without it a
  // line cannot be placed against a reset or a bus-death episode.
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

  // The same bytes readSince() appends, handed to `sink(data, len)` as at most
  // two contiguous spans straight out of the ring — no copy (#503/#519).
  // Returns the next cursor.
  template <typename Sink>
  uint32_t readSinceInto(uint32_t after, Sink&& sink) const {
    uint32_t start = after;
    if (start < oldestCursor()) start = oldestCursor();
    if (start > written) start = written;
    size_t count = (size_t)(written - start);
    size_t idx = (head + (size_t)FOLLOWER_LOG_SIZE - count) % FOLLOWER_LOG_SIZE;
    size_t first = count;
    if (idx + first > (size_t)FOLLOWER_LOG_SIZE) first = (size_t)FOLLOWER_LOG_SIZE - idx;
    if (first > 0) sink(buf + idx, first);
    if (count > first) sink(buf, count - first);
    return written;
  }

  // Bytes readSinceInto() would hand out for `after`.
  size_t countSince(uint32_t after) const {
    uint32_t start = after;
    if (start < oldestCursor()) start = oldestCursor();
    if (start > written) start = written;
    return (size_t)(written - start);
  }

  // Append the retained bytes with cursor >= `after` (clamped to what the ring
  // still holds), oldest first, to `out`; return the next cursor (== written).
  // A stale `after` past `written` — the leader outlived a follower reboot —
  // yields nothing and rewinds the leader to `written`, so a reboot can't
  // trigger a re-dump storm.
  uint32_t readSince(uint32_t after, String& out) const {
    uint32_t start = after;
    if (start < oldestCursor()) start = oldestCursor();
    if (start > written) start = written;
    size_t count = (size_t)(written - start);
    size_t idx = (head + (size_t)FOLLOWER_LOG_SIZE - count) % FOLLOWER_LOG_SIZE;
    for (size_t i = 0; i < count; i++) {
      out += buf[idx++];
      if (idx >= FOLLOWER_LOG_SIZE) idx = 0;
    }
    return written;
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

// The ring itself, for a reader that streams it (GET /log). Single-core
// superloop: a handler runs to completion between writers.
const FollowerLogRing& followerLogRing();
