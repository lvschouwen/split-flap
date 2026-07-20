#pragma once
// FollowerRescue.h — boot-rescue beacon policy (#343), natively tested by
// test_follower_rescue. The ESP-01 has no A/B slot and no serial rescue
// (the pins are the unit bus), so a crash-looping image bricks the row.
// Countermeasure: an RTC-memory boot counter increments at every boot and
// is zeroed only by (a) FOLLOWER_RESCUE_HEALTHY_MS of stable uptime or
// (b) the deliberate-reboot path (which the OTA-complete flow also takes) —
// so only unexpected early deaths accumulate. At the cap the next boot
// comes up as a minimal "rescue beacon": WiFi + cluster wire + OTA only,
// no I2C/render, advertising rescue:1 in the join/ping replies so the
// leader auto re-pushes the stored follower image (#343 leader side rides
// the #344 convergence machine). The flash's reboot clears the counter.
//
// RTC user memory survives crash/soft resets but NOT a power cycle — a
// power glitch never counts as a bad boot, by design.
//
// Pure encode/decode + thresholds here; the ESP.rtcUserMemory glue lives
// in FollowerRescue.cpp (decls at the bottom).

#include <stdint.h>

// Word-block offset into RTC user memory — clear of the low words some OTA
// tooling touches (v1 ESPMaster lesson: park at 32).
#define FOLLOWER_RESCUE_RTC_OFFSET 32

// Consecutive early deaths before the beacon engages.
#define FOLLOWER_RESCUE_BOOT_CAP 3

// Uptime that proves a boot healthy (loop() marks once past this).
#define FOLLOWER_RESCUE_HEALTHY_MS 60000UL

#define FOLLOWER_RESCUE_MAGIC 0x52435346UL  // "FSCR" LE

// 3 RTC words. check binds magic^counter so factory garbage (0xFF/0x00
// fills) and torn writes never decode as a real count.
struct FollowerRescueBlob {
  uint32_t magic;
  uint32_t counter;
  uint32_t check;
};

inline uint32_t followerRescueCheck(uint32_t counter) {
  return FOLLOWER_RESCUE_MAGIC ^ counter ^ 0xA5A5A5A5UL;
}

// Garbage/unset RTC decodes as 0 — a fresh chip is a healthy chip.
inline uint32_t followerRescueDecode(const FollowerRescueBlob& b) {
  if (b.magic != FOLLOWER_RESCUE_MAGIC) return 0;
  if (b.check != followerRescueCheck(b.counter)) return 0;
  return b.counter;
}

inline void followerRescueEncode(FollowerRescueBlob& b, uint32_t counter) {
  b.magic = FOLLOWER_RESCUE_MAGIC;
  b.counter = counter;
  b.check = followerRescueCheck(counter);
}

// Boot decision from the PREVIOUS boots' tally.
inline bool followerRescueBeaconAtBoot(uint32_t prevCounter) {
  return prevCounter >= FOLLOWER_RESCUE_BOOT_CAP;
}

// The value this boot persists (saturating — a long crash loop must never
// wrap back below the cap).
inline uint32_t followerRescueNextCounter(uint32_t prevCounter) {
  return prevCounter >= 0xFFFFFFFFUL ? prevCounter : prevCounter + 1;
}

#ifdef ARDUINO
// --- glue (FollowerRescue.cpp, ESP.rtcUserMemory) ---------------------------------
void rescueBootInit();      // read + tally + persist; FIRST thing in setup()
bool rescueActive();        // beacon mode this boot
void rescueMarkHealthy();   // zero the counter (idempotent per boot)
void rescueHealthyTick();   // loop(): auto-mark after FOLLOWER_RESCUE_HEALTHY_MS
#endif
