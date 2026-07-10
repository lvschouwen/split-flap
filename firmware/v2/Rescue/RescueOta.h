#pragma once
// RescueOta.h — upload gating for the rescue app's POST /firmware/master
// (#195). normalizeOtaMd5 is a COPY of Master's OtaStatus.h helper (repo
// convention: rescue shares no compiled code with Master — fix bugs in both
// while both trees are alive). Natively tested (test/test_rescue_ota).

#include <Arduino.h>

// Lowercases the digest in place, then demands exactly 32 hex chars — a
// digest that can't be a digest is a client bug, rejected with 400 before
// any flash work starts (same wire contract as Master's /firmware/master).
inline bool normalizeOtaMd5(String& md5) {
  md5.toLowerCase();
  if (md5.length() != 32) return false;
  for (size_t i = 0; i < 32; i++) {
    char c = md5[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}
