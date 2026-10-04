#pragma once

// FlapFrame.h (#203) — pure text → per-unit letter-index frame, the frame
// displayTask hands to the unit bus. The per-character mapping and the speed
// conversion are the shared FlapLetters.h; this adds the alignment pass.
// Natively tested by test_flap_frame.

#include <stdint.h>

#include "DisplayCommand.h"  // DisplayAlignment
#include "FlapLetters.h"     // flapLetterOrBlank, convertSpeedToUnit

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
    out[i] = (textIndex >= 0 && textIndex < len)
                 ? flapLetterOrBlank(text[textIndex])
                 : 0;
  }
}
