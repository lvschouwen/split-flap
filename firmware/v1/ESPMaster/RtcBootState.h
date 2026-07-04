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

// Boot-mode values for RtcBootState.bootMode (issue #117). BOOT_MODE_OTA is
// one-shot: setup() clears it back to NORMAL immediately on entry, so any
// reboot or power cycle out of OTA mode lands in a normal boot.
static const uint32_t BOOT_MODE_NORMAL = 0;
static const uint32_t BOOT_MODE_OTA    = 1;

// What the MD5 in the cookie slot means (issue #118):
//   PRE_FLASH    — the sketch MD5 that was running BEFORE the flash.
//                  running == cookie  ->  old bits still here -> "reverted".
//                  Ambiguous for a same-image reflash (legacy semantics,
//                  used only when the upload carried no ?md5= param).
//   EXPECTED_MD5 — the MD5 of the uploaded image itself.
//                  running == cookie  ->  new bits running    -> "ok".
//                  Unambiguous; used whenever ?md5= is present.
static const uint32_t COOKIE_KIND_NONE         = 0;
static const uint32_t COOKIE_KIND_PRE_FLASH    = 1;
static const uint32_t COOKIE_KIND_EXPECTED_MD5 = 2;

struct RtcBootState {
  uint32_t magic;
  uint32_t bootCounter;
  uint32_t bootMode;
  uint32_t cookieKind;
  char     preFlashSketchMd5[PRE_FLASH_MD5_LEN];
};

// Magic bumped to V2 in #53 when the struct gained preFlashSketchMd5, to V3
// in #117 when it gained bootMode, and to V4 in #118 when it gained
// cookieKind — without the bump, an older layout's stale trailing bytes
// could be misread as live fields (e.g. BOOT_MODE_OTA uninvited). A
// mismatched magic makes normalizeBootState() zero-init the whole struct.
static const uint32_t RTC_BOOT_MAGIC           = 0xC0FFEE45UL;

// V3 (#117, last shipped in 98ec681): no cookieKind, so the md5 slot sat
// 4 bytes earlier. Kept only for the in-place migration below — a device
// flashed BY a V3 firmware carries its pre-flash cookie in this layout,
// and discarding it (the pre-#119 behavior) left the post-upgrade boot
// unable to adjudicate the flash, so /settings kept showing whatever
// stale verdict was in EEPROM.
static const uint32_t RTC_BOOT_MAGIC_V3        = 0xC0FFEE44UL;

struct RtcBootStateV3 {
  uint32_t magic;
  uint32_t bootCounter;
  uint32_t bootMode;
  char     preFlashSketchMd5[PRE_FLASH_MD5_LEN];
};
// Block 32, NOT 0 (issue #115): the ESP8266 core's Update.end() writes the
// eboot command (32 blocks / 128 bytes: magic, ACTION_COPY_RAW, args, crc32)
// at the start of RTC user memory. At offset 0, the post-flash cookie write
// destroyed that command — eboot found no magic, booted the old image, and
// every OTA "silently reverted" (while the cookie dutifully reported the
// revert it had itself caused). State previously stored at offset 0 is
// ignored after this change; the magic check zero-inits on first boot.
static const uint32_t RTC_BOOT_OFFSET_BLOCKS   = 32;
static const uint32_t RECOVERY_BOOT_THRESHOLD  = 3;
static const unsigned long HEALTHY_BOOT_MS     = 30000UL;

// Zero the whole struct and stamp the current magic when the stored magic
// doesn't match. Guarantees every field is initialized before downstream
// code dereferences preFlashSketchMd5 or trusts bootCounter. A V3 state
// is migrated in place instead of zeroed (#119): V3 writers only ever
// stored the pre-flash sketch MD5 (#118's EXPECTED_MD5 kind didn't exist
// yet), so a present cookie gets legacy PRE_FLASH semantics.
inline void normalizeBootState(RtcBootState& state) {
  if (state.magic == RTC_BOOT_MAGIC_V3) {
    RtcBootStateV3 old;
    memcpy(&old, &state, sizeof(old));
    memset(&state, 0, sizeof(state));
    state.magic = RTC_BOOT_MAGIC;
    state.bootCounter = old.bootCounter;
    state.bootMode = old.bootMode;
    memcpy(state.preFlashSketchMd5, old.preFlashSketchMd5, PRE_FLASH_MD5_LEN);
    state.cookieKind = (state.preFlashSketchMd5[0] != '\0')
                           ? COOKIE_KIND_PRE_FLASH
                           : COOKIE_KIND_NONE;
    return;
  }
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

// Resolves the post-boot flash verdict from the cookie (issue #118).
// Returns:
//   nullptr    — no cookie present; nothing to judge (don't touch EEPROM)
//   ""         — cookie malformed; outcome unknown (clears a stale verdict)
//   "ok"       — the flash took
//   "reverted" — the old image is still running
inline const char* resolveFlashVerdict(const RtcBootState& state, const char* runningMd5);

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

inline const char* resolveFlashVerdict(const RtcBootState& state, const char* runningMd5) {
  if (!cookieIsPresent(state)) return nullptr;
  if (cookieIsMalformed(state)) return "";
  bool matches = cookieMatchesRunning(state, runningMd5);
  if (state.cookieKind == COOKIE_KIND_EXPECTED_MD5) {
    return matches ? "ok" : "reverted";
  }
  //COOKIE_KIND_PRE_FLASH (and anything unexpected): legacy semantics.
  return matches ? "reverted" : "ok";
}
