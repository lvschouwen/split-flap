#pragma once
// Pure EEPROM layout + block-integrity logic for the Nano unit (#406),
// natively tested by test_eeprom_layout. The EEPROM read/write glue lives in
// the .ino files; nothing here touches hardware.
//
// ---------------------------------------------------------------------------
// ATmega328P EEPROM — 1024 B
//
//  -- identity --------------------------------------------------------------
//    0       u8    layoutVersion      != UNIT_EE_LAYOUT_VERSION -> blank, init
//    1       u8    i2cAddress         1..126, else DIP fallback
//    2       u8    flags              bit0 = address provisioned
//    3       u8    checksum over 0..2
//
//  -- calibration -----------------------------------------------------------
//    4..5    i16   calibrationOffset  clamped +/-SFP_OFFSET_LIMIT_STEPS
//    6       u8    checksum over 4..5
//    7       -     reserved
//
//  -- lifetime health -------------------------------------------------------
//    8       u8    brownoutCount             saturating
//    9       u8    watchdogCount             saturating
//   10       u8    homeFailedCount           saturating
//   11       u8    featureGates              UNIT_GATE_* (#407)
//   12..13   u16   stepExcessLifetimeMax
//   14..15   u16   selfTestFirstHallWindow
//   16..17   u16   selfTestFirstStepsPerRev
//   18..19   u16   selfTestLastHallWindow
//   20..21   u16   selfTestLastStepsPerRev
//   22       u8    checksum over 8..21
//   23       -     reserved
//
//  -- reserved scalars ------------------------------------------------------
//   24..63         40 B — future fields land here, the ring never moves again
//
//  -- odometer ring ---------------------------------------------------------
//   64..703        ODO_RING_SLOTS x ODO_SLOT_STRIDE, interleaved
//                  (geometry + policy in UnitOdometer.h)
//
//  -- free ------------------------------------------------------------------
//  704..1023       320 B
// ---------------------------------------------------------------------------
//
// Three properties this layout is built for:
//
//   ONE VERSION BYTE, NOT THREE MAGICS. Erased EEPROM reads 0xFF on every
//   byte, so blank detection needs no magic constant at all — anything that
//   is not the current version is blank and gets initialised.
//
//   THE RING GOES LAST. It used to start at byte 8, so any new scalar shoved
//   it and forced another re-layout. With 40 bytes of headroom ahead of it,
//   that never happens again.
//
//   EVERY BLOCK CHECKSUMMED, AND FAILURE IS SAFE BY CONSTRUCTION. The I2C
//   address used to be guarded only by a magic byte, so a corrupted address
//   with an intact magic silently stranded the unit behind twiboot's
//   DIP-derived address — a physical trip to recover. A blank or
//   failed-checksum identity block now falls back to DIP, which is always
//   reachable. Per-block masks are distinct so a block read at the wrong
//   offset cannot validate.
//
// Introduced as a day-0 erase-and-initialise (#406): these are not production
// units, so there is no migration path, no magic chain and no
// interrupted-migration recovery. The one thing that does not survive an
// erase is the per-unit calibration offset — restore list lives in #407.

#include <stdint.h>

#include "UnitOdometer.h"

#define UNIT_EE_LAYOUT_VERSION 1
#define EE_SIZE                1024

// --- identity ---------------------------------------------------------------
#define EE_LAYOUT_VERSION       0
#define EE_I2C_ADDRESS          1
#define EE_ID_FLAGS             2
#define EE_ID_CHECKSUM          3
#define EE_ID_BLOCK_LEN         4
#define EE_ID_FLAG_PROVISIONED  0x01
#define EE_ID_CHECKSUM_MASK     0x5A

// --- calibration ------------------------------------------------------------
#define EE_CAL_OFFSET           4
#define EE_CAL_CHECKSUM         6
#define EE_CAL_BLOCK_LEN        3
#define EE_CAL_CHECKSUM_MASK    0x2D

// --- lifetime health --------------------------------------------------------
#define EE_HEALTH_BASE            8
#define EE_HEALTH_LEN             14  // payload bytes 8..21
#define EE_HEALTH_CHECKSUM        22
#define EE_HEALTH_BLOCK_LEN       (EE_HEALTH_LEN + 1)
#define EE_HEALTH_CHECKSUM_MASK   0x71

// --- reserved + ring --------------------------------------------------------
#define EE_RESERVED_BASE        24
#define EE_ODO_RING_BASE        64

static_assert(EE_ODO_RING_BASE + ODO_RING_BYTES <= EE_SIZE,
              "odometer ring must fit the ATmega328P EEPROM");
static_assert(EE_RESERVED_BASE <= EE_ODO_RING_BASE,
              "reserved scalars must sit ahead of the ring");

// Runtime feature gates (#407), byte 11. The epic lands five changes in one
// physical reflash; the two that alter MOTION behaviour ship OFF so the wall
// can be proven on the low-risk three first, then have each motion change
// switched on over I2C without another reflash.
#define UNIT_GATE_IDLE_HALL_CHECK   0x01  // #268 idle hall consistency check
#define UNIT_GATE_SCHEDULED_REHOME  0x02  // #269 scheduled verification re-home

// Every bit this firmware has code for. SET_GATES (#409) refuses anything
// outside it: a unit must never persist a gate it will not act on, or
// /units/health reports a feature as enabled that does not exist here.
#define UNIT_GATE_ALL \
  (UNIT_GATE_IDLE_HALL_CHECK | UNIT_GATE_SCHEDULED_REHOME)

inline bool unitGateEnabled(uint8_t gates, uint8_t gate) {
  return (gates & gate) != 0;
}

inline bool unitGateBitsKnown(uint8_t gates) {
  return (gates & (uint8_t)~UNIT_GATE_ALL) == 0;
}

// Masked XOR over a block's payload. The mask keeps an all-zero block's
// checksum away from 0x00 and an erased all-0xFF block's away from 0xFF, so
// neither zeroed nor erased EEPROM can present as a valid block.
inline uint8_t unitEeBlockChecksum(const uint8_t* bytes, uint8_t len,
                                   uint8_t mask) {
  uint8_t x = 0;
  for (uint8_t i = 0; i < len; i++) x ^= bytes[i];
  return (uint8_t)(x ^ mask);
}

// Anything that is not the current layout version is blank — erased (0xFF),
// zeroed, or written by a firmware whose layout we do not know.
inline bool unitEeIsBlank(uint8_t layoutVersion) {
  return layoutVersion != UNIT_EE_LAYOUT_VERSION;
}

inline void unitEeIdentityEncode(uint8_t address,
                                 uint8_t block[EE_ID_BLOCK_LEN]) {
  block[0] = UNIT_EE_LAYOUT_VERSION;
  block[1] = address;
  block[2] = EE_ID_FLAG_PROVISIONED;
  block[3] = unitEeBlockChecksum(block, EE_ID_BLOCK_LEN - 1,
                                 EE_ID_CHECKSUM_MASK);
}

// SFP_CMD_CLEAR_I2C_ADDRESS: drop the provisioned flag but leave a VALID
// block behind, so the next boot reads "no address burned" rather than
// "corrupt" — both fall back to DIP, but only one of them is the truth.
inline void unitEeIdentityClear(uint8_t block[EE_ID_BLOCK_LEN]) {
  block[0] = UNIT_EE_LAYOUT_VERSION;
  block[1] = 0;
  block[2] = 0;
  block[3] = unitEeBlockChecksum(block, EE_ID_BLOCK_LEN - 1,
                                 EE_ID_CHECKSUM_MASK);
}

// Resolve the burned I2C address, or 0 meaning "fall back to DIP". Every
// rejection path lands on DIP because twiboot listens there regardless — an
// address we refuse to adopt is always recoverable, an address we wrongly
// adopt is a physical trip.
inline uint8_t unitEeIdentityAddress(const uint8_t block[EE_ID_BLOCK_LEN]) {
  if (unitEeIsBlank(block[0])) return 0;
  if (block[EE_ID_BLOCK_LEN - 1] !=
      unitEeBlockChecksum(block, EE_ID_BLOCK_LEN - 1, EE_ID_CHECKSUM_MASK)) {
    return 0;
  }
  if (!(block[2] & EE_ID_FLAG_PROVISIONED)) return 0;
  // 0 is general call, 127 is reserved; anything else in 1..126 is plausible.
  if (block[1] < 1 || block[1] > 126) return 0;
  return block[1];
}

inline void unitEeCalEncode(int16_t offset, uint8_t block[EE_CAL_BLOCK_LEN]) {
  block[0] = (uint8_t)((uint16_t)offset & 0xFF);
  block[1] = (uint8_t)(((uint16_t)offset >> 8) & 0xFF);
  block[2] = unitEeBlockChecksum(block, EE_CAL_BLOCK_LEN - 1,
                                 EE_CAL_CHECKSUM_MASK);
}

// False means the block is blank or torn; `out` is set to 0 (centre, no
// offset), never to the unreadable value.
inline bool unitEeCalDecode(const uint8_t block[EE_CAL_BLOCK_LEN],
                            int16_t& out) {
  if (block[EE_CAL_BLOCK_LEN - 1] !=
      unitEeBlockChecksum(block, EE_CAL_BLOCK_LEN - 1, EE_CAL_CHECKSUM_MASK)) {
    out = 0;
    return false;
  }
  out = (int16_t)((uint16_t)block[0] | ((uint16_t)block[1] << 8));
  return true;
}

// Lifetime health — what the unit remembers about its own decline across
// power cycles. Unit 0x0f degraded pass -> marginal -> hall-dead in about two
// hours on 2026-07-26 and nothing on the unit recorded the trajectory; its
// healthy hallWindow of 46 survived only in a session note.
struct UnitLifetimeHealth {
  uint8_t  brownoutCount;
  uint8_t  watchdogCount;
  uint8_t  homeFailedCount;
  uint8_t  featureGates;
  uint16_t stepExcessLifetimeMax;
  uint16_t selfTestFirstHallWindow;
  uint16_t selfTestFirstStepsPerRev;
  uint16_t selfTestLastHallWindow;
  uint16_t selfTestLastStepsPerRev;
};

inline void unitEeHealthEncode(const UnitLifetimeHealth& h,
                               uint8_t block[EE_HEALTH_BLOCK_LEN]) {
  block[0] = h.brownoutCount;
  block[1] = h.watchdogCount;
  block[2] = h.homeFailedCount;
  block[3] = h.featureGates;
  block[4] = (uint8_t)(h.stepExcessLifetimeMax & 0xFF);
  block[5] = (uint8_t)((h.stepExcessLifetimeMax >> 8) & 0xFF);
  block[6] = (uint8_t)(h.selfTestFirstHallWindow & 0xFF);
  block[7] = (uint8_t)((h.selfTestFirstHallWindow >> 8) & 0xFF);
  block[8] = (uint8_t)(h.selfTestFirstStepsPerRev & 0xFF);
  block[9] = (uint8_t)((h.selfTestFirstStepsPerRev >> 8) & 0xFF);
  block[10] = (uint8_t)(h.selfTestLastHallWindow & 0xFF);
  block[11] = (uint8_t)((h.selfTestLastHallWindow >> 8) & 0xFF);
  block[12] = (uint8_t)(h.selfTestLastStepsPerRev & 0xFF);
  block[13] = (uint8_t)((h.selfTestLastStepsPerRev >> 8) & 0xFF);
  block[EE_HEALTH_LEN] =
      unitEeBlockChecksum(block, EE_HEALTH_LEN, EE_HEALTH_CHECKSUM_MASK);
}

// False means blank or torn — `out` is zeroed. Adopting the raw bytes is the
// #139 failure: erased EEPROM reads 0xFF everywhere, every freshly
// provisioned unit reported 255/255 saturated resets, and the saturating
// guard then pinned them there forever.
inline bool unitEeHealthDecode(const uint8_t block[EE_HEALTH_BLOCK_LEN],
                               UnitLifetimeHealth& out) {
  if (block[EE_HEALTH_LEN] !=
      unitEeBlockChecksum(block, EE_HEALTH_LEN, EE_HEALTH_CHECKSUM_MASK)) {
    out = UnitLifetimeHealth();
    return false;
  }
  out.brownoutCount = block[0];
  out.watchdogCount = block[1];
  out.homeFailedCount = block[2];
  out.featureGates = block[3];
  out.stepExcessLifetimeMax = (uint16_t)block[4] | ((uint16_t)block[5] << 8);
  out.selfTestFirstHallWindow = (uint16_t)block[6] | ((uint16_t)block[7] << 8);
  out.selfTestFirstStepsPerRev = (uint16_t)block[8] | ((uint16_t)block[9] << 8);
  out.selfTestLastHallWindow = (uint16_t)block[10] | ((uint16_t)block[11] << 8);
  out.selfTestLastStepsPerRev = (uint16_t)block[12] | ((uint16_t)block[13] << 8);
  return true;
}

inline void unitEeBumpSaturating(uint8_t& counter) {
  if (counter < 0xFF) counter++;
}

// Self-test trend: the FIRST valid reading is the baseline the unit compares
// itself against for life and is never overwritten; every later one moves
// `last`. A failed test that measured nothing (0) records nothing — a zero
// must not become the baseline, and must not erase a real last reading.
inline void unitEeRecordSelfTest(UnitLifetimeHealth& h, uint16_t hallWindow,
                                 uint16_t stepsPerRev) {
  if (hallWindow != 0) {
    if (h.selfTestFirstHallWindow == 0) h.selfTestFirstHallWindow = hallWindow;
    h.selfTestLastHallWindow = hallWindow;
  }
  if (stepsPerRev != 0) {
    if (h.selfTestFirstStepsPerRev == 0) h.selfTestFirstStepsPerRev = stepsPerRev;
    h.selfTestLastStepsPerRev = stepsPerRev;
  }
}

// Lifetime high-water mark of the per-move step excess. Returns true only
// when the mark actually moved, so the caller writes EEPROM on a new record
// rather than on every move (`stepExcessMax` in UnitExtDiag.h is the
// since-boot twin — every reboot forgets its drag record).
inline bool unitEeRecordStepExcess(UnitLifetimeHealth& h, uint16_t excess) {
  if (excess <= h.stepExcessLifetimeMax) return false;
  h.stepExcessLifetimeMax = excess;
  return true;
}
