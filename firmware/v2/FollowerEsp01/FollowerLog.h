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

  // --- /log reader (#679): independent of nextLine's cursor ----------------
  // A stream is a snapshot [start, end) of the monotonic cursor taken when the
  // request arrives; every chunk is addressed by its byte offset into it, so
  // a retried chunk reads the same bytes and nothing here keeps state that the
  // wall link's cursor could share. `start` skips a first line the ring has
  // already cut into, so the text opens on a whole line.
  struct Snapshot {
    uint32_t start = 0;
    uint32_t end = 0;
  };

  Snapshot snapshot() const {
    Snapshot s;
    s.end = written;
    s.start = oldestCursor();
    if (s.start == 0) return s;  // nothing evicted: the first byte is a line start
    // After an eviction the byte before the oldest one is gone, so whether the
    // oldest byte opens a line cannot be decided: the first line is always
    // dropped, even when it happened to be whole.
    uint32_t c = s.start;
    for (size_t i = 0; i < fill() && c < s.end; i++, c++) {
      if (byteAt(c) == '\n') {
        s.start = c + 1;
        return s;
      }
    }
    s.start = s.end;  // the whole ring is one unfinished line
    return s;
  }

  // Up to cap bytes of the snapshot from `offset` on. Returns the count (0 at
  // the end, which is the end of the stream). If appends have since evicted
  // the bytes wanted, `overtaken` is set and 0 returned: the stream stops
  // there rather than send bytes that were overwritten.
  size_t readSnapshot(const Snapshot& s, size_t offset, char* out, size_t cap,
                      bool& overtaken) const {
    overtaken = false;
    const uint32_t total = s.end - s.start;
    if (offset >= total) return 0;
    const uint32_t cursor = s.start + (uint32_t)offset;
    if (cursor < oldestCursor()) {
      overtaken = true;
      return 0;
    }
    size_t n = (size_t)(total - offset);
    if (n > cap) n = cap;
    for (size_t i = 0; i < n; i++) out[i] = byteAt(cursor + (uint32_t)i);
    return n;
  }

 private:
  // The byte at a cursor value the ring still holds.
  char byteAt(uint32_t cursor) const {
    const size_t back = (size_t)(written - cursor);
    return buf[(head + (size_t)FOLLOWER_LOG_SIZE - back) % FOLLOWER_LOG_SIZE];
  }
};

// What a stream the ring overtook ends with, so a truncated body shows it.
#define FOLLOWER_LOG_OVERTAKEN_MARK "\n[log overtaken]\n"

// One /log stream's progress past the snapshot: where the ring overtook it.
struct FollowerLogStream {
  bool overtaken = false;
  size_t at = 0;  // stream offset the marker starts at
};

// The next piece of a stream at `offset`: the snapshot's bytes, then, if the
// ring overtook the reader, the marker once; 0 ends the stream. Addressed by
// offset like readSnapshot, so a retried chunk repeats itself.
inline size_t followerLogStreamRead(const FollowerLogRing& ring,
                                    const FollowerLogRing::Snapshot& snap,
                                    FollowerLogStream& st, size_t offset,
                                    char* out, size_t cap) {
  if (!st.overtaken) {
    bool over = false;
    const size_t n = ring.readSnapshot(snap, offset, out, cap, over);
    if (!over) return n;
    st.overtaken = true;
    st.at = offset;
  }
  const size_t markLen = sizeof(FOLLOWER_LOG_OVERTAKEN_MARK) - 1;
  const size_t done = offset - st.at;
  if (done >= markLen) return 0;
  size_t n = markLen - done;
  if (n > cap) n = cap;
  for (size_t i = 0; i < n; i++) out[i] = FOLLOWER_LOG_OVERTAKEN_MARK[done + i];
  return n;
}

// Below this largest free block GET /log answers 503 (#679). 4096 is where
// FollowerEscalation.h starts its low-heap clock toward a self-restart, so the
// stream must never be what takes the row there; the 2048 on top covers the
// response object, the chunk buffer and the TCP segment it is copied into.
// A field row has ~29 KB free, so this refuses only a row already in trouble.
#define FOLLOWER_LOG_SERVE_MIN_BLOCK 6144U
// Bytes handed to the web library per chunk.
#define FOLLOWER_LOG_CHUNK 256U

inline bool followerLogServeAllowed(uint32_t largestFreeBlock) {
  return largestFreeBlock >= FOLLOWER_LOG_SERVE_MIN_BLOCK;
}

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
