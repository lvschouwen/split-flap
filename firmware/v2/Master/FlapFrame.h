#pragma once

// FlapFrame.h (#203) — pure text → per-unit letter-index mapping, the frame
// displayTask hands to the unit bus. Extracted as a real seam from the body
// of v1's showMessage()/translateLettertoInt() (v1 keeps its own code — no
// retrofit). Semantics are v1's, with one approved deviation: an unknown
// char maps to blank (index 0) instead of skipping the slot and leaving the
// previous letter standing. Natively tested by test_flap_frame.

#include <stdint.h>

#include "DisplayCommand.h"     // DisplayAlignment
#include "SplitFlapProtocol.h"  // SFP_ALPHABET, SFP_FLAP_AMOUNT

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

// Web slider speed (1..100) → unit wire speed (MIN_SPEED..MAX_SPEED). Exact
// v1 convertSpeed() math: clamp first (Arduino map() extrapolates outside
// its input range), then the integer map with truncation toward zero.
inline int convertSpeedToUnit(int webSpeed) {
  if (webSpeed < 1) webSpeed = 1;
  if (webSpeed > 100) webSpeed = 100;
  return MIN_SPEED + (webSpeed - 1) * (MAX_SPEED - MIN_SPEED) / 99;
}

// Fills out[0..width-1] with the letter index each unit must show for
// `text` at the given alignment. v1 alignment contract: text at or over
// width keeps its FIRST `width` chars regardless of alignment; shorter text
// pads with blanks (left → trailing, right → leading, center → (width-len)/2
// leading, remainder trailing).
inline void flapFrameBuild(const char* text, int width,
                           DisplayAlignment align, uint8_t* out) {
  int len = 0;
  while (text[len] != '\0') len++;
  if (len > width) len = width;

  int lead = 0;
  if (len < width) {
    if (align == DisplayAlignment::Right) lead = width - len;
    else if (align == DisplayAlignment::Center) lead = (width - len) / 2;
  }

  for (int i = 0; i < width; i++) {
    int textIndex = i - lead;
    int letter = (textIndex >= 0 && textIndex < len)
                     ? flapLetterIndex(text[textIndex])
                     : 0;
    out[i] = (uint8_t)(letter < 0 ? 0 : letter);
  }
}
