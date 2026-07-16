#pragma once

#include <Arduino.h>
#include <Print.h>

// In-RAM log ring for the ESP-01 follower (#318 E). The follower has NO
// serial console — GPIO1/3 are the unit bus (FollowerConfig.h), so this ring
// is the ONLY way to see what the row is doing. It is served at GET /log and,
// more usefully, pulled by the S3 leader into the fleet-wide log so the whole
// wall's activity lands in one place (/log/flash on the master).
//
// v1's ESP-01 web log was 2 KB (#133); we keep that budget. Unlike the
// master's ring the bytes are NOT timestamped here — the leader stamps each
// line on ingest, giving the fleet log one coherent clock (the ESP-01's own
// clock is SNTP-epoch-only and often unset).

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

  // Bytes currently retained in the ring.
  size_t fill() const { return wrapped ? (size_t)FOLLOWER_LOG_SIZE : head; }

  // Oldest cursor value still recoverable from the ring.
  uint32_t oldestCursor() const { return written - (uint32_t)fill(); }

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

// Read the delta since `after` into `out`; returns the next cursor.
uint32_t followerLogReadSince(uint32_t after, String& out);
