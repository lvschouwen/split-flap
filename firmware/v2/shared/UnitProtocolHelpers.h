#pragma once

#include <stdint.h>

#include "UnitSelfTest.h"  // reply length/mask/vocabulary, defined once (#404)

// Pure logic for the master<->unit I2C wire protocol — v2 copy of v1's
// UnitProtocolHelpers.h (copy policy: fix bugs in both trees). Header-only
// so the native test env (test_unit_health) exercises it without Arduino
// deps.

// Validates a CMD_GET_LETTER readback pair (issue #106). New-firmware units
// reply with 2 bytes: the displayed letter index plus its bitwise complement.
// Old firmware replies with the 1-byte status fallback, so a 2-byte read can
// still contain a stale status byte + bus garbage — the complement check and
// the range check reject those instead of "verifying" against noise.
inline bool letterReadbackValid(uint8_t value, uint8_t complement, uint8_t flapAmount) {
  if ((uint8_t)(value ^ complement) != 0xFF) return false;
  return value < flapAmount;
}

// Validates + decodes a CMD_GET_ODOMETER reply (#231): 4 bytes uint32 LE +
// XOR-of-payload ^ 0xA5 (encode side in the unit's UnitOdometer.h). Old
// firmware answers the unknown opcode with its 1-byte status reply + bus
// padding — the masked checksum rejects all-0xFF, all-0x00 and repeated
// status bytes instead of "verifying" garbage as a count (#106 class).
inline bool odometerReadbackValid(const uint8_t buf[5], uint32_t& out) {
  uint8_t expected = (uint8_t)((buf[0] ^ buf[1] ^ buf[2] ^ buf[3]) ^ 0xA5);
  if (buf[4] != expected) return false;
  out = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
        ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
  return true;
}

// Decoded CMD_GET_DIAG reply (#263/#264) — encode side in the unit's
// UnitDrift.h. physicalLetter is the unit's hall-corrected estimate of the
// drum's PHYSICAL letter (0xFF = position never synced), distinct from
// CMD_GET_LETTER's belief.
struct UnitDiagReading {
  uint8_t physicalLetter = 0xFF;
  uint8_t flags = 0;  // bit0 re-home pending, bit1 position known
  uint8_t driftEvents = 0;
  int8_t lastDriftSteps = 0;
};

// Validates + decodes the 6-byte diag reply: masked XOR checksum (^ 0xB7)
// plus a letter range check — a checksum-valid glitch carrying an
// impossible letter index must not become a phantom mismatch. 0xFF is the
// legitimate "position unknown" sentinel.
inline bool diagReadbackValid(const uint8_t buf[6], uint8_t flapAmount,
                              UnitDiagReading& out) {
  uint8_t expected =
      (uint8_t)((buf[0] ^ buf[1] ^ buf[2] ^ buf[3] ^ buf[4]) ^ 0xB7);
  if (buf[5] != expected) return false;
  if (buf[0] != 0xFF && buf[0] >= flapAmount) return false;
  out.physicalLetter = buf[0];
  out.flags = buf[1];
  out.driftEvents = buf[2];
  out.lastDriftSteps = (int8_t)buf[3];
  return true;
}

// Decoded CMD_GET_SELF_TEST reply (#265) — encode side in the unit's
// UnitSelfTest.h. state vocabulary: 0 never / 1 running / 2 ok / 3 failed.
struct UnitSelfTestReading {
  uint8_t state = 0;
  uint16_t stepsPerRev = 0;
  uint16_t hallWindowSteps = 0;
  uint16_t revTimeMs = 0;
  uint8_t reason = 0;  // SELFTEST_REASON_* (#404)
};

// Lengths, mask and vocabulary all come from UnitSelfTest.h now — this
// decoder used to restate them as magic numbers, which is a format defined
// twice and free to drift.
inline bool selfTestReadbackValid(const uint8_t buf[SELFTEST_REPLY_LEN],
                                  UnitSelfTestReading& out) {
  uint8_t x = 0;
  for (uint8_t i = 0; i < SELFTEST_REPLY_LEN - 1; i++) x ^= buf[i];
  if (buf[SELFTEST_REPLY_LEN - 1] != (uint8_t)(x ^ SELFTEST_REPLY_CHECKSUM_MASK)) {
    return false;
  }
  if (buf[0] > SELFTEST_STATE_FAILED) return false;  // outside the vocabulary
  if (buf[7] > SELFTEST_REASON_REV_INCOMPLETE) return false;
  out.state = buf[0];
  out.stepsPerRev = (uint16_t)buf[1] | ((uint16_t)buf[2] << 8);
  out.hallWindowSteps = (uint16_t)buf[3] | ((uint16_t)buf[4] << 8);
  out.revTimeMs = (uint16_t)buf[5] | ((uint16_t)buf[6] << 8);
  out.reason = buf[7];
  return true;
}
