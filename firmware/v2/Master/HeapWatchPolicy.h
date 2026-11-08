#pragma once
// Low-heap watch (#504): the master restarts itself when its internal heap
// stays below a floor for minutes — an allocation that fails in lwIP, a TLS
// session or a web handler is otherwise a hang or a crash at a random place.
// The S3 twin of the ESP-01 row's FollowerEscalation.h (#503), on the
// internal free heap (PSRAM holds the large buffers and is not watched).
//
//   internal free heap below HEAP_WATCH_LOW_BYTES: logged after 1 min,
//   restarted after HEAP_WATCH_RESTART_MS, never under a job a restart would
//   cut in half (an upload, a unit update, a release install) — the restart
//   waits for it — and at most once per HEAP_WATCH_MIN_INTERVAL_MIN of
//   uptime: an image short of heap at every start is not restarted in a loop
//   (each start re-homes the units, which is the brownout risk).
//
// The restart goes through the web drain's graceful reboot, whose cause is
// stamped to NVS and given by the next boot as lastRebootCause; the count
// lives in an RTC_NOINIT record (survives the restart, not a power cycle).
// Pure rules here (natively tested by test_heap_watch); glue HeapWatch.cpp.

#include <stdint.h>

#include <type_traits>

// Measured on the leader (GET /api/v2/board/<id> .stats.now, 10.6 h up):
// `heap` (internal free) 49.7 KB flat across the history, `minHeap` (the
// since-boot low-water) 16.6 KB — a transient it recovered from. The floor
// sits below that low-water, so the deepest dip seen in normal running
// never starts the clock, yet above the few KB lwIP and the WiFi driver need
// to keep the board reachable long enough to restart cleanly.
#define HEAP_WATCH_LOW_BYTES (12U * 1024U)
#define HEAP_WATCH_LOG_MS (60UL * 1000UL)
// Longer than any transient: a release download holds its TLS session for
// the whole fetch (a minute or two), an OTA upload its buffers likewise; the
// row's escalation waits the same five minutes.
#define HEAP_WATCH_RESTART_MS (5UL * 60UL * 1000UL)
#define HEAP_WATCH_MIN_INTERVAL_MIN (6U * 60U)

#define HEAP_WATCH_MAGIC 0x50414548UL  // "HEAP" LE
#define HEAP_WATCH_CHECK_MASK 0x5AC35AC3UL

// RTC_NOINIT: no constructor, no default member initializers, or static init
// wipes it at every boot. Garbage after power-on fails the check.
struct HeapWatchRecord {
  uint32_t magic;
  uint32_t minutesSince;  // uptime since the last low-heap restart, saturating
  uint32_t restarts;      // low-heap restarts since power-on
  uint32_t check;
};

static_assert(std::is_trivially_default_constructible<HeapWatchRecord>::value,
              "static init must not touch the RTC_NOINIT heap watch record");

inline uint32_t heapWatchRecordCheck(const HeapWatchRecord& r) {
  return r.magic ^ r.minutesSince ^ r.restarts ^ HEAP_WATCH_CHECK_MASK;
}

inline bool heapWatchRecordValid(const HeapWatchRecord& r) {
  return r.magic == HEAP_WATCH_MAGIC && r.check == heapWatchRecordCheck(r);
}

inline void heapWatchRecordSeal(HeapWatchRecord& r) {
  r.magic = HEAP_WATCH_MAGIC;
  r.check = heapWatchRecordCheck(r);
}

// setup(): anything this code did not write is a fresh record.
inline void heapWatchRecordBoot(HeapWatchRecord& r) {
  if (heapWatchRecordValid(r)) return;
  r.minutesSince = 0;
  r.restarts = 0;
  heapWatchRecordSeal(r);
}

inline uint32_t heapWatchRestarts(const HeapWatchRecord& r) {
  return heapWatchRecordValid(r) ? r.restarts : 0;
}

// May the board restart itself for low heap now? None taken since power-on:
// yes; otherwise only once the interval has run since the last.
inline bool heapWatchAllowed(const HeapWatchRecord& r) {
  if (heapWatchRestarts(r) == 0) return true;
  return r.minutesSince >= HEAP_WATCH_MIN_INTERVAL_MIN;
}

inline void heapWatchRecordMinute(HeapWatchRecord& r) {
  if (!heapWatchRecordValid(r)) heapWatchRecordBoot(r);
  if (r.minutesSince < 0xFFFFFFFFUL) r.minutesSince++;
  heapWatchRecordSeal(r);
}

inline void heapWatchRecordTaken(HeapWatchRecord& r) {
  if (!heapWatchRecordValid(r)) heapWatchRecordBoot(r);
  if (r.restarts < 0xFFFFFFFFUL) r.restarts++;
  r.minutesSince = 0;
  heapWatchRecordSeal(r);
}

// --- the decision ---------------------------------------------------------------

struct HeapWatchInput {
  uint32_t nowMs = 0;
  uint32_t freeInternal = 0xFFFFFFFFUL;
  bool restartBlocked = false;  // a job a restart would cut in half runs
  bool restartAllowed = true;   // heapWatchAllowed()
};

struct HeapWatchState {
  bool low = false;
  uint32_t lowSinceMs = 0;
  uint32_t lowest = 0;  // within this episode
  bool logged = false;
  bool withheldLogged = false;
};

struct HeapWatchVerdict {
  bool logLow = false;        // once per episode, after HEAP_WATCH_LOG_MS
  bool logRecovered = false;  // a logged episode ended
  bool logWithheld = false;   // due, but blocked or rate-limited: said once
  bool restart = false;
};

// About once a second.
inline HeapWatchVerdict heapWatchStep(HeapWatchState& st, const HeapWatchInput& in) {
  HeapWatchVerdict v;
  if (in.freeInternal >= HEAP_WATCH_LOW_BYTES) {
    v.logRecovered = st.logged;
    st = HeapWatchState{};
    return v;
  }
  if (!st.low) {
    st.low = true;
    st.lowSinceMs = in.nowMs;
    st.lowest = in.freeInternal;
  }
  if (in.freeInternal < st.lowest) st.lowest = in.freeInternal;
  const uint32_t lowFor = in.nowMs - st.lowSinceMs;
  if (!st.logged && lowFor >= HEAP_WATCH_LOG_MS) {
    st.logged = true;
    v.logLow = true;
  }
  if (lowFor < HEAP_WATCH_RESTART_MS) return v;
  if (!in.restartBlocked && in.restartAllowed) {
    v.restart = true;
    return v;
  }
  if (!st.withheldLogged) {
    st.withheldLogged = true;
    v.logWithheld = true;
  }
  return v;
}
