#pragma once
// Pure batched boot-home sequencing (#309, master side). The v1 units now boot
// UNHOMED (BootHomePolicy.h) instead of homing in setup(), so the master
// orchestrates homing in bounded batches after the boot probe — a whole row's
// steppers no longer spike the shared rail at once (the #305 verify-boot
// brownout). This header is the natively-tested target selection + batch math;
// the Wire glue (send HOME, wait for homed-or-faulted, rail-settle) stays in
// Tasks.cpp / UnitBus.cpp.
//
// SHARED header: copied verbatim into firmware/v2/FollowerEsp01 (copy policy —
// fix bugs in both trees). Constants are STARTING POINTS, bench-tuned against
// the #306 vmin telemetry. See spec
// docs/superpowers/specs/2026-07-15-unit-boot-home-stagger-and-i2c-heartbeat-health-design.md
// §1.

#include "UnitHealth.h"  // UnitFacts + UNIT_FLAG_HOMED

// Units homed per batch. START AT 1; grow to 2..4 only once bench vmin proves
// the larger batch keeps the rail above the brownout threshold.
#define BOOT_HOME_BATCH_SIZE       1
// Rail-settle delay between batches (spec: 300..1000 ms).
#define BOOT_HOME_SETTLE_MS        500UL
// Per-batch cap on the homed-or-faulted wait (status-driven, not a fixed sleep).
#define BOOT_HOME_BATCH_TIMEOUT_MS 8000UL

// Collects the bus addresses (base + column) of sketch-mode units that report
// NOT homed and so still need a HOME. Reading the homed bit lets the same
// sequence serve both a cold boot (every unit unhomed -> home all) and a
// post-reflash top-up (only the just-flashed units unhomed) without re-homing
// units that are already good. out[] must hold >= width entries; returns count.
inline int bootHomeCollectTargets(const UnitFacts* units, int width, int base,
                                  uint8_t* out) {
  int n = 0;
  for (int i = 0; i < width; i++) {
    const UnitFacts& u = units[i];
    if (u.state == 1 && u.statusValid &&
        !(u.status.flags & UNIT_FLAG_HOMED)) {
      out[n++] = (uint8_t)(base + i);
    }
  }
  return n;
}

// Number of batches n targets split into at the given batch size (>=1).
inline int bootHomeBatchCount(int nTargets, int batchSize) {
  if (batchSize < 1) batchSize = 1;
  if (nTargets <= 0) return 0;
  return (nTargets + batchSize - 1) / batchSize;
}
