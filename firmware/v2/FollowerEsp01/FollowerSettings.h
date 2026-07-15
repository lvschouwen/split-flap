#pragma once
// FollowerSettings.h — the follower's EEPROM membership record (#298),
// natively tested by test_follower_settings. The ONLY on-ESP persisted
// setting besides the SDK's WiFi credentials: the `clusteredBy` marker
// (leader name/host + row) so a reboot boots into Grace instead of
// flashing stale standalone content. Pure encode/decode over a fixed blob;
// the EEPROM.begin/commit glue lives in main.cpp.

#include <stdint.h>
#include <string.h>

#define FOLLOWER_NAME_MAX 32
#define FOLLOWER_HOST_MAX 40

// magic u32 LE | row u8 | name[NAME_MAX+1] | host[HOST_MAX+1] | xor checksum
#define FOLLOWER_MEMBERSHIP_MAGIC 0x53464631UL  // "1FFS" LE
#define FOLLOWER_MEMBERSHIP_BLOB_LEN \
  (4 + 1 + (FOLLOWER_NAME_MAX + 1) + (FOLLOWER_HOST_MAX + 1) + 1)

// XOR of the payload ^ 0x7A: factory-fresh flash (all 0xFF / all 0x00)
// must never decode as a membership.
inline uint8_t followerMembershipChecksum(const uint8_t* blob) {
  uint8_t x = 0;
  for (int i = 0; i < FOLLOWER_MEMBERSHIP_BLOB_LEN - 1; i++) x ^= blob[i];
  return (uint8_t)(x ^ 0x7A);
}

// Encodes a membership. Rejects (returns false, blob untouched) an empty
// host — a membership without a reachable leader host is not a membership
// (v2 follower's leaderHost sentinel rule) — and oversized fields.
inline bool followerMembershipEncode(const char* leaderName,
                                     const char* leaderHost, uint8_t row,
                                     uint8_t* blob) {
  size_t nameLen = strlen(leaderName);
  size_t hostLen = strlen(leaderHost);
  if (hostLen == 0 || hostLen > FOLLOWER_HOST_MAX) return false;
  if (nameLen > FOLLOWER_NAME_MAX) return false;
  memset(blob, 0, FOLLOWER_MEMBERSHIP_BLOB_LEN);
  blob[0] = (uint8_t)(FOLLOWER_MEMBERSHIP_MAGIC & 0xFF);
  blob[1] = (uint8_t)((FOLLOWER_MEMBERSHIP_MAGIC >> 8) & 0xFF);
  blob[2] = (uint8_t)((FOLLOWER_MEMBERSHIP_MAGIC >> 16) & 0xFF);
  blob[3] = (uint8_t)((FOLLOWER_MEMBERSHIP_MAGIC >> 24) & 0xFF);
  blob[4] = row;
  memcpy(blob + 5, leaderName, nameLen);
  memcpy(blob + 5 + FOLLOWER_NAME_MAX + 1, leaderHost, hostLen);
  blob[FOLLOWER_MEMBERSHIP_BLOB_LEN - 1] = followerMembershipChecksum(blob);
  return true;
}

// Decodes the blob; false on bad magic/checksum (out params untouched).
// name/host buffers must hold FOLLOWER_*_MAX + 1.
inline bool followerMembershipDecode(const uint8_t* blob, char* leaderName,
                                     char* leaderHost, uint8_t& row) {
  uint32_t magic = (uint32_t)blob[0] | ((uint32_t)blob[1] << 8) |
                   ((uint32_t)blob[2] << 16) | ((uint32_t)blob[3] << 24);
  if (magic != FOLLOWER_MEMBERSHIP_MAGIC) return false;
  if (blob[FOLLOWER_MEMBERSHIP_BLOB_LEN - 1] !=
      followerMembershipChecksum(blob)) {
    return false;
  }
  // Bounded copies — the terminators were zeroed at encode time, but a
  // corrupted-yet-checksum-colliding blob must still not run past the field.
  memcpy(leaderName, blob + 5, FOLLOWER_NAME_MAX);
  leaderName[FOLLOWER_NAME_MAX] = '\0';
  memcpy(leaderHost, blob + 5 + FOLLOWER_NAME_MAX + 1, FOLLOWER_HOST_MAX);
  leaderHost[FOLLOWER_HOST_MAX] = '\0';
  if (leaderHost[0] == '\0') return false;
  row = blob[4];
  return true;
}

inline void followerMembershipClear(uint8_t* blob) {
  memset(blob, 0, FOLLOWER_MEMBERSHIP_BLOB_LEN);
}
