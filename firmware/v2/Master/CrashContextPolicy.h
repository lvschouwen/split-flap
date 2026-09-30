#pragma once
// Crash context (#504): each task records the activity it is in right now, in
// RTC memory that survives watchdog, panic and brownout resets (not power
// loss). The boot after a crash reports what every task was doing, and for how
// long, when the core stopped — the evidence an interrupt-watchdog loop with
// no coredump otherwise destroys. Pure layout + policy here (natively tested);
// the RTC_NOINIT instance and reporting live in CrashContext.cpp.

#include <stdint.h>
#include <string.h>

#define CRASH_CTX_MAGIC 0x5F1A7C0DUL

enum CrashSlot : int {
  CRASH_SLOT_DISPLAY = 0,  // displayTask: every unit-bus transaction
  CRASH_SLOT_NET = 1,      // netTask: WiFi / web drain / flash flushes
  CRASH_SLOT_MQTT = 2,
  CRASH_SLOT_CLUSTER = 3,
  CRASH_CTX_SLOTS = 4,
};

enum CrashAct : uint8_t {
  CRASH_ACT_NONE = 0,
  CRASH_ACT_IDLE = 1,         // waiting for work
  CRASH_ACT_I2C_WRITE = 2,    // arg = unit address
  CRASH_ACT_I2C_READ = 3,     // arg = unit address
  CRASH_ACT_I2C_RECOVER = 4,  // Wire.end + re-init after a failed read
  CRASH_ACT_I2C_PROBE = 5,    // arg = address
  CRASH_ACT_FRAME = 6,        // driving a frame
  CRASH_ACT_WIFI = 7,
  CRASH_ACT_WEB_LOOP = 8,     // settings drain, reboot, flushes
  CRASH_ACT_MQTT = 9,
  CRASH_ACT_CLUSTER = 10,
};

// Mirrors the esp_reset_reason_t values the report cares about, so this
// header stays IDF-free for the native tests.
enum CrashReset : int {
  CRASH_RESET_POWERON = 1,
  CRASH_RESET_SW = 3,
  CRASH_RESET_PANIC = 4,
  CRASH_RESET_INT_WDT = 5,
  CRASH_RESET_TASK_WDT = 6,
  CRASH_RESET_WDT = 7,
  CRASH_RESET_BROWNOUT = 9,
};

struct CrashSlotState {
  uint8_t act = CRASH_ACT_NONE;
  uint8_t arg = 0;
  uint32_t sinceMs = 0;
};

struct CrashContext {
  uint32_t magic;
  uint32_t lastTickMs;  // advanced by a live task; "age" is relative to it
  CrashSlotState slot[CRASH_CTX_SLOTS];
};

inline void crashCtxArm(CrashContext& c) {
  memset(&c, 0, sizeof(c));
  c.magic = CRASH_CTX_MAGIC;
}

inline bool crashCtxValid(const CrashContext& c) {
  if (c.magic != CRASH_CTX_MAGIC) return false;
  for (int i = 0; i < CRASH_CTX_SLOTS; i++) {
    if (c.slot[i].act > CRASH_ACT_CLUSTER) return false;
  }
  return true;
}

// Re-marking the current activity keeps its start time, so a loop that marks
// every pass still reports how long it has been stuck.
inline void crashCtxSet(CrashContext& c, int slot, uint8_t act, uint8_t arg,
                        uint32_t nowMs) {
  if (slot < 0 || slot >= CRASH_CTX_SLOTS) return;
  CrashSlotState& s = c.slot[slot];
  if (s.act == act && s.arg == arg) return;
  s.act = act;
  s.arg = arg;
  s.sinceMs = nowMs;
}

inline void crashCtxTick(CrashContext& c, uint32_t nowMs) { c.lastTickMs = nowMs; }

inline uint32_t crashCtxAgeMs(const CrashContext& c, int slot) {
  if (slot < 0 || slot >= CRASH_CTX_SLOTS) return 0;
  return c.lastTickMs - c.slot[slot].sinceMs;
}

inline bool crashCtxWorthReporting(int resetReason) {
  switch (resetReason) {
    case CRASH_RESET_PANIC:
    case CRASH_RESET_INT_WDT:
    case CRASH_RESET_TASK_WDT:
    case CRASH_RESET_WDT:
    case CRASH_RESET_BROWNOUT:
      return true;
    default:
      return false;
  }
}

inline const char* crashSlotName(int slot) {
  switch (slot) {
    case CRASH_SLOT_DISPLAY: return "display";
    case CRASH_SLOT_NET: return "net";
    case CRASH_SLOT_MQTT: return "mqtt";
    case CRASH_SLOT_CLUSTER: return "cluster";
    default: return "?";
  }
}

inline const char* crashActName(uint8_t act) {
  switch (act) {
    case CRASH_ACT_NONE: return "none";
    case CRASH_ACT_IDLE: return "idle";
    case CRASH_ACT_I2C_WRITE: return "i2c-write";
    case CRASH_ACT_I2C_READ: return "i2c-read";
    case CRASH_ACT_I2C_RECOVER: return "i2c-recover";
    case CRASH_ACT_I2C_PROBE: return "i2c-probe";
    case CRASH_ACT_FRAME: return "frame";
    case CRASH_ACT_WIFI: return "wifi";
    case CRASH_ACT_WEB_LOOP: return "web-loop";
    case CRASH_ACT_MQTT: return "mqtt";
    case CRASH_ACT_CLUSTER: return "cluster";
    default: return "?";
  }
}
