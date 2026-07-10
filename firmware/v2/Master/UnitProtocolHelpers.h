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
