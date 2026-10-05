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

#include "FollowerPolicy.h"  // ClusterFollowerPhase

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

// Centered date in a width-char space-padded field: "DD MON YY" (the master's
// date row) where it fits, "DD-MM" on a narrower row. month is 1..12. out
// must hold width + 1.
inline void followerDateText(int day, int month, int year, int width, char* out) {
  static const char kMonths[] = "JANFEBMARAPRMAYJUNJULAUGSEPOCTNOVDEC";
  char text[10];
  text[0] = (char)('0' + (day / 10) % 10);
  text[1] = (char)('0' + day % 10);
  int len;
  if (width >= 9 && month >= 1 && month <= 12) {
    text[2] = ' ';
    memcpy(text + 3, kMonths + (month - 1) * 3, 3);
    text[6] = ' ';
    text[7] = (char)('0' + (year / 10) % 10);
    text[8] = (char)('0' + year % 10);
    len = 9;
  } else {
    text[2] = '-';
    text[3] = (char)('0' + (month / 10) % 10);
    text[4] = (char)('0' + month % 10);
    len = 5;
  }
  if (width <= 0) {
    out[0] = '\0';
    return;
  }
  memset(out, ' ', width);
  out[width] = '\0';
  int left = width > len ? (width - len) / 2 : 0;
  for (int i = 0; i < len && left + i < width; i++) out[left + i] = text[i];
}

// #362: the epoch→local-HH:MM conversion is NOT here — it is target libc
// glue (bench tier). On the ESP8266, setenv("TZ")+tzset() is INERT for
// localtime_r; the zone must be installed via the core's configTime(tz)
// (which drives newlib's __gettzinfo), done from loop context in
// FollowerCluster.cpp. A host-side test would use glibc's fully-working
// tzset and pass while the target renders UTC — actively misleading — so the
// tz application is proven on the bench, not natively. `followerClockText`
// (pure formatting) stays natively tested.

// The fallback runs ONLY in Blank with a held membership (the tz belongs
// to a leader we still expect back), a known zone, and synced time.
// Standalone (never joined / left) stays dark — no membership, no zone.
// A row set to fall back to blank never runs it.
inline bool followerClockEligible(ClusterFollowerPhase phase, bool membershipHeld,
                                  bool tzKnown, bool timeSynced,
                                  bool fallbackShowsClock = true) {
  return phase == ClusterFollowerPhase::LeaderLost && membershipHeld && tzKnown &&
         timeSynced && fallbackShowsClock;
}
