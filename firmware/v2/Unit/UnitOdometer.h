#pragma once
// Pure odometer logic for the unit's revolution counter (#231) — step
// accumulation, EEPROM ring rotation and boot recovery, natively tested by
// test_odometer. The EEPROM/I2C glue lives in the .ino files.
//
// Wear model: one revolution = one full drum rotation (STEPS stepper steps,
// passed in by the caller so this header stays free of sketch globals).
// The count is persisted every ODO_PERSIST_INTERVAL_REVS revolutions into a
// ring of ODO_RING_SLOTS uint32 EEPROM slots — rotating the write across
// slots multiplies EEPROM endurance by the slot count (16 slots x 100k
// writes x 128 revs ~= 200M revolutions). Boot takes the ring maximum:
// counts are monotonic, so the largest valid slot is always the newest.

#include <stdint.h>
#include <stdlib.h>

#define ODO_RING_SLOTS            16
#define ODO_PERSIST_INTERVAL_REVS 128

struct OdometerState {
  uint32_t revolutions;
  uint16_t stepAccumulator;  // steps toward the next revolution
};

// Fold |steps| into the state; direction is irrelevant to mechanical wear.
inline void odometerAddSteps(OdometerState& s, long steps,
                             uint16_t stepsPerRev) {
  uint32_t magnitude = (uint32_t)(steps < 0 ? -steps : steps);
  uint32_t total = (uint32_t)s.stepAccumulator + magnitude;
  s.revolutions += total / stepsPerRev;
  s.stepAccumulator = (uint16_t)(total % stepsPerRev);
}

// Which ring slot a persist of `revolutions` writes into. Derived from the
// count itself — no separate index byte to wear out or corrupt.
inline uint8_t odometerSlotIndex(uint32_t revolutions) {
  return (uint8_t)((revolutions / ODO_PERSIST_INTERVAL_REVS) % ODO_RING_SLOTS);
}

// Boot recovery: the ring maximum. 0xFFFFFFFF is erased/corrupt EEPROM, not
// a count — trusting it would pin the odometer at 4 billion and trip the
// wear alert forever (the #139 fresh-EEPROM lesson).
inline uint32_t odometerBootValue(const uint32_t slots[ODO_RING_SLOTS]) {
  uint32_t best = 0;
  for (uint8_t i = 0; i < ODO_RING_SLOTS; i++) {
    if (slots[i] != 0xFFFFFFFFUL && slots[i] > best) best = slots[i];
  }
  return best;
}

inline bool odometerShouldPersist(uint32_t revolutions,
                                  uint32_t lastPersisted) {
  return revolutions - lastPersisted >= ODO_PERSIST_INTERVAL_REVS;
}

// SFP_CMD_GET_ODOMETER wire reply: uint32 LE + XOR-of-payload ^ 0xA5.
// A unit running pre-odometer firmware answers the unknown opcode with its
// 1-byte status reply and bus padding — the masked checksum rejects all-0xFF,
// all-0x00 and repeated-status garbage instead of "verifying" it as a tiny
// count (#106 class). Decode lives in the master's UnitProtocolHelpers.h.
#define ODO_REPLY_LEN            5
#define ODO_REPLY_CHECKSUM_MASK  0xA5

inline void odometerEncodeReply(uint32_t revolutions,
                                uint8_t buf[ODO_REPLY_LEN]) {
  buf[0] = (uint8_t)(revolutions & 0xFF);
  buf[1] = (uint8_t)((revolutions >> 8) & 0xFF);
  buf[2] = (uint8_t)((revolutions >> 16) & 0xFF);
  buf[3] = (uint8_t)((revolutions >> 24) & 0xFF);
  buf[4] = (uint8_t)((buf[0] ^ buf[1] ^ buf[2] ^ buf[3]) ^
                     ODO_REPLY_CHECKSUM_MASK);
}
