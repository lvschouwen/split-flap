#pragma once
// Pure relative-wear assessment over the per-unit revolution odometers
// (#231) — natively tested by test_wear_policy, no Arduino deps.
//
// A unit is flagged when its odometer exceeds
//   max(WEAR_FLAG_RATIO x median, median + WEAR_FLAG_FLOOR_REVS)
// of the display's valid odometers. Relative, not absolute: nobody has real
// 28BYJ-48 flap-drum life data, but a unit wearing 2x faster than its peers
// is mechanically suspect regardless of where the cliff is. The absolute
// floor keeps young displays quiet (9 x a tiny median is still a tiny
// number). Median is the LOWER middle so on small displays the worn outlier
// can't set the median itself and escape its own flag.
//
// Consumed by WebEndpoints (buildWearJson spliced into /units/health, same
// pattern as the #205 reflash object) and mqttTask (wear-warning binary
// sensor).

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "UnitHealth.h"

#define WEAR_FLAG_RATIO      2
#define WEAR_FLAG_FLOOR_REVS 10000UL

struct WearAssessment {
  uint32_t median = 0;
  int validCount = 0;
  int flaggedCount = 0;
  bool flagged[UNITS_AMOUNT] = {false};
};

inline void assessWear(const UnitFacts* units, int n, WearAssessment& out) {
  out = WearAssessment{};
  if (n > UNITS_AMOUNT) n = UNITS_AMOUNT;

  uint32_t valid[UNITS_AMOUNT];
  for (int i = 0; i < n; i++) {
    if (units[i].odometerValid) valid[out.validCount++] = units[i].odometer;
  }
  if (out.validCount == 0) return;

  // Insertion sort — n <= 16, no library dependency.
  for (int i = 1; i < out.validCount; i++) {
    uint32_t v = valid[i];
    int j = i - 1;
    while (j >= 0 && valid[j] > v) { valid[j + 1] = valid[j]; j--; }
    valid[j + 1] = v;
  }
  out.median = valid[(out.validCount - 1) / 2];

  uint64_t threshold = (uint64_t)out.median * WEAR_FLAG_RATIO;
  uint64_t floorBound = (uint64_t)out.median + WEAR_FLAG_FLOOR_REVS;
  if (floorBound > threshold) threshold = floorBound;
  for (int i = 0; i < n; i++) {
    if (units[i].odometerValid && (uint64_t)units[i].odometer > threshold) {
      out.flagged[i] = true;
      out.flaggedCount++;
    }
  }
}

// Top-level /units/health fragment: "wear":{"median":M,"flagged":[i,...]}.
// Returns the would-be length like snprintf; the caller rejects >= cap.
inline size_t buildWearJson(const WearAssessment& w, char* buf, size_t cap) {
  size_t o = (size_t)snprintf(buf, cap, "\"wear\":{\"median\":%lu,\"flagged\":[",
                              (unsigned long)w.median);
  bool first = true;
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    if (!w.flagged[i]) continue;
    if (o >= cap) return o;
    o += (size_t)snprintf(buf + o, cap - o, "%s%d", first ? "" : ",", i);
    first = false;
  }
  if (o >= cap) return o;
  o += (size_t)snprintf(buf + o, cap - o, "]}");
  return o;
}
