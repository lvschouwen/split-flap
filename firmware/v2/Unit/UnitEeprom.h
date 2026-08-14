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
//    7       -     padding, deliberately stranded (see below)
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
//   23       -     padding, deliberately stranded (see below)
//
//  -- reserved scalars ------------------------------------------------------
//   24       u8    ringInitVersion           UNIT_EE_RING_INIT_VERSION (#417)
//   25       u8    checksum over 24
//   26..63         38 B — EE_RESERVED_NEXT_FREE, the ring never moves again
//
//  -- odometer ring ---------------------------------------------------------
//   64..143        ODO_RING_SLOTS x ODO_SLOT_STRIDE, interleaved
//                  (geometry + policy in UnitOdometer.h)
//
//  -- free ------------------------------------------------------------------
//  144..1023       880 B — 144..703 returned by the #463 right-sizing; the
//                  self-heal sweep clears that whole span once so no slot of
//                  the old 128-slot geometry survives to be read back
// ---------------------------------------------------------------------------
//
// The properties this layout is built for:
//
//   NO MAGIC CHAIN. Erased EEPROM reads 0xFF on every byte, so blank
//   detection needs no magic constant at all — anything that is not the
//   current version is blank and gets initialised.
//
//   BLOCK-ADDRESSED, NOT VERSION-ADDRESSED. Every block proves itself by
//   checksum, so UNIT_EE_LAYOUT_VERSION separates only "initialised by a
//   firmware whose map we know" from "blank or foreign". It is not an index
//   of which fields exist, and adding a field does not bump it: an
//   un-migrated unit reads the new block as garbage and heals it. Bumping it
//   means ERASE EVERYTHING, which costs 21 hand-measured calibration offsets.
//
//   A REGION EARNS ITS OWN VERSION BYTE only when its repair action is
//   strictly cheaper than that full erase, and only as a self-validating
//   checksummed block. EE_RING_INIT_VERSION is the one that qualifies:
//   re-zeroing the ring costs a revolution count, so coupling it to byte 0
//   would force the expensive repair for the cheap problem. Anything that
//   cannot clear that bar is a field, not a version — otherwise the magic
//   chain comes back one byte at a time.
//
//   THE RING GOES LAST. It used to start at byte 8, so any new scalar shoved
//   it and forced another re-layout. With the reserved scalars ahead of it,
//   that never happens again. A new field lands at EE_RESERVED_NEXT_FREE and
//   moves it; bytes 7 and 23 are NOT free space but padding stranded between
//   blocks, left alone because reclaiming them would move a block for two
//   bytes.
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

// Odometer ring init marker (#417), the first of the reserved scalars.
//
// The ring is the one region the day-0 init never wrote: erased slots read
// 0xFFFFFFFF and the checksummed boot scan rejects them unconditionally, so a
// blank ring already recovers as 0 and zeroing 640 bytes would spend
// endurance to reach the same answer. That holds for a factory-fresh Nano and
// for nothing on this wall — #406 rewrites bytes 0..22 only, while the ring
// moved from 16 slots at byte 8 to 128 at byte 64. The new scan therefore
// reads bytes the OLD ring wrote and the init never cleared; one of those
// stale slots satisfied its own checksum and a1 booted claiming 1010580540
// revolutions, then counted up from there.
//
// A one-shot sweep fixes it, but it cannot hang off UNIT_EE_LAYOUT_VERSION:
// bumping that re-runs the erase, and the one thing an erase does not survive
// is the per-unit calibration offset (#407's restore list) — a re-campaign to
// fix a lifetime counter. So the ring carries its own version marker, here in
// the reserved scalars the erase never touches. A VERSION rather than a flag
// because the next geometry change gets the same free self-heal.
#define EE_RING_INIT_VERSION        24
#define EE_RING_INIT_CHECKSUM       25
#define EE_RING_INIT_BLOCK_LEN      2
#define EE_RING_INIT_CHECKSUM_MASK  0x4B
// 1 = #417's first sweep. 2 = #463 right-sized the ring 128 -> 16 slots, so
// every unit re-sweeps once: the counts are discarded deliberately (a
// prototype wall, and the units ran for months with no odometer at all) and
// the geometry change costs one boot instead of a UNIT_EE_LAYOUT_VERSION bump
// and 21 destroyed calibration offsets. This is the self-heal the marker was
// versioned for.
#define UNIT_EE_RING_INIT_VERSION   2

// Where the NEXT reserved scalar lands, and therefore what is left ahead of
// the ring. Claiming bytes means re-pointing this at the end of the new block
// — the same edit the test's claimed-block table demands, so a field that
// updates only one of the two fails test_eeprom_layout rather than colliding
// on a unit.
#define EE_RESERVED_NEXT_FREE  (EE_RING_INIT_VERSION + EE_RING_INIT_BLOCK_LEN)

static_assert(EE_RESERVED_NEXT_FREE <= EE_ODO_RING_BASE,
              "the reserved scalars have run into the ring");

static_assert(EE_RING_INIT_VERSION >= EE_RESERVED_BASE,
              "the ring marker is a reserved scalar");
static_assert(EE_RING_INIT_VERSION + EE_RING_INIT_BLOCK_LEN <= EE_ODO_RING_BASE,
              "the ring marker must not overlap the ring it guards");

static_assert(EE_ODO_RING_BASE + ODO_RING_BYTES <= EE_SIZE,
              "odometer ring must fit the ATmega328P EEPROM");
static_assert(EE_ODO_RING_BASE + ODO_RING_SWEEP_BYTES <= EE_SIZE,
              "the sweep's historical extent must fit the ATmega328P EEPROM");
static_assert(EE_RESERVED_BASE <= EE_ODO_RING_BASE,
              "reserved scalars must sit ahead of the ring");

// Runtime feature gates (#407), byte 11. The epic lands its changes in one
// physical reflash; the one that alters MOTION behaviour ships OFF so the
// wall can be proven on the low-risk changes first, then have the motion
// change switched on over I2C without another reflash.
//
// 0x02 was allocated to #269's scheduled verification re-home and is RETIRED,
// not reserved (#458): #269 turned out to be entirely master-side — the
// master broadcasts CMD_HOME on its own schedule — so no unit firmware ever
// implemented it, yet UNIT_GATE_ALL admitted the bit. A unit accepted it,
// persisted it, and reported it back through GET_LIFETIME, so the op's
// read-back verification confirmed a feature that did not exist: truthful
// about STORAGE, silent about BEHAVIOUR. The number stays burned here rather
// than in a dead #define, because a constant nothing reads is exactly what
// made that possible. Mirrored by SFP_UNIT_GATE_IMPLEMENTED on the master
// side; widen the two together with the code that honours the bit.
#define UNIT_GATE_IDLE_HALL_CHECK   0x01  // #268 idle hall consistency check

// Every bit this firmware has code for. SET_GATES (#409) refuses anything
// outside it: a unit must never persist a gate it will not act on, or
// /units/health reports a feature as enabled that does not exist here.
#define UNIT_GATE_ALL  (UNIT_GATE_IDLE_HALL_CHECK)

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

inline void unitEeRingInitEncode(uint8_t block[EE_RING_INIT_BLOCK_LEN]) {
  block[0] = UNIT_EE_RING_INIT_VERSION;
  block[1] = unitEeBlockChecksum(block, EE_RING_INIT_BLOCK_LEN - 1,
                                 EE_RING_INIT_CHECKSUM_MASK);
}

// False means "sweep the ring, then stamp this". Everything that is not this
// exact version under a valid checksum asks for the sweep — erased bytes, a
// stale slot from the old ring geometry that happens to sit here, a torn
// write, or a marker from an older geometry.
inline bool unitEeRingInitDone(const uint8_t block[EE_RING_INIT_BLOCK_LEN]) {
  if (block[EE_RING_INIT_BLOCK_LEN - 1] !=
      unitEeBlockChecksum(block, EE_RING_INIT_BLOCK_LEN - 1,
                          EE_RING_INIT_CHECKSUM_MASK)) {
    return false;
  }
  return block[0] == UNIT_EE_RING_INIT_VERSION;
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
