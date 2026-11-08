#pragma once
// Brownout-loop guard (#504): a board whose supply cannot carry its own start
// browns out at the same point of every boot — the boot-home inrush, the
// radio's join — and never stays up long enough for anyone to look. After
// BROWNOUT_LOOP_LIMIT brownout resets in a row, each cutting the boot before
// it short of BROWNOUT_LOOP_WINDOW_MS, the board boots held: no boot-home,
// no display command run, until an operator clears it (the API action
// `clear-brownout-hold`); after that one unit moves at a time for the rest
// of that boot. The hold is latched: it survives every reset until cleared,
// or until a held boot has stayed up BROWNOUT_HOLD_RELEASE_MS — a boot that
// moves nothing and survives that long lets go by itself, so a household
// wall never stays frozen for want of an API call.
//
// One brownout is no evidence: a manual power cycle of the S3 reports
// Brownout too, and that is the normal start of a leader. What tells a loop
// from an operator is that the boot it ended was young.
//
// The `starts` ring (BootTracePolicy.h) holds reset reason + stage per boot
// but no time, so whether a boot lived the window is kept here, in one small
// NVS record. Pure rules + codec here (natively tested by
// test_brownout_guard); NVS, the hold and the report in BrownoutGuard.cpp.

#include <stddef.h>
#include <stdint.h>

#include "CrashContextPolicy.h"  // CRASH_RESET_BROWNOUT

// Three, like the boot guard's crash limit: one brownout is a power cycle,
// two can be an operator cycling power again during a start, a third in a
// row each inside the window is a loop. The ring holds 8 boots, so the boots
// that tripped it stay visible in `starts` beside the hold.
#define BROWNOUT_LOOP_LIMIT 3
// A boot that lived this long was not cut short by its own start: the
// observed loops died within 31 s of reset (three boots in that window), the
// boot-home of 16 units one at a time with its settles takes about a minute,
// and the first renders after it follow. Ten minutes covers a start-up unit
// update's first batches too, yet three deliberate power cycles each inside
// ten minutes of the last are not an operator's habit.
#define BROWNOUT_LOOP_WINDOW_MS (10UL * 60UL * 1000UL)
// Units moving at once for the rest of a held boot once it is cleared
// (MotionBudget.h's floor; the normal ceiling is MOTION_BUDGET_MAX).
#define BROWNOUT_HOLD_MOTION_CAP 1
// A held boot up this long releases itself, as a clear would. Three times
// the loop window: a supply that browns out only under motion still holds
// still for this long, and every brownout before it starts the next boot's
// own count from zero.
#define BROWNOUT_HOLD_RELEASE_MS (30UL * 60UL * 1000UL)

#define BROWNOUT_GUARD_MAGIC 0xB6
#define BROWNOUT_GUARD_BLOB_LEN 4

#define BROWNOUT_GUARD_FLAG_YOUNG 0x01  // the boot that wrote it has not lived the window
#define BROWNOUT_GUARD_FLAG_HELD 0x02

struct BrownoutGuardRecord {
  uint8_t streak = 0;  // brownouts in a row that each ended a young boot
  bool young = false;
  bool held = false;
};

inline void brownoutGuardEncode(const BrownoutGuardRecord& r,
                                uint8_t out[BROWNOUT_GUARD_BLOB_LEN]) {
  out[0] = BROWNOUT_GUARD_MAGIC;
  out[1] = r.streak;
  out[2] = (uint8_t)((r.young ? BROWNOUT_GUARD_FLAG_YOUNG : 0) |
                     (r.held ? BROWNOUT_GUARD_FLAG_HELD : 0));
  out[3] = (uint8_t)(out[0] ^ out[1] ^ out[2] ^ 0xFF);
}

// Anything this code did not write decodes as no history, never as a hold.
inline bool brownoutGuardDecode(const uint8_t* blob, size_t len, BrownoutGuardRecord& out) {
  out = BrownoutGuardRecord{};
  if (blob == nullptr || len < BROWNOUT_GUARD_BLOB_LEN) return false;
  if (blob[0] != BROWNOUT_GUARD_MAGIC) return false;
  if (blob[3] != (uint8_t)(blob[0] ^ blob[1] ^ blob[2] ^ 0xFF)) return false;
  out.streak = blob[1];
  out.young = (blob[2] & BROWNOUT_GUARD_FLAG_YOUNG) != 0;
  out.held = (blob[2] & BROWNOUT_GUARD_FLAG_HELD) != 0;
  return true;
}

// The record for this boot, from the one the boot before left and the reset
// this boot started from. Any other reset, or a brownout that ended a boot
// which had lived the window, ends the streak; a hold stays until cleared.
inline BrownoutGuardRecord brownoutGuardStart(const BrownoutGuardRecord& prev, int resetReason) {
  BrownoutGuardRecord r = prev;
  if (resetReason == CRASH_RESET_BROWNOUT && prev.young) {
    if (r.streak < UINT8_MAX) r.streak++;
  } else {
    r.streak = 0;
  }
  if (r.streak >= BROWNOUT_LOOP_LIMIT) r.held = true;
  r.young = true;
  return r;
}

// True once this boot has lived the window (the caller persists): the
// streak is over. A held boot never settles by itself — only a clear ends it.
inline bool brownoutGuardSettle(BrownoutGuardRecord& r, uint32_t uptimeMs) {
  if (!r.young || r.held || uptimeMs < BROWNOUT_LOOP_WINDOW_MS) return false;
  r.young = false;
  r.streak = 0;
  return true;
}

// The operator's clear: the hold and the streak end, and this boot counts as
// one that lived, so a brownout right after it starts a new count. False
// (nothing to persist) when nothing was held.
inline bool brownoutGuardClear(BrownoutGuardRecord& r) {
  if (!r.held) return false;
  r.held = false;
  r.streak = 0;
  r.young = false;
  return true;
}

// A held boot that has stayed up the release time lets go by itself, as the
// operator's clear (the caller persists and logs). False when not held or
// not yet that long up.
inline bool brownoutGuardAutoRelease(BrownoutGuardRecord& r, uint32_t uptimeMs) {
  if (!r.held || uptimeMs < BROWNOUT_HOLD_RELEASE_MS) return false;
  return brownoutGuardClear(r);
}
