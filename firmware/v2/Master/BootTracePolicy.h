#pragma once
// Boot-stage trace (#504): a small ring, one entry per boot, recording the
// reset reason the boot started from and the furthest start-up stage it
// reached. Persisted in NVS at every stage, so it survives soft resets,
// brownouts AND the manual power cycle that ends an outage — the boot after
// it reports where the failed ones stopped. Pure policy + blob codec here
// (natively tested); NVS glue in BootTrace.cpp.

#include <stdint.h>
#include <string.h>

#define BOOT_TRACE_RING 8
#define BOOT_TRACE_MAGIC 0xB7
#define BOOT_TRACE_BLOB_LEN (2 + 2 * BOOT_TRACE_RING)

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
}

inline bool bootTraceDecode(const uint8_t* blob, size_t len, BootTrace& out) {
  out = BootTrace{};
  if (len < BOOT_TRACE_BLOB_LEN) return false;
  if (blob[0] != BOOT_TRACE_MAGIC || blob[1] > BOOT_TRACE_RING) return false;
  BootTrace t;
  t.count = blob[1];
  for (int i = 0; i < t.count; i++) {
    t.e[i].resetReason = blob[2 + 2 * i];
    t.e[i].stage = blob[3 + 2 * i];
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
