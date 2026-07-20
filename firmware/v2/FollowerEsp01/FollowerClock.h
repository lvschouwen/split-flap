#pragma once
// FollowerClock.h — local clock fallback for a lost leader (#342), natively
// tested by test_follower_clock. ESP-01 rows can't promote, so a dead
// leader used to end in a blanked row; with the leader's POSIX tz persisted
// alongside the membership (FollowerSettings.h v4 blob) and SNTP synced,
// the Blank phase renders local HH:MM instead. Unsynced time or no tz keeps
// the old blank behavior; any leader contact reclaims the row and the next
// render replaces the clock. Trimmed from v1's clock composer: 24 h HH:MM,
// centered — no date/12 h variants on an 8-wide row.

#include <string.h>

#include "FollowerPolicy.h"  // FollowerPhase

// Centered zero-padded "HH:MM" in a width-char space-padded field; a width
// under 5 keeps the leading characters (a tiny row shows what fits). out
// must hold width + 1.
inline void followerClockText(int hour, int minute, int width, char* out) {
  char hhmm[6];
  hhmm[0] = (char)('0' + (hour / 10) % 10);
  hhmm[1] = (char)('0' + hour % 10);
  hhmm[2] = ':';
  hhmm[3] = (char)('0' + (minute / 10) % 10);
  hhmm[4] = (char)('0' + minute % 10);
  hhmm[5] = '\0';
  if (width <= 0) {
    out[0] = '\0';
    return;
  }
  memset(out, ' ', width);
  out[width] = '\0';
  int left = width > 5 ? (width - 5) / 2 : 0;
  for (int i = 0; i < 5 && left + i < width; i++) out[left + i] = hhmm[i];
}

// The fallback runs ONLY in Blank with a held membership (the tz belongs
// to a leader we still expect back), a known zone, and synced time.
// Standalone (never joined / left) stays dark — no membership, no zone.
inline bool followerClockEligible(FollowerPhase phase, bool membershipHeld,
                                  bool tzKnown, bool timeSynced) {
  return phase == FollowerPhase::Blank && membershipHeld && tzKnown &&
         timeSynced;
}
