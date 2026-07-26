#pragma once
// Pure sub-frame render stagger (#324). A bulk render commands every unit its
// new flap target back-to-back; a full 16-unit row spinning its steppers up at
// once spiked the shared rail hard enough to brown out the S3 mid-takeover (the
// #305/#309 inrush family, but RUNTIME on a render, not boot). The render loop
// pauses for a rail-settle delay before opening each new group of units,
// spreading the stepper inrush transients. This header is the natively-tested
// batch decision; the Wire glue + the actual delay stay in UnitBus.cpp /
// FollowerBus.cpp.
//
// SHARED header: copied verbatim into firmware/v2/FollowerEsp01 (copy policy —
// the ESP-01 dumb row drives a full 16-unit row with identical physics; fix
// bugs in both trees). Constants are STARTING POINTS, bench-tuned against the
// #306 vmin telemetry — grow the batch once the rail proves it holds a larger
// simultaneous inrush.

// Units commanded per group before a rail-settle pause.
#define RENDER_STAGGER_BATCH     4
// Rail-settle delay inserted between groups (ms).
#define RENDER_STAGGER_SETTLE_MS 100UL

// Placed in the render loop BEFORE writing the next unit: true means "pause for
// a rail-settle, the current group is full". Counts only units actually
// commanded (absent/skipped slots draw no inrush, so callers must not advance
// the count for them) — so a row of N present units yields exactly
// ceil(N/batch)-1 pauses with no trailing settle after the final unit. batch<1
// falls back to 1 (settle between every unit) rather than dividing by zero.
inline bool renderStaggerShouldSettle(int commandedSoFar, int batch) {
  if (batch < 1) batch = 1;
  if (commandedSoFar <= 0) return false;
  return (commandedSoFar % batch) == 0;
}
