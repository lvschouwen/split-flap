#pragma once
// Pure logic for the SFP_CMD_GET_LIFETIME reply (#406/#407) — the read path
// for the lifetime health fields the day-0 EEPROM layout added. Natively
// tested by test_lifetime. The EEPROM glue that FEEDS it lives in the unit's
// .ino files (bench tier).
//
// SHARED header: copied verbatim into Unit, Master and FollowerEsp01. Fix bugs
// in ALL three trees (copy policy; tests/test_copied_headers.py gates it).
//
// Why a new opcode rather than widening GET_EXT_DIAG: 0x89 is documented as
// unversioned — a format change there takes a new opcode anyway. And the two
// carry different things. Ext-diag is SINCE-BOOT (every reboot forgets it);
// this is what the unit remembers across power cycles.
//
// Wire format (15 bytes):
//   off   field                     type    notes
//   0     layoutVersion             u8      unit's EEPROM layout (UnitEeprom.h)
//   1     homeFailedCount           u8      lifetime, saturating
//   2     featureGates              u8      UNIT_GATE_* bits currently active
//   3     reserved                  u8      0
//   4..5  stepExcessLifetimeMax     u16 LE  worst drag ever seen
//   6..7  selfTestFirstHallWindow   u16 LE  baseline, set once
//   8..9  selfTestFirstStepsPerRev  u16 LE  baseline, set once
//   10..11 selfTestLastHallWindow   u16 LE  most recent valid reading
//   12..13 selfTestLastStepsPerRev  u16 LE  most recent valid reading
//   14    checksum                  u8      XOR of 0..13 ^ the mask below
//
// The pairs are the point: "hall window 46 when new, 12 now" is a diagnosis
// the unit carries itself. Unit 0x0f degraded pass -> marginal -> hall-dead in
// about two hours and its healthy reading of 46 survived only in a human's
// session note.
//
// MIXED-FIRMWARE SAFETY (mandatory — the wall runs both firmwares during a
// reflash): a unit predating this opcode answers with its 1-byte rotation
// status plus bus padding. Validation rejects on LENGTH first and then on the
// masked checksum, so all-0xFF, all-0x00 and repeated-status shapes all fail.
// The master degrades to "no lifetime data" and emits no fields — never a
// phantom reading (the #231/#106 pattern).

#include <stdint.h>

#define LIFETIME_REPLY_LEN            15
#define LIFETIME_REPLY_CHECKSUM_MASK  0x6E

struct UnitLifetimeFacts {
  uint8_t  layoutVersion = 0;
  uint8_t  homeFailedCount = 0;
  uint8_t  featureGates = 0;
  uint16_t stepExcessLifetimeMax = 0;
  uint16_t selfTestFirstHallWindow = 0;
  uint16_t selfTestFirstStepsPerRev = 0;
  uint16_t selfTestLastHallWindow = 0;
  uint16_t selfTestLastStepsPerRev = 0;
};

inline uint8_t lifetimeChecksum(const uint8_t buf[LIFETIME_REPLY_LEN]) {
  uint8_t x = 0;
  for (uint8_t i = 0; i < LIFETIME_REPLY_LEN - 1; i++) x ^= buf[i];
  return (uint8_t)(x ^ LIFETIME_REPLY_CHECKSUM_MASK);
}

inline void lifetimeEncodeReply(const UnitLifetimeFacts& f,
                                uint8_t buf[LIFETIME_REPLY_LEN]) {
  buf[0] = f.layoutVersion;
  buf[1] = f.homeFailedCount;
  buf[2] = f.featureGates;
  buf[3] = 0;  // reserved
  buf[4] = (uint8_t)(f.stepExcessLifetimeMax & 0xFF);
  buf[5] = (uint8_t)((f.stepExcessLifetimeMax >> 8) & 0xFF);
  buf[6] = (uint8_t)(f.selfTestFirstHallWindow & 0xFF);
  buf[7] = (uint8_t)((f.selfTestFirstHallWindow >> 8) & 0xFF);
  buf[8] = (uint8_t)(f.selfTestFirstStepsPerRev & 0xFF);
  buf[9] = (uint8_t)((f.selfTestFirstStepsPerRev >> 8) & 0xFF);
  buf[10] = (uint8_t)(f.selfTestLastHallWindow & 0xFF);
  buf[11] = (uint8_t)((f.selfTestLastHallWindow >> 8) & 0xFF);
  buf[12] = (uint8_t)(f.selfTestLastStepsPerRev & 0xFF);
  buf[13] = (uint8_t)((f.selfTestLastStepsPerRev >> 8) & 0xFF);
  buf[14] = lifetimeChecksum(buf);
}

// `len` is what the transaction actually returned. A short reply is an old
// unit, not a corrupt one — either way it is not decodable, and `out` is left
// untouched so a rejected read cannot half-write the master's fold.
inline bool lifetimeReadbackValid(const uint8_t* buf, uint8_t len,
                                  UnitLifetimeFacts& out) {
  if (len < LIFETIME_REPLY_LEN) return false;
  if (buf[LIFETIME_REPLY_LEN - 1] != lifetimeChecksum(buf)) return false;
  out.layoutVersion = buf[0];
  out.homeFailedCount = buf[1];
  out.featureGates = buf[2];
  out.stepExcessLifetimeMax = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
  out.selfTestFirstHallWindow = (uint16_t)buf[6] | ((uint16_t)buf[7] << 8);
  out.selfTestFirstStepsPerRev = (uint16_t)buf[8] | ((uint16_t)buf[9] << 8);
  out.selfTestLastHallWindow = (uint16_t)buf[10] | ((uint16_t)buf[11] << 8);
  out.selfTestLastStepsPerRev = (uint16_t)buf[12] | ((uint16_t)buf[13] << 8);
  return true;
}
