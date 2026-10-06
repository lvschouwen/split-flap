#pragma once
// PrintableAscii.h — the one rule for strings that get stored and served
// again: printable ASCII only, from `lowest` up (0x21 = no spaces, for hosts
// and time zones handed to the C library; 0x20 where a space is allowed).

#include <Arduino.h>

inline bool printableAscii(const String& v, char lowest) {
  for (unsigned int i = 0; i < v.length(); i++) {
    unsigned char c = (unsigned char)v[i];
    if (c < (unsigned char)lowest || c > 0x7E) return false;
  }
  return true;
}
