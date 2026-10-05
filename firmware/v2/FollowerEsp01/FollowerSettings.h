#pragma once
// FollowerSettings.h — the row's EEPROM records, natively tested by
// test_follower_settings. Besides the SDK's WiFi credentials the board
// persists two things: its pairing (the master's id, address and tz rule, so
// a restart lands in Grace and dials the master again) and, behind it, the
// operator preferences (#513). Pure encode/decode over fixed blobs; the
// EEPROM glue lives in FollowerCluster.cpp and FollowerPrefs.cpp.

#include <stdint.h>
#include <string.h>

#define FOLLOWER_NAME_MAX 32
#define FOLLOWER_HOST_MAX 40
#define FOLLOWER_TZ_MAX 64

// magic u32 LE | 42 bytes written as zero | name[NAME_MAX+1] |
// host[HOST_MAX+1] | tz[TZ_MAX+1] | xor checksum. The layout is per-device
// truth: the zero bytes held a row index and a message key the board no
// longer has, and stay so that the name, host and tz fields and the
// preferences record behind this one keep their place across images. A
// record that fails to decode leaves the row unpaired.
#define FOLLOWER_MEMBERSHIP_MAGIC 0x53464634UL  // "4FFS" LE
#define FOLLOWER_MEMBERSHIP_NAME_OFF (4 + 42)
#define FOLLOWER_MEMBERSHIP_HOST_OFF \
  (FOLLOWER_MEMBERSHIP_NAME_OFF + FOLLOWER_NAME_MAX + 1)
#define FOLLOWER_MEMBERSHIP_TZ_OFF \
  (FOLLOWER_MEMBERSHIP_HOST_OFF + FOLLOWER_HOST_MAX + 1)
#define FOLLOWER_MEMBERSHIP_BLOB_LEN \
  (FOLLOWER_MEMBERSHIP_TZ_OFF + FOLLOWER_TZ_MAX + 1 + 1)

// XOR of the payload ^ 0x7A: factory-fresh flash (all 0xFF / all 0x00)
// must never decode as a membership.
inline uint8_t followerMembershipChecksum(const uint8_t* blob) {
  uint8_t x = 0;
  for (int i = 0; i < FOLLOWER_MEMBERSHIP_BLOB_LEN - 1; i++) x ^= blob[i];
  return (uint8_t)(x ^ 0x7A);
}

// Encodes a pairing. Rejects (returns false, blob untouched) an empty host —
// a pairing without an address to dial is not a pairing — and oversized
// fields. tz is the master's POSIX zone for the clock fallback (may be "").
inline bool followerMembershipEncode(const char* leaderName,
                                     const char* leaderHost, const char* tz,
                                     uint8_t* blob) {
  size_t nameLen = strlen(leaderName);
  size_t hostLen = strlen(leaderHost);
  size_t tzLen = strlen(tz);
  if (hostLen == 0 || hostLen > FOLLOWER_HOST_MAX) return false;
  if (nameLen > FOLLOWER_NAME_MAX) return false;
  if (tzLen > FOLLOWER_TZ_MAX) return false;
  memset(blob, 0, FOLLOWER_MEMBERSHIP_BLOB_LEN);
  blob[0] = (uint8_t)(FOLLOWER_MEMBERSHIP_MAGIC & 0xFF);
  blob[1] = (uint8_t)((FOLLOWER_MEMBERSHIP_MAGIC >> 8) & 0xFF);
  blob[2] = (uint8_t)((FOLLOWER_MEMBERSHIP_MAGIC >> 16) & 0xFF);
  blob[3] = (uint8_t)((FOLLOWER_MEMBERSHIP_MAGIC >> 24) & 0xFF);
  memcpy(blob + FOLLOWER_MEMBERSHIP_NAME_OFF, leaderName, nameLen);
  memcpy(blob + FOLLOWER_MEMBERSHIP_HOST_OFF, leaderHost, hostLen);
  memcpy(blob + FOLLOWER_MEMBERSHIP_TZ_OFF, tz, tzLen);
  blob[FOLLOWER_MEMBERSHIP_BLOB_LEN - 1] = followerMembershipChecksum(blob);
  return true;
}

// Decodes the blob; false on bad magic/checksum (out params untouched).
// name/host/tz buffers must hold FOLLOWER_*_MAX + 1.
inline bool followerMembershipDecode(const uint8_t* blob, char* leaderName,
                                     char* leaderHost, char* tz) {
  uint32_t magic = (uint32_t)blob[0] | ((uint32_t)blob[1] << 8) |
                   ((uint32_t)blob[2] << 16) | ((uint32_t)blob[3] << 24);
  if (magic != FOLLOWER_MEMBERSHIP_MAGIC) return false;
  if (blob[FOLLOWER_MEMBERSHIP_BLOB_LEN - 1] !=
      followerMembershipChecksum(blob)) {
    return false;
  }
  if (blob[FOLLOWER_MEMBERSHIP_HOST_OFF] == '\0') return false;
  // Bounded copies — the terminators were zeroed at encode time, but a
  // corrupted-yet-checksum-colliding blob must still not run past the field.
  memcpy(leaderName, blob + FOLLOWER_MEMBERSHIP_NAME_OFF, FOLLOWER_NAME_MAX);
  leaderName[FOLLOWER_NAME_MAX] = '\0';
  memcpy(leaderHost, blob + FOLLOWER_MEMBERSHIP_HOST_OFF, FOLLOWER_HOST_MAX);
  leaderHost[FOLLOWER_HOST_MAX] = '\0';
  memcpy(tz, blob + FOLLOWER_MEMBERSHIP_TZ_OFF, FOLLOWER_TZ_MAX);
  tz[FOLLOWER_TZ_MAX] = '\0';
  return true;
}

inline void followerMembershipClear(uint8_t* blob) {
  memset(blob, 0, FOLLOWER_MEMBERSHIP_BLOB_LEN);
}

// --- operator preferences (#513) ----------------------------------------------
// A second, independent record directly behind the pairing, whose layout and
// magic it leaves alone. magic | flags | check. Bytes a firmware
// predating the record never wrote read back erased (0xFF) and fail the check,
// which yields the defaults.
#define FOLLOWER_PREFS_OFF FOLLOWER_MEMBERSHIP_BLOB_LEN
#define FOLLOWER_PREFS_LEN 3
#define FOLLOWER_EEPROM_LEN (FOLLOWER_PREFS_OFF + FOLLOWER_PREFS_LEN)
#define FOLLOWER_PREFS_MAGIC 0xB7
#define FOLLOWER_PREFS_CHECK_MASK 0x5C
#define FOLLOWER_PREF_REFLASH_ON_BOOT 0x01
// Bits 1-2: what the row shows once its master is written off, stored as the
// value + 1 so a record written before the field existed (0) reads as Time —
// what such a row did.
#define FOLLOWER_PREF_FALLBACK_SHIFT 1
#define FOLLOWER_PREF_FALLBACK_MASK 0x06

// Same numbers as wl.Fallback (wall_link.proto).
enum class FollowerFallback : uint8_t { Blank = 0, Time = 1, Date = 2 };

struct FollowerPrefs {
  FollowerFallback fallback = FollowerFallback::Time;
  // Boot auto-install of bootloader-mode units and auto-update of outdated
  // ones. Off = a gated campaign: nothing is flashed until an operator asks,
  // one unit at a time.
  bool reflashOnBoot = true;
};

inline uint8_t followerPrefsCheck(const uint8_t* rec) {
  return (uint8_t)(rec[0] ^ rec[1] ^ FOLLOWER_PREFS_CHECK_MASK);
}

inline void followerPrefsEncode(const FollowerPrefs& p,
                                uint8_t rec[FOLLOWER_PREFS_LEN]) {
  rec[0] = FOLLOWER_PREFS_MAGIC;
  rec[1] = (uint8_t)((p.reflashOnBoot ? FOLLOWER_PREF_REFLASH_ON_BOOT : 0) |
                     (((uint8_t)p.fallback + 1) << FOLLOWER_PREF_FALLBACK_SHIFT));
  rec[2] = followerPrefsCheck(rec);
}

// An unreadable record decodes to the defaults, never to "off": a corrupted
// byte must not silently stop a row from repairing its units.
inline FollowerPrefs followerPrefsDecode(const uint8_t rec[FOLLOWER_PREFS_LEN]) {
  FollowerPrefs p;
  if (rec[0] != FOLLOWER_PREFS_MAGIC || rec[2] != followerPrefsCheck(rec)) {
    return p;
  }
  p.reflashOnBoot = (rec[1] & FOLLOWER_PREF_REFLASH_ON_BOOT) != 0;
  const uint8_t stored = (rec[1] & FOLLOWER_PREF_FALLBACK_MASK) >> FOLLOWER_PREF_FALLBACK_SHIFT;
  if (stored != 0) p.fallback = (FollowerFallback)(stored - 1);
  return p;
}
