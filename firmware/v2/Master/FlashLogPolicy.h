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
// Floor between flushes asked for by a warning or an error line
// (flashLogTextUrgent). A flush is an open/append/close on LittleFS: a
// metadata commit, now and then a 4 KB sector erase, tens of ms of netTask
// time. One per second keeps a storm of failure lines at most five times
// the steady cadence and under a few percent of netTask, while the lines
// before a hang or a power cut lose at most one second instead of five.
#define FLASH_LOG_URGENT_MIN_MS 1000
// Per-file cap; two files (current + rotated) bound the partition use at
// 2 MB on the ~5.8 MB storage partition — always 1..2 MB of contiguous
// history (≈2–4 weeks in clock mode at ~70–140 KB/day, ≈1 week under a
// chatty bench session). Wear is a non-issue at these volumes.
#define FLASH_LOG_FILE_CAP (1024 * 1024)

struct FlashLogStageState {
  size_t used = 0;      // bytes currently staged
  uint32_t dropped = 0; // bytes lost since the last flush (marker line at flush)
  bool urgent = false;  // a warning or error is staged; cleared by the flush
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

// `lastFlushFailed`: the flash would not take the last flush. Urgency then
// stands down to the period, or the failure line each failed write logs
// would re-arm a flush every FLASH_LOG_URGENT_MIN_MS for as long as the
// fault lasts.
inline bool flashLogShouldFlush(size_t used, size_t cap,
                                uint32_t msSinceFlush, uint32_t intervalMs,
                                bool urgent = false,
                                bool lastFlushFailed = false) {
  if (used == 0) return false;  // never touch the flash for nothing
  if (used >= cap / 2) return true;
  if (urgent && !lastFlushFailed && msSinceFlush >= FLASH_LOG_URGENT_MIN_MS)
    return true;
  return msSinceFlush >= intervalMs;
}

// The log has no severity field: a warning or an error is recognised by the
// words its failure lines carry. A word counts only whole (no letter or digit
// on either side), so "install" is no "stall" and "failsafe" no "fail". A
// count is no failure: a word written as a field (`error=`) or right after a
// number ("0 of 3 failed offers") does not count. A false positive costs one
// early flush, bounded by FLASH_LOG_URGENT_MIN_MS. The words are written in
// lower case; the scan folds the text's ASCII case and stops at len (chunks
// are not NUL-terminated). tests/flash_log_urgency.txt holds the verdict of
// every log literal that carries one of these words.
static const char* const FLASH_LOG_URGENT_WORDS[] = {
    "fail",    "failed",   "failing", "failure",  "failures", "error",
    "fatal",   "crash",    "crashed", "crashes",  "panic",    "lost",
    "stall",   "stalled",  "abort",   "aborted",  "aborting", "jam",
    "timeout", "timed out",
};

inline bool flashLogIsWordChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}

inline bool flashLogWordAt(const char* data, size_t len, size_t i,
                           const char* word, size_t wl) {
  if (i + wl > len) return false;
  if (i > 0 && flashLogIsWordChar(data[i - 1])) return false;
  for (size_t k = 0; k < wl; k++) {
    char c = data[i + k];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != word[k]) return false;
  }
  return i + wl == len || !flashLogIsWordChar(data[i + wl]);
}

// A count: `word=`, or the word right after a number that stands alone
// ("3 failed"; not "0x3 lost", not "row1 failed").
inline bool flashLogWordIsCount(const char* data, size_t len, size_t i,
                                size_t wl) {
  if (i + wl < len && data[i + wl] == '=') return true;
  if (i < 2 || data[i - 1] != ' ') return false;
  size_t start = i - 1;
  while (start > 0 && data[start - 1] >= '0' && data[start - 1] <= '9') start--;
  if (start == i - 1) return false;  // no digits before the space
  return start == 0 || !flashLogIsWordChar(data[start - 1]);
}

inline bool flashLogTextUrgent(const char* data, size_t len) {
  if (data == nullptr) return false;
  for (const char* word : FLASH_LOG_URGENT_WORDS) {
    size_t wl = strlen(word);
    for (size_t i = 0; i + wl <= len; i++) {
      if (flashLogWordAt(data, len, i, word, wl) &&
          !flashLogWordIsCount(data, len, i, wl)) {
        return true;
      }
    }
  }
  return false;
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
