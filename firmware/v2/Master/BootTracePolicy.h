#pragma once
// Boot-stage trace (#504): a small ring, one entry per boot, recording the
// reset reason the boot started from and the furthest start-up stage it
// reached. Persisted in NVS at every stage, so it survives soft resets,
// brownouts AND the manual power cycle that ends an outage — the boot after
// it reports where the failed ones stopped. Pure policy + blob codec here
// (natively tested); NVS glue in BootTrace.cpp.
//
// Last alive (#628): each entry also carries the minutes its boot was known
// to be alive, rewritten on a 10-min step, so a hang that ended in a power
// pull is bounded to within 10 min of its start.
//
// Blob layout (little-endian):
//   [0] magic 0xB7  [1] count  [2..17] 8 x {resetReason, stage}   (v1, 18 B)
//   [18] version (2)  [19..34] 8 x uint16 aliveMin                 (v2 tail)
// The v1 prefix is unchanged, so an 18-byte blob from older firmware still
// decodes, its entries reading alive = unknown.

#include <stdint.h>
#include <string.h>

#define BOOT_TRACE_RING 8
#define BOOT_TRACE_MAGIC 0xB7
#define BOOT_TRACE_BLOB_LEN_V1 (2 + 2 * BOOT_TRACE_RING)
#define BOOT_TRACE_VERSION 2
#define BOOT_TRACE_BLOB_LEN (BOOT_TRACE_BLOB_LEN_V1 + 1 + 2 * BOOT_TRACE_RING)

#define BOOT_TRACE_ALIVE_UNKNOWN 0xFFFF
#define BOOT_TRACE_ALIVE_STEP_MIN 10
// Largest multiple of the step below the sentinel: about 45 days.
#define BOOT_TRACE_ALIVE_MAX \
  ((BOOT_TRACE_ALIVE_UNKNOWN - 1) / BOOT_TRACE_ALIVE_STEP_MIN * BOOT_TRACE_ALIVE_STEP_MIN)

// Ordered: a later stage implies every earlier one.
enum BootStage : uint8_t {
  BOOT_STAGE_NONE = 0,
  BOOT_STAGE_SETUP = 1,      // top of setup()
  BOOT_STAGE_INIT_DONE = 2,  // single-threaded init survived, tasks next
  BOOT_STAGE_UNITS = 3,      // displayTask finished the boot unit probe
  BOOT_STAGE_JOIN = 4,       // WiFi join started
  BOOT_STAGE_ONLINE = 5,     // joined, web server up
};

struct BootTraceEntry {
  uint8_t resetReason = 0;  // esp_reset_reason_t
  uint8_t stage = BOOT_STAGE_NONE;
  // Minutes this boot was last known alive, on the 10-min step; a lower
  // bound for the boot that is still running.
  uint16_t aliveMin = BOOT_TRACE_ALIVE_UNKNOWN;
};

// Oldest first; e[count-1] is this boot once bootTraceBegin ran.
struct BootTrace {
  uint8_t count = 0;
  BootTraceEntry e[BOOT_TRACE_RING];
};

inline void bootTraceBegin(BootTrace& t, uint8_t resetReason) {
  if (t.count == BOOT_TRACE_RING) {
    memmove(&t.e[0], &t.e[1], sizeof(BootTraceEntry) * (BOOT_TRACE_RING - 1));
    t.count--;
  }
  t.e[t.count].resetReason = resetReason;
  t.e[t.count].stage = BOOT_STAGE_SETUP;
  t.e[t.count].aliveMin = 0;
  t.count++;
}

// Advances this boot's stage. True when it moved (the caller persists).
inline bool bootTraceMark(BootTrace& t, uint8_t stage) {
  if (t.count == 0) return false;
  BootTraceEntry& cur = t.e[t.count - 1];
  if (stage <= cur.stage) return false;
  cur.stage = stage;
  return true;
}

// This boot's uptime as the stored last-alive value: whole minutes, down to
// the 10-min step, saturating.
inline uint16_t bootTraceAliveStep(uint64_t uptimeMs) {
  uint64_t minutes = uptimeMs / 60000ULL;
  if (minutes >= BOOT_TRACE_ALIVE_MAX) return BOOT_TRACE_ALIVE_MAX;
  return (uint16_t)(minutes / BOOT_TRACE_ALIVE_STEP_MIN * BOOT_TRACE_ALIVE_STEP_MIN);
}

// Records this boot's last-alive step. True when it changed (the caller
// persists): so one write per step, none once saturated.
inline bool bootTraceAliveTick(BootTrace& t, uint64_t uptimeMs) {
  if (t.count == 0) return false;
  BootTraceEntry& cur = t.e[t.count - 1];
  uint16_t step = bootTraceAliveStep(uptimeMs);
  if (step == cur.aliveMin) return false;
  cur.aliveMin = step;
  return true;
}

// Consecutive boots right before this one that never came online.
inline int bootTraceFailedStreak(const BootTrace& t) {
  int n = 0;
  for (int i = (int)t.count - 2; i >= 0; i--) {
    if (t.e[i].stage >= BOOT_STAGE_ONLINE) break;
    n++;
  }
  return n;
}

inline void bootTraceEncode(const BootTrace& t, uint8_t out[BOOT_TRACE_BLOB_LEN]) {
  memset(out, 0, BOOT_TRACE_BLOB_LEN);
  out[0] = BOOT_TRACE_MAGIC;
  out[1] = t.count;
  for (int i = 0; i < t.count; i++) {
    out[2 + 2 * i] = t.e[i].resetReason;
    out[3 + 2 * i] = t.e[i].stage;
  }
  out[BOOT_TRACE_BLOB_LEN_V1] = BOOT_TRACE_VERSION;
  uint8_t* alive = out + BOOT_TRACE_BLOB_LEN_V1 + 1;
  for (int i = 0; i < t.count; i++) {
    alive[2 * i] = (uint8_t)(t.e[i].aliveMin & 0xFF);
    alive[2 * i + 1] = (uint8_t)(t.e[i].aliveMin >> 8);
  }
}

inline bool bootTraceDecode(const uint8_t* blob, size_t len, BootTrace& out) {
  out = BootTrace{};
  if (len < BOOT_TRACE_BLOB_LEN_V1) return false;
  if (blob[0] != BOOT_TRACE_MAGIC || blob[1] > BOOT_TRACE_RING) return false;
  BootTrace t;
  t.count = blob[1];
  for (int i = 0; i < t.count; i++) {
    t.e[i].resetReason = blob[2 + 2 * i];
    t.e[i].stage = blob[3 + 2 * i];
  }
  // A v1 blob has no tail: its entries keep alive = unknown.
  if (len >= BOOT_TRACE_BLOB_LEN && blob[BOOT_TRACE_BLOB_LEN_V1] >= BOOT_TRACE_VERSION) {
    const uint8_t* alive = blob + BOOT_TRACE_BLOB_LEN_V1 + 1;
    for (int i = 0; i < t.count; i++) {
      t.e[i].aliveMin = (uint16_t)(alive[2 * i] | (alive[2 * i + 1] << 8));
    }
  }
  out = t;
  return true;
}

inline const char* bootStageName(uint8_t stage) {
  switch (stage) {
    case BOOT_STAGE_SETUP: return "setup";
    case BOOT_STAGE_INIT_DONE: return "init-done";
    case BOOT_STAGE_UNITS: return "units";
    case BOOT_STAGE_JOIN: return "join";
    case BOOT_STAGE_ONLINE: return "online";
    default: return "?";
  }
}
