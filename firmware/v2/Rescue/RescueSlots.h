#pragma once
// RescueSlots.h — pure slot-choice logic for POST /rescue/exit (#195/#200).
// Decides which slot an accidental rescue entry boots back into. With
// otadata erased there is no "previously running slot" signal left; the
// primary ranking is Master's NVS confirm records (RescueSlotRecord.h —
// higher seq = ran healthy more recently). The esp_app_desc_t date/time
// stamp is only a fallback: pioarduino's hybrid compile freezes it at
// framework-assembly time, so images from the same framework cache all
// carry the same stamp (#200). Natively tested (test/test_rescue_slots).

#include <stdint.h>

// The pieces of parseAppBuildStamp. Each returns -1 (or false) for a field
// that does not parse; the caller turns that into stamp 0.

// True when s holds exactly n characters.
static inline bool buildStampHasLength(const char* s, int n) {
  for (int i = 0; i < n; i++) {
    if (s[i] == '\0') return false;
  }
  return s[n] == '\0';
}

// The n digits at s as a number, -1 when one is not a digit.
static inline int buildStampDigits(const char* s, int n) {
  int v = 0;
  for (int i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return -1;
    v = v * 10 + (s[i] - '0');
  }
  return v;
}

// "Mmm" as 1..12, -1 when it names no month.
static inline int buildStampMonth(const char* date) {
  static const char* MONTHS[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  for (int m = 0; m < 12; m++) {
    if (date[0] == MONTHS[m][0] && date[1] == MONTHS[m][1] &&
        date[2] == MONTHS[m][2]) {
      return m + 1;
    }
  }
  return -1;
}

// The "dd" at date[4..5], 1..31; __DATE__ pads single digits with a space
// ("Jul  1 2026"). -1 when it does not parse.
static inline int buildStampDay(const char* date) {
  int day = buildStampDigits(date + 5, 1);
  if (day < 0) return -1;
  if (date[4] != ' ') {
    int tens = buildStampDigits(date + 4, 1);
    if (tens < 0) return -1;
    day += tens * 10;
  }
  return (day < 1 || day > 31) ? -1 : day;
}

// "hh:mm:ss" (exactly 8 chars) into hms; false when it does not parse.
static inline bool buildStampTime(const char* time, int hms[3]) {
  if (!buildStampHasLength(time, 8) || time[2] != ':' || time[5] != ':') return false;
  for (int f = 0; f < 3; f++) {
    hms[f] = buildStampDigits(time + f * 3, 2);
    if (hms[f] < 0) return false;
  }
  return hms[0] <= 23 && hms[1] <= 59 && hms[2] <= 59;
}

// Encodes __DATE__ ("Mmm dd yyyy", day space-padded) + __TIME__ ("hh:mm:ss")
// into one comparable integer. Returns 0 for anything unparseable — callers
// treat 0 as "no ordering signal", never as an error.
static inline uint64_t parseAppBuildStamp(const char* date, const char* time) {
  if (date == nullptr || time == nullptr) return 0;
  // "Mmm dd yyyy" — exactly 11 chars.
  if (!buildStampHasLength(date, 11)) return 0;
  int month = buildStampMonth(date);
  if (month < 0 || date[3] != ' ') return 0;
  int day = buildStampDay(date);
  if (day < 0 || date[6] != ' ') return 0;
  int year = buildStampDigits(date + 7, 4);
  if (year < 0) return 0;
  int hms[3] = {0, 0, 0};
  if (!buildStampTime(time, hms)) return 0;

  uint64_t stamp = (uint64_t)year * 12 + (month - 1);
  stamp = stamp * 32 + day;
  stamp = stamp * 24 + hms[0];
  stamp = stamp * 60 + hms[1];
  stamp = stamp * 60 + hms[2];
  return stamp;
}

// One OTA slot as seen by /rescue/exit: descriptor validity + build stamp,
// plus the #200 NVS confirm record (confirmed = record present AND its
// sha256 matches the slot's current image).
struct ExitSlotCandidate {
  bool valid = false;
  uint64_t stamp = 0;
  bool confirmed = false;
  uint32_t seq = 0;
};

// Which OTA slot should /rescue/exit boot. Confirm records rank first (the
// build stamp is frozen at framework-assembly time under pioarduino hybrid
// builds, so it cannot order images — #200): higher seq = image Master ran
// healthy most recently. A confirmed slot beats an unconfirmed one
// (known-good vs pre-#200 firmware or a hand-flashed unknown); with no
// records the stamp comparison remains as fallback. Ties fall to slot 0
// deterministically. -1 = no valid slot (the endpoint answers 409 — nothing
// to exit into).
static inline int pickExitSlot(const ExitSlotCandidate& s0,
                               const ExitSlotCandidate& s1) {
  if (s0.valid && s1.valid) {
    bool c0 = s0.confirmed, c1 = s1.confirmed;
    if (c0 && c1) return (s1.seq > s0.seq) ? 1 : 0;
    if (c0 != c1) return c1 ? 1 : 0;
    return (s1.stamp > s0.stamp) ? 1 : 0;
  }
  if (s0.valid) return 0;
  if (s1.valid) return 1;
  return -1;
}
