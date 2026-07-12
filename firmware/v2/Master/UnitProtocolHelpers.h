#pragma once

#include <stdint.h>

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
