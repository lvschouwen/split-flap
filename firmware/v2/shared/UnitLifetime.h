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
//   3     idleHallStandDown         u8      bit7 = disarmed, bits0..6 = count
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

// Byte 3, the idle hall check's own report on itself (#460). The check
// disarms after a few FUTILE re-homes — ones that measured no drift, and so
// proved the window model wrong rather than the belief, which a re-home
// cannot fix. Both halves of that were invisible from the wire: a since-boot
// RAM counter printed to a Nano serial line nobody monitors (and which on the
// esp01 rows IS the I2C bus), and a futile re-home moves no other counter,
// because finding no drift is the whole point. A permanently disarmed unit
// read exactly like a healthy one.
//
// The DISARMED STATE is its own bit rather than something the master
// re-derives from the count. The limit is the unit's rule
// (IDLE_HALL_FUTILE_REHOME_LIMIT, unit-local); a master comparing against its
// own copy of that constant is precisely the two-sides-drift that #458 was.
//
// Byte 3 was a hard-coded reserved zero, so a unit predating this decodes as
// count 0 / armed — the honest "nothing to report" for the campaign window in
// which the wall runs both firmwares. Reply LENGTH is unchanged, so every
// mixed-firmware rejection rule below is untouched.
#define LIFETIME_FUTILE_REHOME_MAX  0x7F
#define LIFETIME_STAND_DOWN_BIT     0x80

struct UnitLifetimeFacts {
  uint8_t  layoutVersion = 0;
  uint8_t  homeFailedCount = 0;
  uint8_t  featureGates = 0;
  uint8_t  idleHallFutileRehomes = 0;  // since boot, saturating at the field
  bool     idleHallStoodDown = false;  // the check is disarmed for this boot
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
  buf[3] = (uint8_t)((f.idleHallFutileRehomes > LIFETIME_FUTILE_REHOME_MAX
                          ? LIFETIME_FUTILE_REHOME_MAX
                          : f.idleHallFutileRehomes) |
                     (f.idleHallStoodDown ? LIFETIME_STAND_DOWN_BIT : 0));
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
  out.idleHallFutileRehomes = (uint8_t)(buf[3] & LIFETIME_FUTILE_REHOME_MAX);
  out.idleHallStoodDown = (buf[3] & LIFETIME_STAND_DOWN_BIT) != 0;
  out.stepExcessLifetimeMax = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
  out.selfTestFirstHallWindow = (uint16_t)buf[6] | ((uint16_t)buf[7] << 8);
  out.selfTestFirstStepsPerRev = (uint16_t)buf[8] | ((uint16_t)buf[9] << 8);
  out.selfTestLastHallWindow = (uint16_t)buf[10] | ((uint16_t)buf[11] << 8);
  out.selfTestLastStepsPerRev = (uint16_t)buf[12] | ((uint16_t)buf[13] << 8);
  return true;
}
