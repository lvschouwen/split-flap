#pragma once
// Pure odometer logic for the unit's revolution counter (#231) — step
// accumulation, EEPROM ring rotation and boot recovery, natively tested by
// test_odometer. The EEPROM/I2C glue lives in the .ino files; the ring's
// base address and the rest of the EEPROM map live in UnitEeprom.h.
//
// Wear model: one revolution = one full drum rotation (STEPS stepper steps,
// passed in by the caller so this header stays free of sketch globals).
//
// The count is persisted every ODO_PERSIST_INTERVAL_REVS revolutions into a
// ring of ODO_RING_SLOTS slots — rotating the write across slots multiplies
// EEPROM endurance by the slot count. Boot takes the ring maximum: counts
// are monotonic, so the largest valid slot is always the newest.
//
// Interval 1 means the persist boundary is unreachable: below it a count did
// not merely undercount, it reset to 0 at every power loss, and a rotation
// only happened after 128 revolutions no unit on the wall had ever reached.
// That half of #406 was right and is not up for revisiting.
//
// SIZING (#463). The ring was 128 slots — 640 of the part's 1024 bytes, 62.5%
// of the device — justified as "1,280x WearPolicy.h's 10,000-revolution flag
// threshold". That flag is not a life expectancy: WearPolicy.h states plainly
// that nobody has real 28BYJ-48 flap-drum life data and that 10,000 is a
// floor to keep young displays quiet. A margin taken against an invented
// number cannot be falsified, which is how a counter came to own most of the
// EEPROM — it was sized to the space that happened to be free.
//
// The anchor is the drum, not a calendar. One revolution is ~3.4 s of motion,
// so a generous thousand-hour gearbox is on the order of 1e6 revolutions and
// the flap tabs give out before the gears do. 16 slots x 100k writes x 1 rev
// = 1.6 M revolutions: one to two drum lifetimes, and still a power of two so
// the slot index stays a mask rather than a division on an 8-bit MCU.
//
// Not 8 (800 k). The case that burns writes fastest is a mechanically failing
// drum drifting and re-homing constantly — GENUINE drift, so #268's
// futile-rehome standdown never fires — and that is precisely the unit whose
// history is worth keeping.

#include <stdint.h>
#include <stdlib.h>

#define ODO_RING_SLOTS            16
#define ODO_PERSIST_INTERVAL_REVS 1

// Slots are INTERLEAVED: count and checksum adjacent, so a persist is one
// contiguous 5-byte write and the slot address is a single base-plus-stride
// rather than two writes at distant addresses.
#define ODO_SLOT_STRIDE           5
#define ODO_RING_BYTES            (ODO_RING_SLOTS * ODO_SLOT_STRIDE)

// Every byte the ring has ever occupied, across every geometry this firmware
// has shipped — #406's 128-slot ring is the widest so far. The one-shot
// self-heal sweep (#417) clears THIS extent, never the live ring: shrinking
// strands old slots past the new end, they still satisfy their own checksums,
// and a later GROW would read them straight back into range. That is #417's
// own bug re-armed by construction, so the extent only ever grows.
#define ODO_RING_SWEEP_BYTES      640

static_assert(ODO_RING_SWEEP_BYTES >= ODO_RING_BYTES,
              "the sweep must cover at least the live ring");

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

// Byte offset of a slot within the ring (add the ring base to address EEPROM).
inline uint16_t odometerSlotOffset(uint8_t slot) {
  return (uint16_t)slot * ODO_SLOT_STRIDE;
}

inline bool odometerShouldPersist(uint32_t revolutions,
                                  uint32_t lastPersisted) {
  return revolutions - lastPersisted >= ODO_PERSIST_INTERVAL_REVS;
}

// Per-slot EEPROM integrity (#354): each ring slot carries a masked XOR
// checksum byte. A power-loss-torn slot write yields a value whose checksum
// no longer matches — the slot is skipped instead of a large garbage count
// being adopted at boot. Mask keeps the zero count's byte away from
// 0x00/0xFF (erased EEPROM).
#define ODO_SLOT_CHECKSUM_MASK 0x3C

inline uint8_t odometerSlotChecksum(uint32_t value) {
  uint8_t x = (uint8_t)(value & 0xFF) ^ (uint8_t)((value >> 8) & 0xFF) ^
              (uint8_t)((value >> 16) & 0xFF) ^ (uint8_t)((value >> 24) & 0xFF);
  return (uint8_t)(x ^ ODO_SLOT_CHECKSUM_MASK);
}

// Boot recovery, folded one slot at a time as the sketch reads them. A
// 128-slot ring read at once would need a 512-byte stack buffer on a 2 KB
// Nano; this keeps recovery at O(1) RAM for any ring size.
//
// 0xFFFFFFFF stays excluded UNCONDITIONALLY (the #139 lesson) — erased
// EEPROM whose checksum byte happens to match must not resurrect as a
// 4-billion count that trips the wear alert forever.
struct OdometerBootScan {
  uint32_t best;
};

inline void odometerBootScanInit(OdometerBootScan& s) { s.best = 0; }

inline void odometerBootScanSlot(OdometerBootScan& s, uint32_t value,
                                 uint8_t checksum) {
  if (value != 0xFFFFFFFFUL && checksum == odometerSlotChecksum(value) &&
      value > s.best) {
    s.best = value;
  }
}

inline uint32_t odometerBootScanResult(const OdometerBootScan& s) {
  return s.best;
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
