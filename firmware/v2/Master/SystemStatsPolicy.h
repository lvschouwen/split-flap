#pragma once
// SystemStatsPolicy.h — pure logic for the System tab (#245): the S3-vitals
// sample ring, CPU-load math and the /system/stats JSON. Natively tested by
// test_system_stats; the netTask sampler glue lives in SystemStats.cpp.
//
// Dual-rate (#251): the sampler ticks every SYSTEM_STATS_FAST_INTERVAL_S and
// every sample refreshes `latest` (the JSON `now` block — live tiles), but
// only every SYSTEM_STATS_DECIMATION-th sample lands in the history ring, so
// the sparklines keep ~10 min of SYSTEM_STATS_INTERVAL_S samples
// (server-side memory — a freshly opened tab shows history immediately).
// One-shot facts (uptime, counters, reset reason) ride the `now` object.

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define SYSTEM_STATS_RING            120  // history samples
#define SYSTEM_STATS_INTERVAL_S      5    // history cadence (JSON "interval")
#define SYSTEM_STATS_FAST_INTERVAL_S 1    // sampler tick / `now` freshness
#define SYSTEM_STATS_DECIMATION \
  (SYSTEM_STATS_INTERVAL_S / SYSTEM_STATS_FAST_INTERVAL_S)

struct SystemSample {
  int16_t  rssi = 0;       // dBm
  uint32_t freeHeap = 0;   // bytes
  uint32_t maxAlloc = 0;   // largest free internal-heap block
  uint32_t psramFree = 0;  // bytes
  uint8_t  cpu0 = 0;       // load %, core 0 (network)
  uint8_t  cpu1 = 0;       // load %, core 1 (display)
  int16_t  tempC10 = 0;    // die temperature x10 (label it die temp, not ambient)
};

struct SystemStatsRing {
  SystemSample samples[SYSTEM_STATS_RING];
  uint16_t count = 0;  // valid samples (saturates at SYSTEM_STATS_RING)
  uint16_t next = 0;   // write index
};

inline void systemStatsPush(SystemStatsRing& r, const SystemSample& s) {
  r.samples[r.next] = s;
  r.next = (uint16_t)((r.next + 1) % SYSTEM_STATS_RING);
  if (r.count < SYSTEM_STATS_RING) r.count++;
}

// i = 0 is the OLDEST sample, i = count-1 the newest.
inline const SystemSample* systemStatsAt(const SystemStatsRing& r, uint16_t i) {
  uint16_t start = (uint16_t)((r.next + SYSTEM_STATS_RING - r.count) %
                              SYSTEM_STATS_RING);
  return &r.samples[(start + i) % SYSTEM_STATS_RING];
}

// The sampler's whole state: decimated history + the newest fast sample.
struct SystemStatsSampler {
  SystemStatsRing ring;
  SystemSample latest;
  uint8_t decim = 0;  // 0 = the next intake also lands in the ring
};

// Fast-tick intake (#251): `latest` always refreshes; the ring takes the
// first sample (so boot has a history point) and every DECIMATION-th after.
inline void systemStatsIntake(SystemStatsSampler& s, const SystemSample& smp) {
  s.latest = smp;
  if (s.decim == 0) systemStatsPush(s.ring, smp);
  s.decim = (uint8_t)((s.decim + 1) % SYSTEM_STATS_DECIMATION);
}

// Load % of one core over a sample window, from FreeRTOS run-time-stats
// counter deltas (idle task runtime vs total runtime). Deltas are computed
// by the caller with plain unsigned subtraction — wrap-safe.
inline uint8_t cpuLoadPercent(uint32_t idleDelta, uint32_t totalDelta) {
  if (totalDelta == 0) return 0;
  if (idleDelta >= totalDelta) return 0;
  return (uint8_t)(100 - (idleDelta * 100ULL) / totalDelta);
}

// One-shot facts sampled at request/tick time, not ring-worthy.
struct SystemNow {
  uint32_t uptimeS = 0;
  uint32_t minFreeHeap = 0;  // lifetime low-water mark
  uint32_t i2cTx = 0;        // unit-bus transactions since boot
  uint32_t i2cErr = 0;       // failed transactions since boot
  uint32_t mqttDrops = 0;    // broker disconnects since boot
  int32_t  ntpAgeS = -1;     // seconds since last SNTP sync; -1 = never
  char resetReason[24] = {0};  // webResetReasonString()'s longest + NUL
  // #415: per-task stack low-water marks, BYTES still free at the worst
  // point since boot (portSTACK_TYPE is uint8_t on Xtensa) — the trim-down
  // evidence for the *_TASK_STACK sizes, previously trapped on USB-CDC.
  uint32_t hwmDisplay = 0;
  uint32_t hwmClock = 0;
  uint32_t hwmNet = 0;
  uint32_t hwmMqtt = 0;
  uint32_t hwmCluster = 0;
};

// Worst case measured by test_system_stats' saturated-ring test; headroom
// pinned there. Heap-allocated per request like /units/health.
#define SYSTEM_STATS_JSON_CAP 6144

#define SYSTEM_STATS_APPEND(...) do { \
    if (o >= cap) return o; \
    o += (size_t)snprintf(buf + o, cap - o, __VA_ARGS__); \
  } while (0)

// Emits just the "now" object {...} — the current vitals, no history. Shared
// by buildSystemStatsJson below and /status's one-shot aggregate (#307) so
// the two can never disagree on the field set. `newest` is the FAST sample;
// temp is x10 (the UI divides). Returns the would-be length like snprintf.
inline size_t buildSystemNowJson(char* buf, size_t cap,
                                 const SystemSample& newest,
                                 const SystemNow& now) {
  size_t o = 0;
  SYSTEM_STATS_APPEND(
      "{\"rssi\":%d,\"heap\":%lu,\"maxAlloc\":%lu,\"psram\":%lu,"
      "\"cpu0\":%u,\"cpu1\":%u,\"temp\":%d,\"uptime\":%lu,\"minHeap\":%lu,"
      "\"i2cTx\":%lu,\"i2cErr\":%lu,\"mqttDrops\":%lu,\"ntpAge\":%ld,"
      "\"reset\":\"%s\",\"hwm\":{\"display\":%lu,\"clock\":%lu,\"net\":%lu,"
      "\"mqtt\":%lu,\"cluster\":%lu}}",
      (int)newest.rssi, (unsigned long)newest.freeHeap,
      (unsigned long)newest.maxAlloc, (unsigned long)newest.psramFree,
      (unsigned)newest.cpu0, (unsigned)newest.cpu1, (int)newest.tempC10,
      (unsigned long)now.uptimeS, (unsigned long)now.minFreeHeap,
      (unsigned long)now.i2cTx, (unsigned long)now.i2cErr,
      (unsigned long)now.mqttDrops, (long)now.ntpAgeS, now.resetReason,
      (unsigned long)now.hwmDisplay, (unsigned long)now.hwmClock,
      (unsigned long)now.hwmNet, (unsigned long)now.hwmMqtt,
      (unsigned long)now.hwmCluster);
  return o;
}

// {"now":{...},"hist":{"rssi":[...],"heap":[...],"cpu0":[...],"cpu1":[...],
//  "temp":[...]},"interval":5}
// `now`'s spark fields mirror the newest FAST sample (#251), not the ring;
// temp is x10 (the UI divides). Returns the would-be length like snprintf;
// callers reject >= cap.
inline size_t buildSystemStatsJson(char* buf, size_t cap,
                                   const SystemStatsSampler& smp,
                                   const SystemNow& now) {
  size_t o = 0;
  const SystemStatsRing& r = smp.ring;
  const SystemSample* newest = &smp.latest;

  SYSTEM_STATS_APPEND("{\"now\":");
  if (o < cap) o += buildSystemNowJson(buf + o, cap - o, *newest, now);
  SYSTEM_STATS_APPEND(",\"hist\":{");

  // Series emitters: oldest-first arrays for the sparklines.
  SYSTEM_STATS_APPEND("\"rssi\":[");
  for (uint16_t i = 0; i < r.count; i++) {
    SYSTEM_STATS_APPEND("%s%d", i == 0 ? "" : ",",
                        (int)systemStatsAt(r, i)->rssi);
  }
  SYSTEM_STATS_APPEND("],\"heap\":[");
  for (uint16_t i = 0; i < r.count; i++) {
    SYSTEM_STATS_APPEND("%s%lu", i == 0 ? "" : ",",
                        (unsigned long)systemStatsAt(r, i)->freeHeap);
  }
  SYSTEM_STATS_APPEND("],\"cpu0\":[");
  for (uint16_t i = 0; i < r.count; i++) {
    SYSTEM_STATS_APPEND("%s%u", i == 0 ? "" : ",",
                        (unsigned)systemStatsAt(r, i)->cpu0);
  }
  SYSTEM_STATS_APPEND("],\"cpu1\":[");
  for (uint16_t i = 0; i < r.count; i++) {
    SYSTEM_STATS_APPEND("%s%u", i == 0 ? "" : ",",
                        (unsigned)systemStatsAt(r, i)->cpu1);
  }
  SYSTEM_STATS_APPEND("],\"temp\":[");
  for (uint16_t i = 0; i < r.count; i++) {
    SYSTEM_STATS_APPEND("%s%d", i == 0 ? "" : ",",
                        (int)systemStatsAt(r, i)->tempC10);
  }
  SYSTEM_STATS_APPEND("]},\"interval\":%d}", SYSTEM_STATS_INTERVAL_S);
  return o;
}
