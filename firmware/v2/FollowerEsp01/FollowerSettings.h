#pragma once
// FollowerSettings.h — the follower's EEPROM records, natively tested by
// test_follower_settings. Besides the SDK's WiFi credentials the board
// persists two things: the `clusteredBy` membership marker (#298: leader
// name/host + row, so a reboot boots into Grace instead of flashing stale
// standalone content) and, behind it, the operator preferences (#513). Pure
// encode/decode over fixed blobs; the EEPROM glue lives in FollowerCluster.cpp
// and FollowerPrefs.cpp.

#include <stdint.h>
#include <string.h>

#define FOLLOWER_NAME_MAX 32
#define FOLLOWER_HOST_MAX 40
#define FOLLOWER_TZ_MAX 64
#define FOLLOWER_HMAC_KEY_LEN 32

// magic u32 LE | row u8 | keyPresent u8 | key[32] | lastTs u64 LE |
// name[NAME_MAX+1] | host[HOST_MAX+1] | tz[TZ_MAX+1] | xor checksum. The
// #313-follow-on key field bumped the magic to 2FFS; the follow-on
// replay-mark (lastTs, HIGH#1) to 3FFS; the #342 clock-fallback tz (the
// leader's POSIX zone, sent on the join) to 4FFS — an old-format record
// fails to decode and the follower boots standalone (harmless: it re-joins
// and gets key + tz fresh).
#define FOLLOWER_MEMBERSHIP_MAGIC 0x53464634UL  // "4FFS" LE (was 3FFS/2FFS/1FFS)
#define FOLLOWER_MEMBERSHIP_BLOB_LEN                                       \
  (4 + 1 + 1 + FOLLOWER_HMAC_KEY_LEN + 8 + (FOLLOWER_NAME_MAX + 1) +       \
   (FOLLOWER_HOST_MAX + 1) + (FOLLOWER_TZ_MAX + 1) + 1)
#define FOLLOWER_MEMBERSHIP_LASTTS_OFF (4 + 1 + 1 + FOLLOWER_HMAC_KEY_LEN)
#define FOLLOWER_MEMBERSHIP_NAME_OFF (FOLLOWER_MEMBERSHIP_LASTTS_OFF + 8)
#define FOLLOWER_MEMBERSHIP_HOST_OFF \
  (FOLLOWER_MEMBERSHIP_NAME_OFF + FOLLOWER_NAME_MAX + 1)
#define FOLLOWER_MEMBERSHIP_TZ_OFF \
  (FOLLOWER_MEMBERSHIP_HOST_OFF + FOLLOWER_HOST_MAX + 1)

// XOR of the payload ^ 0x7A: factory-fresh flash (all 0xFF / all 0x00)
// must never decode as a membership.
inline uint8_t followerMembershipChecksum(const uint8_t* blob) {
  uint8_t x = 0;
  for (int i = 0; i < FOLLOWER_MEMBERSHIP_BLOB_LEN - 1; i++) x ^= blob[i];
  return (uint8_t)(x ^ 0x7A);
}

// Encodes a membership. Rejects (returns false, blob untouched) an empty
// host — a membership without a reachable leader host is not a membership
// (v2 follower's leaderHost sentinel rule) — and oversized fields. keyValid
// carries the #313-follow-on wire-auth key (key32 read only when keyValid);
// tz is the leader's POSIX zone for the clock fallback (#342, may be "").
inline bool followerMembershipEncode(const char* leaderName,
                                     const char* leaderHost, const char* tz,
                                     uint8_t row, bool keyValid,
                                     const uint8_t* key32, uint64_t lastTs,
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
  blob[4] = row;
  blob[5] = keyValid ? 1 : 0;
  if (keyValid && key32 != nullptr) memcpy(blob + 6, key32, FOLLOWER_HMAC_KEY_LEN);
  for (int i = 0; i < 8; i++) {
    blob[FOLLOWER_MEMBERSHIP_LASTTS_OFF + i] = (uint8_t)(lastTs >> (i * 8));
  }
  memcpy(blob + FOLLOWER_MEMBERSHIP_NAME_OFF, leaderName, nameLen);
  memcpy(blob + FOLLOWER_MEMBERSHIP_HOST_OFF, leaderHost, hostLen);
  memcpy(blob + FOLLOWER_MEMBERSHIP_TZ_OFF, tz, tzLen);
  blob[FOLLOWER_MEMBERSHIP_BLOB_LEN - 1] = followerMembershipChecksum(blob);
  return true;
}

// Decodes the blob; false on bad magic/checksum (out params untouched).
// name/host/tz buffers must hold FOLLOWER_*_MAX + 1; key32 must hold 32
// bytes.
inline bool followerMembershipDecode(const uint8_t* blob, char* leaderName,
                                     char* leaderHost, char* tz, uint8_t& row,
                                     bool& keyValid, uint8_t* key32,
                                     uint64_t& lastTs) {
  uint32_t magic = (uint32_t)blob[0] | ((uint32_t)blob[1] << 8) |
                   ((uint32_t)blob[2] << 16) | ((uint32_t)blob[3] << 24);
  if (magic != FOLLOWER_MEMBERSHIP_MAGIC) return false;
  if (blob[FOLLOWER_MEMBERSHIP_BLOB_LEN - 1] !=
      followerMembershipChecksum(blob)) {
    return false;
  }
  // Bounded copies — the terminators were zeroed at encode time, but a
  // corrupted-yet-checksum-colliding blob must still not run past the field.
  memcpy(leaderName, blob + FOLLOWER_MEMBERSHIP_NAME_OFF, FOLLOWER_NAME_MAX);
  leaderName[FOLLOWER_NAME_MAX] = '\0';
  memcpy(leaderHost, blob + FOLLOWER_MEMBERSHIP_HOST_OFF, FOLLOWER_HOST_MAX);
  leaderHost[FOLLOWER_HOST_MAX] = '\0';
  if (leaderHost[0] == '\0') return false;
  memcpy(tz, blob + FOLLOWER_MEMBERSHIP_TZ_OFF, FOLLOWER_TZ_MAX);
  tz[FOLLOWER_TZ_MAX] = '\0';
  row = blob[4];
  keyValid = blob[5] != 0;
  if (key32 != nullptr) memcpy(key32, blob + 6, FOLLOWER_HMAC_KEY_LEN);
  lastTs = 0;
  for (int i = 0; i < 8; i++) {
    lastTs |= (uint64_t)blob[FOLLOWER_MEMBERSHIP_LASTTS_OFF + i] << (i * 8);
  }
  return true;
}

inline void followerMembershipClear(uint8_t* blob) {
  memset(blob, 0, FOLLOWER_MEMBERSHIP_BLOB_LEN);
}

// --- operator preferences (#513) ----------------------------------------------
// A second, independent record directly behind the membership blob, so adding
// it leaves that blob's layout and magic alone — a change there drops the
// follower out of its cluster. magic | flags | check. Bytes a firmware
// predating the record never wrote read back erased (0xFF) and fail the check,
// which yields the defaults.
#define FOLLOWER_PREFS_OFF FOLLOWER_MEMBERSHIP_BLOB_LEN
#define FOLLOWER_PREFS_LEN 3
#define FOLLOWER_EEPROM_LEN (FOLLOWER_PREFS_OFF + FOLLOWER_PREFS_LEN)
#define FOLLOWER_PREFS_MAGIC 0xB7
#define FOLLOWER_PREFS_CHECK_MASK 0x5C
#define FOLLOWER_PREF_REFLASH_ON_BOOT 0x01

struct FollowerPrefs {
  // Boot auto-install of bootloader-mode units and auto-update of outdated
  // ones. Off = a gated campaign: nothing is flashed until an operator asks,
  // one unit at a time (the S3's #412 setting, same name on the wire).
  bool reflashOnBoot = true;
};

inline uint8_t followerPrefsCheck(const uint8_t* rec) {
  return (uint8_t)(rec[0] ^ rec[1] ^ FOLLOWER_PREFS_CHECK_MASK);
}

inline void followerPrefsEncode(const FollowerPrefs& p,
                                uint8_t rec[FOLLOWER_PREFS_LEN]) {
  rec[0] = FOLLOWER_PREFS_MAGIC;
  rec[1] = p.reflashOnBoot ? FOLLOWER_PREF_REFLASH_ON_BOOT : 0;
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
  return p;
}

// Strict: a setting that gates unit flashing takes exactly "true" or "false".
inline bool followerParseBool(const char* s, bool& out) {
  if (s == nullptr) return false;
  if (strcmp(s, "true") == 0) {
    out = true;
    return true;
  }
  if (strcmp(s, "false") == 0) {
    out = false;
    return true;
  }
  return false;
}
