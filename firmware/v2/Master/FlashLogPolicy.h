#pragma once
// FlashLogPolicy.h — pure staging/flush/rotation decisions for the
// persistent flash log (#206), natively tested by test_flash_log_policy.
// The glue (FlashLog.cpp) owns LittleFS, the mutex and the netTask tick;
// this header owns every number and every decision so the wear/latency
// tradeoffs are pinned by tests, not scattered through the glue.

#ifdef UNIT_TEST
  #include <cstdint>
  #include <cstddef>
  #include <cstdio>
  #include <cstring>
#else
  #include <Arduino.h>
#endif

// Staging ring capacity (largeAlloc'd, PSRAM-preferred). Producers append
// here; only the netTask tick touches the flash.
#define FLASH_LOG_STAGE_CAP 8192
// Flush cadence: at half-full (burst protection) or on this interval when
// anything is staged (bounded loss window on a crash).
#define FLASH_LOG_FLUSH_INTERVAL_MS 5000
// Per-file cap; two files (current + rotated) bound the partition use at
// 2 MB on the ~5.8 MB storage partition — always 1..2 MB of contiguous
// history (≈2–4 weeks in clock mode at ~70–140 KB/day, ≈1 week under a
// chatty bench session). Wear is a non-issue at these volumes.
#define FLASH_LOG_FILE_CAP (1024 * 1024)

struct FlashLogStageState {
  size_t used = 0;      // bytes currently staged
  uint32_t dropped = 0; // bytes lost since the last flush (marker line at flush)
};

// --- per-line timestamps -----------------------------------------------------------
// Every line gets a stamp prefix at stage time: wall clock once NTP has
// synced, milliseconds-since-boot before that (the useful axis for boot
// debugging). Sized for the longer boot form "[+00042.317] ".
#define LOG_STAMP_MAX 16

inline void logStampClock(char out[LOG_STAMP_MAX], unsigned h, unsigned m,
                          unsigned s) {
  snprintf(out, LOG_STAMP_MAX, "[%02u:%02u:%02u] ", h, m, s);
}

inline void logStampBoot(char out[LOG_STAMP_MAX], uint32_t millisSinceBoot) {
  snprintf(out, LOG_STAMP_MAX, "[+%05lu.%03lu] ",
           (unsigned long)(millisSinceBoot / 1000),
           (unsigned long)(millisSinceBoot % 1000));
}

// Copies `in` to `out`, inserting `stamp` before the first byte of every
// line; `atLineStart` carries the state across chunk boundaries (a chunk
// may end mid-line). Clips at outCap — a stamp is only emitted when the
// whole stamp fits, and `consumed` reports how many INPUT bytes made it,
// so the caller can count `len - consumed` as dropped (drop-newest keeps
// the oldest context, which is what post-mortems want).
struct LogLineStamper {
  bool atLineStart = true;
};

inline size_t logStamperApply(LogLineStamper& st, const char* stamp,
                              const char* in, size_t len, char* out,
                              size_t outCap, size_t& consumed) {
  size_t stampLen = strlen(stamp);
  size_t written = 0;
  consumed = 0;
  for (size_t i = 0; i < len; i++) {
    size_t need = (st.atLineStart ? stampLen : 0) + 1;
    if (written + need > outCap) break;
    if (st.atLineStart) {
      memcpy(out + written, stamp, stampLen);
      written += stampLen;
      st.atLineStart = false;
    }
    out[written++] = in[i];
    consumed++;
    if (in[i] == '\n') st.atLineStart = true;
  }
  return written;
}

inline bool flashLogShouldFlush(size_t used, size_t cap,
                                uint32_t msSinceFlush, uint32_t intervalMs) {
  if (used == 0) return false;  // never touch the flash for nothing
  if (used >= cap / 2) return true;
  return msSinceFlush >= intervalMs;
}

inline bool flashLogShouldRotate(size_t fileSize) {
  return fileSize >= FLASH_LOG_FILE_CAP;
}

// Day boundary markers: the flush writes a "===== YYYY-MM-DD =====" line
// whenever the NTP-synced local date changed since the last marker. Days
// are encoded yyyymmdd; nowDay == 0 means the clock isn't synced (no
// marker — early-boot lines sit under the previous day / boot marker).
inline bool flashLogDayMarkerDue(uint32_t lastMarkedDay, uint32_t nowDay) {
  if (nowDay == 0) return false;
  return nowDay != lastMarkedDay;
}
