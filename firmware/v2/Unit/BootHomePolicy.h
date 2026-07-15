#pragma once
// Pure boot-home decision logic (#309). To end the power-up brownout (root
// cause of #305) the unit no longer homes in setup(): it boots UNHOMED/blank,
// records bootMs, and loop() homes on the FIRST trigger. This header owns the
// two time-driven triggers —
//   3. no master contact within SELF_TIMEOUT -> address-staggered self-home
//   4. hard cap: a master IS seen but never commands a home -> self-home anyway
// Triggers 1 (SFP_CMD_HOME) and 2 (a letter command while unhomed -> home then
// show) are edge events handled directly in the .ino. Natively tested by
// test_boot_home; the loop()/calibrate() glue is bench tier.
//
// The constants are STARTING POINTS — bench-tuned against the #306 vmin
// telemetry (home with different values, watch the rail floor). See spec
// docs/superpowers/specs/2026-07-15-unit-boot-home-stagger-and-i2c-heartbeat-health-design.md
// §1.

#include <stdint.h>

// No master contact within this window -> the unit is standalone; self-home.
#define BOOT_HOME_SELF_TIMEOUT_MS  30000UL
// Per-address spread so a shared rail doesn't see every unit home at once:
// deadline = SELF_TIMEOUT + dipIndex*STAGGER + jitter.
#define BOOT_HOME_STAGGER_MS       600UL
// Jitter ceiling (0..this ms inclusive) — de-synchronizes coincident timing so
// two rows brought up together don't re-lock into the same schedule.
#define BOOT_HOME_JITTER_MAX_MS    250UL
// A master IS present (it contacted us) but never commanded a home: home anyway
// so the row is never permanently dark. The only case that overrides
// "wait for the master" (probe-only / old master that skips batched HOME).
#define BOOT_HOME_HARD_CAP_MS      120000UL

// Inputs loop() gathers each pass. dipIndex is (i2cAddress - SFP_I2C_ADDRESS_BASE),
// 0-based; jitterMs is a per-unit 0..BOOT_HOME_JITTER_MAX_MS value seeded once
// at boot. elapsedMs is millis() since boot (bootMs is ~0 on the AVR).
struct BootHomeInputs {
  bool homed;                 // already homed since boot -> never self-home
  bool masterEverContacted;   // any I2C receive seen since boot
  uint32_t elapsedMs;         // ms since boot
  uint8_t dipIndex;           // 0-based address index for the stagger
  uint16_t jitterMs;          // 0..BOOT_HOME_JITTER_MAX_MS
};

// The staggered self-home deadline (no-master case), ms since boot.
inline uint32_t bootHomeSelfHomeDeadline(uint8_t dipIndex, uint16_t jitterMs) {
  return (uint32_t)BOOT_HOME_SELF_TIMEOUT_MS
       + (uint32_t)dipIndex * (uint32_t)BOOT_HOME_STAGGER_MS
       + (uint32_t)jitterMs;
}

// true -> loop() should self-home now (a single calibrate(true)). A unit that
// has already homed never self-homes; a unit that has seen a master waits the
// full hard cap (the master owns the homing until then); a standalone unit
// homes on its staggered deadline.
inline bool bootHomeShouldSelfHome(const BootHomeInputs& in) {
  if (in.homed) return false;
  if (in.masterEverContacted) {
    return in.elapsedMs >= (uint32_t)BOOT_HOME_HARD_CAP_MS;
  }
  return in.elapsedMs >= bootHomeSelfHomeDeadline(in.dipIndex, in.jitterMs);
}
