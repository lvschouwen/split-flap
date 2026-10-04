#pragma once

// FlapLetters.h — the pure character → flap index and web speed → wire speed
// mappings every row master renders through. One definition, so a row driven
// by the S3 and a row driven by an ESP-01 show the same glyph for the same
// character. Natively tested by test_flap_frame (Master) and
// test_flap_letters (FollowerEsp01).

#include <stdint.h>

#include "SplitFlapProtocol.h"  // SFP_ALPHABET, SFP_FLAP_AMOUNT

// Unit speed byte range both row masters send (the wire contract's rpm scale).
#define MIN_SPEED 1
#define MAX_SPEED 12

// A unit clamps the speed byte to SFP_UNIT_SPEED_MAX, so a wider range here
// would silently flatten its top end.
static_assert(MIN_SPEED >= 1 && MAX_SPEED <= SFP_UNIT_SPEED_MAX,
              "the wire speed range must stay inside what a unit accepts");

// Index of `c` in SFP_ALPHABET after ASCII uppercasing, or -1 when the drum
// has no such flap. ä/ö/ü never reach this layer raw — the web UI wire-
// encodes them as $ & # (SplitFlapProtocol.h contract).
inline int flapLetterIndex(char c) {
  if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
  const char* alphabet = SFP_ALPHABET;
  for (int i = 0; i < SFP_FLAP_AMOUNT; i++) {
    if (alphabet[i] == c) return i;
  }
  return -1;
}

// The index a unit is commanded to: a character the drum lacks shows blank
// (index 0) rather than leaving the previous letter standing.
inline uint8_t flapLetterOrBlank(char c) {
  int letter = flapLetterIndex(c);
  return (uint8_t)(letter < 0 ? 0 : letter);
}

// Web slider speed (1..100) → unit wire speed (MIN_SPEED..MAX_SPEED): clamp
// first (Arduino map() extrapolates outside its input range), then the
// integer map with truncation toward zero.
inline int convertSpeedToUnit(int webSpeed) {
  if (webSpeed < 1) webSpeed = 1;
  if (webSpeed > 100) webSpeed = 100;
  return MIN_SPEED + (webSpeed - 1) * (MAX_SPEED - MIN_SPEED) / 99;
}
