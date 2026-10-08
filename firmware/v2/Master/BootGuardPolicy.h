#pragma once
// Boot guard (#281): counts the crashes in a row that no healthy run came
// between. At the limit the board starts its rescue image instead of the
// image that keeps crashing — the bootloader's own rollback only covers an
// image that was never confirmed, and a confirmed one that crashes every few
// seconds is otherwise stuck until someone holds GPIO 4. Pure rules + the
// record here (natively tested); the RTC_NOINIT instance, the arming and the
// report live in BootGuard.cpp.

#include <stdint.h>

#include <type_traits>

#include "CrashContextPolicy.h"  // CrashReset

#define BOOT_GUARD_MAGIC 0x44524742UL  // "BGRD" LE
#define BOOT_GUARD_CRASH_LIMIT 3
// Longer than the 30 s task watchdog: a boot that hangs must be reset, and
// counted, before it could be called healthy. It is also the guard's reach:
// an image that runs this long between crashes is never sent to rescue — it
// is up most of the time and can be replaced over the network.
#define BOOT_GUARD_HEALTHY_MS 60000UL

// The count lives in RAM that a reset keeps and a power cycle does not, so
// pulling the plug always gives the image a full set of attempts — and that
// RAM holds garbage after power-on, which is why the count travels with a
// magic and its own complement. No constructor, no default member
// initializers: static init would wipe the record at every boot.
struct BootGuardRecord {
  uint32_t magic;
  uint8_t crashes;
  uint8_t check;  // ~crashes
};

static_assert(std::is_trivially_default_constructible<BootGuardRecord>::value,
              "static init must not touch the RTC_NOINIT boot guard record");

inline void bootGuardEncode(BootGuardRecord& r, uint8_t crashes) {
  r.magic = BOOT_GUARD_MAGIC;
  r.crashes = crashes;
  r.check = (uint8_t)~crashes;
}

// Anything that is not a record this code wrote decodes as no crashes.
inline uint8_t bootGuardDecode(const BootGuardRecord& r) {
  if (r.magic != BOOT_GUARD_MAGIC) return 0;
  if (r.check != (uint8_t)~r.crashes) return 0;
  return r.crashes;
}

// Only a reset the firmware itself caused by failing. A brownout is the
// supply, which the rescue image cannot fix, and a restart on purpose says
// nothing about the image.
inline bool bootGuardCountsAsCrash(int resetReason) {
  switch (resetReason) {
    case CRASH_RESET_PANIC:
    case CRASH_RESET_INT_WDT:
    case CRASH_RESET_TASK_WDT:
    case CRASH_RESET_WDT:
    case CRASH_RESET_CPU_LOCKUP:
      return true;
    default:
      return false;
  }
}

// The count after the reset this boot started from. Resets that are not a
// crash leave it alone: only a healthy run (bootGuardHealthy) or a power-on
// forgives.
inline uint8_t bootGuardStep(uint8_t crashes, int resetReason) {
  if (resetReason == CRASH_RESET_POWERON) return 0;
  if (!bootGuardCountsAsCrash(resetReason)) return crashes;
  return crashes == UINT8_MAX ? crashes : (uint8_t)(crashes + 1);
}

inline bool bootGuardShouldTrip(uint8_t crashes) {
  return crashes >= BOOT_GUARD_CRASH_LIMIT;
}

inline bool bootGuardHealthy(uint32_t uptimeMs) {
  return uptimeMs >= BOOT_GUARD_HEALTHY_MS;
}
