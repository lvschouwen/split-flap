#pragma once
// BootInfo.h — pure half of the read-only unit boot report (#499): what a unit
// says about its own boot section through GET_BOOT_INFO — the classified
// state, a CRC32 over the section computed by the unit's sketch, the lock and
// fuse bytes, and the result of its last update attempt. Nothing is written
// and the unit does not restart, so it can be asked at any time, on either
// row; both row masters now also have BootDump.h (#522) for the full
// boot-section dump.
//
// Shared by both row masters (both have the dump too, #522); the Nano never
// compiles it.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "BootUpdateReport.h"

// The ESP-01 keeps string literals in RAM, and that board runs with a few KB
// of free heap: its format strings go to flash instead.
#if defined(ESP8266)
#include <pgmspace.h>
#define BOOT_INFO_SNPRINTF(buf, cap, fmt, ...) \
  snprintf_P(buf, cap, PSTR(fmt), ##__VA_ARGS__)
#else
#define BOOT_INFO_SNPRINTF(buf, cap, fmt, ...) \
  snprintf(buf, cap, fmt, ##__VA_ARGS__)
#endif

// Longest reply: the ok line, ~130 characters.
#define BOOT_INFO_JSON_CAP 176

struct BootInfoSlot {
  uint32_t seq = 0;
  uint8_t addr = 0;
  bool done = false;  // false = slot default or a read still in flight
  bool ok = false;    // the report was read and passed its checksum
  BootUpdateReport report;
};

inline const char* bootInfoStateName(uint8_t state) {
  switch (state) {
    case BOOT_STATE_OLD:             return "old";
    case BOOT_STATE_PAGE7_INSTALLED: return "page7";
    case BOOT_STATE_TRAMPOLINE:      return "trampoline";
    case BOOT_STATE_NEW:             return "new";
    case BOOT_STATE_PREV_NEW:        return "prev-new";
    default:                         return "unknown";
  }
}

inline const char* bootInfoResultName(uint8_t result) {
  switch (result) {
    case BOOT_RESULT_NONE:          return "none";
    case BOOT_RESULT_STAGE1_OK:     return "stage1-ok";
    case BOOT_RESULT_STAGE2_OK:     return "stage2-ok";
    case BOOT_RESULT_REFUSED_STATE: return "refused-state";
    case BOOT_RESULT_REFUSED_LOCK:  return "refused-lock";
    case BOOT_RESULT_REFUSED_BUSY:  return "refused-busy";
    case BOOT_RESULT_VERIFY_FAILED: return "verify-failed";
    default:                        return "unknown";
  }
}

// pending / expired / failed / ok — the same seq ordering as the other
// single-slot op results. Never writes past `cap`; returns the string length.
inline size_t buildBootInfoJson(char* buf, size_t cap, const BootInfoSlot& slot,
                                uint32_t seq) {
  if (cap == 0) return 0;
  int n;
  if (slot.seq < seq || (slot.seq == seq && !slot.done)) {
    n = BOOT_INFO_SNPRINTF(buf, cap, "{\"state\":\"pending\"}");
  } else if (slot.seq > seq) {
    n = BOOT_INFO_SNPRINTF(buf, cap, "{\"state\":\"expired\"}");
  } else if (!slot.ok) {
    n = BOOT_INFO_SNPRINTF(buf, cap,
                 "{\"state\":\"failed\",\"addr\":%u,\"reason\":\"read-fail\"}",
                 (unsigned)slot.addr);
  } else if (!slot.report.lockFuseReadable) {
    // #518: say so instead of printing placeholder bytes as lock and fuses.
    const BootUpdateReport& r = slot.report;
    n = BOOT_INFO_SNPRINTF(buf, cap,
                 "{\"state\":\"ok\",\"addr\":%u,\"boot\":\"%s\","
                 "\"crc32\":\"%08lx\",\"lockfuse\":\"unreadable\","
                 "\"last\":\"%s\"}",
                 (unsigned)slot.addr, bootInfoStateName(r.state),
                 (unsigned long)r.bootCrc32, bootInfoResultName(r.lastResult));
  } else {
    const BootUpdateReport& r = slot.report;
    n = BOOT_INFO_SNPRINTF(buf, cap,
                 "{\"state\":\"ok\",\"addr\":%u,\"boot\":\"%s\","
                 "\"crc32\":\"%08lx\",\"lock\":\"%02x\",\"lfuse\":\"%02x\","
                 "\"hfuse\":\"%02x\",\"efuse\":\"%02x\",\"last\":\"%s\"}",
                 (unsigned)slot.addr, bootInfoStateName(r.state),
                 (unsigned long)r.bootCrc32, (unsigned)r.lockByte,
                 (unsigned)r.fuseLow, (unsigned)r.fuseHigh, (unsigned)r.fuseExt,
                 bootInfoResultName(r.lastResult));
  }
  if (n < 0) {
    buf[0] = '\0';
    return 0;
  }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}
