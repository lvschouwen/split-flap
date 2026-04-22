// RTC-memory boot state shared between the master sketch and host-side
// tests. The hardware-only readBootStateRtc() / writeBootStateRtc() still
// live in ESPMaster.ino — only the pure layout and cookie-handling logic
// sits here so it can be exercised under `pio test -e native` without
// needing a live ESP.

#pragma once

#include <Arduino.h>
#include <string.h>

// 32-hex-char MD5 + NUL = 33, rounded up to 36 for 4-byte RTC-block
// alignment required by ESP.rtcUserMemoryRead/Write. See #53.
#define PRE_FLASH_MD5_LEN 36

struct RtcBootState {
  uint32_t magic;
  uint32_t bootCounter;
  char     preFlashSketchMd5[PRE_FLASH_MD5_LEN];
};

// Magic bumped to V2 in #53 when the struct gained preFlashSketchMd5.
// Old firmware's RTC state (magic 0xC0FFEE42) no longer matches, so
// normalizeBootState() zero-inits the whole struct — correct, because
// we can't trust the extra bytes sitting past where the old firmware wrote.
static const uint32_t RTC_BOOT_MAGIC           = 0xC0FFEE43UL;
static const uint32_t RTC_BOOT_OFFSET_BLOCKS   = 0;
static const uint32_t RECOVERY_BOOT_THRESHOLD  = 3;
static const unsigned long HEALTHY_BOOT_MS     = 30000UL;

// Zero the whole struct and stamp the current magic when the stored magic
// doesn't match. Guarantees every field is initialized before downstream
// code dereferences preFlashSketchMd5 or trusts bootCounter.
inline void normalizeBootState(RtcBootState& state) {
  if (state.magic != RTC_BOOT_MAGIC) {
    memset(&state, 0, sizeof(state));
    state.magic = RTC_BOOT_MAGIC;
  }
}

// Bounded length of the cookie. Returns PRE_FLASH_MD5_LEN if no NUL is
// found within the slot — callers treat that as "malformed, don't trust".
inline size_t cookieLength(const RtcBootState& state) {
  for (size_t i = 0; i < PRE_FLASH_MD5_LEN; i++) {
    if (state.preFlashSketchMd5[i] == '\0') return i;
  }
  return PRE_FLASH_MD5_LEN;
}

inline bool cookieIsPresent(const RtcBootState& state) {
  return state.preFlashSketchMd5[0] != '\0';
}

inline bool cookieIsMalformed(const RtcBootState& state) {
  return cookieLength(state) >= PRE_FLASH_MD5_LEN;
}

// Store `sketchMd5` into the cookie slot. Always leaves a NUL within the
// slot — truncating if input is too long, zero-filling the tail so a
// later SerialPrint cannot read stale bytes past the terminator, and
// tolerating a null pointer. The slot is guaranteed NUL-terminated at
// index <= PRE_FLASH_MD5_LEN - 1 after this call.
inline void setPreFlashMd5(RtcBootState& state, const char* sketchMd5) {
  size_t srcLen = (sketchMd5 == nullptr) ? 0 : strlen(sketchMd5);
  if (srcLen >= PRE_FLASH_MD5_LEN) srcLen = PRE_FLASH_MD5_LEN - 1;
  if (srcLen > 0) {
    memcpy(state.preFlashSketchMd5, sketchMd5, srcLen);
  }
  for (size_t i = srcLen; i < PRE_FLASH_MD5_LEN; i++) {
    state.preFlashSketchMd5[i] = '\0';
  }
}

// Length-bounded equality check. A malformed (unterminated) cookie is
// treated as no-match rather than walking past the slot — closes the
// read-past-end hazard the raw `String ==` compare would create if RTC
// data was ever corrupted or written by a foreign firmware with the same
// magic.
inline bool cookieMatchesRunning(const RtcBootState& state, const char* runningMd5) {
  if (runningMd5 == nullptr) return false;
  if (cookieIsMalformed(state)) return false;
  size_t cookieLen = cookieLength(state);
  if (cookieLen == 0) return false;
  size_t runLen = strlen(runningMd5);
  if (runLen != cookieLen) return false;
  return memcmp(runningMd5, state.preFlashSketchMd5, cookieLen) == 0;
}
