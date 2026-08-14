// Host-side unit tests for the pure EEPROM layout logic in UnitEeprom.h
// (#406): block checksums, identity resolution with its DIP fallback,
// calibration and lifetime-health encode/decode. The EEPROM read/write glue
// in the .ino files is bench tier.
//
// The day-0 rule under test throughout: a block that is blank or fails its
// checksum degrades to a SAFE default, never to an adopted garbage value.

#include <unity.h>
#include <stdint.h>
#include <string.h>
#include "../../UnitEeprom.h"

void setUp() {}
void tearDown() {}

// --- geometry ------------------------------------------------------------
// The layout's whole point is that the ring sits LAST so future scalars
// never move it again. Pin the boundaries so a careless edit fails here
// rather than by silently overlapping two blocks on a real unit.

// Every byte this layout has claimed, in address order. A new field means a
// new row HERE — that is the single action that lets the guards below see it,
// and it is the same action EE_RESERVED_NEXT_FREE demands of the header, so
// the two cross-check each other. The gaps (bytes 7 and 23) are deliberate
// padding and are claimed by nobody.
struct EeBlock {
  int base;
  int len;
  const char* name;
};

static const EeBlock kClaimedBlocks[] = {
    {EE_LAYOUT_VERSION,    EE_ID_BLOCK_LEN,        "identity"},
    {EE_CAL_OFFSET,        EE_CAL_BLOCK_LEN,       "calibration"},
    {EE_HEALTH_BASE,       EE_HEALTH_BLOCK_LEN,    "lifetime health"},
    {EE_RING_INIT_VERSION, EE_RING_INIT_BLOCK_LEN, "odometer ring marker"},
    {EE_ODO_RING_BASE,     ODO_RING_BYTES,         "odometer ring"},
};
static const int kClaimedBlockCount =
    (int)(sizeof(kClaimedBlocks) / sizeof(kClaimedBlocks[0]));

// Walk the table rather than ordering a handful of named constants: ordering
// four boundaries treats the whole reserved region as one undivided span, so
// a field claiming a byte another block already holds satisfies every
// assertion and collides only on the wall.
static void test_layout_blocks_do_not_overlap() {
  for (int i = 0; i < kClaimedBlockCount; i++) {
    const EeBlock& a = kClaimedBlocks[i];
    TEST_ASSERT_TRUE_MESSAGE(a.len > 0, a.name);
    TEST_ASSERT_TRUE_MESSAGE(a.base + a.len <= EE_SIZE, a.name);
    for (int j = i + 1; j < kClaimedBlockCount; j++) {
      const EeBlock& b = kClaimedBlocks[j];
      bool disjoint = (a.base + a.len <= b.base) || (b.base + b.len <= a.base);
      TEST_ASSERT_TRUE_MESSAGE(disjoint, a.name);
    }
  }
  // The health checksum must cover exactly bytes EE_HEALTH_BASE..CHECKSUM-1.
  TEST_ASSERT_EQUAL_INT(EE_HEALTH_CHECKSUM, EE_HEALTH_BASE + EE_HEALTH_LEN);
}

// Headroom is what is LEFT, not the distance between two #defines a new field
// does not move: measure from the highest byte actually claimed ahead of the
// ring, so the number counts down as fields land and the test named for the
// invariant is the one that watches it being consumed.
static void test_reserved_scalar_headroom_counts_down_as_fields_land() {
  int highestClaimedEnd = EE_RESERVED_BASE;
  for (int i = 0; i < kClaimedBlockCount; i++) {
    const EeBlock& b = kClaimedBlocks[i];
    if (b.base >= EE_ODO_RING_BASE) continue;  // the ring itself
    if (b.base < EE_RESERVED_BASE) continue;   // blocks ahead of the region
    if (b.base + b.len > highestClaimedEnd) highestClaimedEnd = b.base + b.len;
  }
  // The header states where the next field lands; the table states what is
  // already taken. Either one going stale on its own fails here.
  TEST_ASSERT_EQUAL_INT(EE_RESERVED_NEXT_FREE, highestClaimedEnd);
  TEST_ASSERT_EQUAL_INT(38, EE_ODO_RING_BASE - highestClaimedEnd);
}

static void test_ring_fits_the_device() {
  TEST_ASSERT_TRUE(EE_ODO_RING_BASE + ODO_RING_BYTES <= EE_SIZE);
}

static void test_the_ring_does_not_dominate_the_device() {
  // #463: the ring once took 640 of 1024 bytes because it was sized to the
  // space that happened to be free rather than to a requirement. A counter is
  // not entitled to most of the device — if this ever trips again, the
  // question to ask is what the endurance is being measured against.
  TEST_ASSERT_TRUE(ODO_RING_BYTES * 4 <= EE_SIZE);
}

// The one-shot sweep must clear every geometry this firmware has ever
// shipped, and that extent still has to fit the part.
static void test_sweep_extent_fits_the_device() {
  TEST_ASSERT_TRUE(EE_ODO_RING_BASE + ODO_RING_SWEEP_BYTES <= EE_SIZE);
}

// --- block checksum ------------------------------------------------------

static void test_block_checksum_is_masked_xor() {
  const uint8_t b[3] = {0x01, 0x02, 0x03};
  TEST_ASSERT_EQUAL_UINT8((0x01 ^ 0x02 ^ 0x03) ^ 0x5A,
                          unitEeBlockChecksum(b, 3, 0x5A));
}

static void test_block_checksum_of_zeros_is_the_bare_mask() {
  // Keeps an all-zero block's checksum away from 0x00, and an all-0xFF
  // erased block away from 0xFF — neither erased nor zeroed EEPROM may
  // accidentally present as a valid block.
  const uint8_t zeros[4] = {0, 0, 0, 0};
  TEST_ASSERT_EQUAL_UINT8(0x5A, unitEeBlockChecksum(zeros, 4, 0x5A));
}

// EVERY mask, in one test. Distinct per block is what stops a block read at
// the wrong offset from validating — the property the whole layout rests on —
// so a mask that only some other test knows about is a hole in it. The ring
// marker's byte 24 is the live case: it sits where the old 16-slot ring used
// to live, so a stale slot's bytes are exactly what its mask must reject.
static void test_block_masks_are_distinct() {
  const uint8_t masks[] = {
      EE_ID_CHECKSUM_MASK,        EE_CAL_CHECKSUM_MASK,
      EE_HEALTH_CHECKSUM_MASK,    EE_RING_INIT_CHECKSUM_MASK,
      ODO_SLOT_CHECKSUM_MASK,
  };
  const int n = (int)(sizeof(masks) / sizeof(masks[0]));
  for (int i = 0; i < n; i++) {
    for (int j = i + 1; j < n; j++) {
      TEST_ASSERT_TRUE(masks[i] != masks[j]);
    }
  }
}

// --- identity: address resolution + DIP fallback --------------------------
// The failure mode being designed out (#406/#405): a corrupted address with
// an intact magic silently strands the unit behind twiboot's DIP address.
// Returning 0 here means "fall back to DIP", which is always reachable.

static void test_identity_roundtrip_returns_the_burned_address() {
  uint8_t block[EE_ID_BLOCK_LEN];
  unitEeIdentityEncode(0x0F, block);
  TEST_ASSERT_EQUAL_UINT8(UNIT_EE_LAYOUT_VERSION, block[0]);
  TEST_ASSERT_EQUAL_UINT8(0x0F, unitEeIdentityAddress(block));
}

static void test_identity_erased_eeprom_falls_back_to_dip() {
  uint8_t block[EE_ID_BLOCK_LEN];
  memset(block, 0xFF, sizeof(block));
  TEST_ASSERT_EQUAL_UINT8(0, unitEeIdentityAddress(block));
}

static void test_identity_zeroed_eeprom_falls_back_to_dip() {
  uint8_t block[EE_ID_BLOCK_LEN] = {0, 0, 0, 0};
  TEST_ASSERT_EQUAL_UINT8(0, unitEeIdentityAddress(block));
}

static void test_identity_bad_checksum_falls_back_to_dip() {
  uint8_t block[EE_ID_BLOCK_LEN];
  unitEeIdentityEncode(0x0F, block);
  block[1] = 0x21;  // address corrupted in place, checksum now stale
  TEST_ASSERT_EQUAL_UINT8(0, unitEeIdentityAddress(block));
}

static void test_identity_unprovisioned_flag_falls_back_to_dip() {
  uint8_t block[EE_ID_BLOCK_LEN];
  unitEeIdentityEncode(0x0F, block);
  unitEeIdentityClear(block);  // what CLEAR_I2C_ADDRESS writes
  TEST_ASSERT_EQUAL_UINT8(0, unitEeIdentityAddress(block));
  // ...and the cleared block is still a VALID block, not corruption.
  TEST_ASSERT_EQUAL_UINT8(UNIT_EE_LAYOUT_VERSION, block[0]);
}

static void test_identity_rejects_reserved_addresses() {
  // 0 is general call, 127 is reserved. A checksum-valid block carrying one
  // of them is still not an address we may adopt.
  for (uint8_t bad = 0; bad < 2; bad++) {
    uint8_t block[EE_ID_BLOCK_LEN];
    unitEeIdentityEncode(bad ? 127 : 0, block);
    TEST_ASSERT_EQUAL_UINT8(0, unitEeIdentityAddress(block));
  }
}

static void test_identity_foreign_layout_version_falls_back_to_dip() {
  uint8_t block[EE_ID_BLOCK_LEN];
  unitEeIdentityEncode(0x0F, block);
  block[0] = UNIT_EE_LAYOUT_VERSION + 1;
  block[EE_ID_BLOCK_LEN - 1] =
      unitEeBlockChecksum(block, EE_ID_BLOCK_LEN - 1, EE_ID_CHECKSUM_MASK);
  TEST_ASSERT_EQUAL_UINT8(0, unitEeIdentityAddress(block));
}

// --- blank detection ------------------------------------------------------

static void test_blank_detection_needs_no_magic_constant() {
  TEST_ASSERT_TRUE(unitEeIsBlank(0xFF));  // erased
  TEST_ASSERT_TRUE(unitEeIsBlank(0x00));
  TEST_ASSERT_TRUE(unitEeIsBlank(UNIT_EE_LAYOUT_VERSION + 1));
  TEST_ASSERT_FALSE(unitEeIsBlank(UNIT_EE_LAYOUT_VERSION));
}

// --- calibration ----------------------------------------------------------

static void test_calibration_roundtrip_signed() {
  uint8_t block[EE_CAL_BLOCK_LEN];
  int16_t out = 0;
  unitEeCalEncode(-1234, block);
  TEST_ASSERT_TRUE(unitEeCalDecode(block, out));
  TEST_ASSERT_EQUAL_INT16(-1234, out);
}

static void test_calibration_bad_checksum_reports_failure() {
  uint8_t block[EE_CAL_BLOCK_LEN];
  int16_t out = 4242;
  unitEeCalEncode(80, block);
  block[0] ^= 0x40;
  TEST_ASSERT_FALSE(unitEeCalDecode(block, out));
  TEST_ASSERT_EQUAL_INT16(0, out);  // safe default, not the torn value
}

static void test_calibration_erased_eeprom_reports_failure() {
  uint8_t block[EE_CAL_BLOCK_LEN];
  int16_t out = 4242;
  memset(block, 0xFF, sizeof(block));
  TEST_ASSERT_FALSE(unitEeCalDecode(block, out));
  TEST_ASSERT_EQUAL_INT16(0, out);
}

// --- lifetime health ------------------------------------------------------

static void test_health_roundtrip_all_fields() {
  UnitLifetimeHealth in = {};
  in.brownoutCount = 3;
  in.watchdogCount = 7;
  in.homeFailedCount = 11;
  in.featureGates = UNIT_GATE_IDLE_HALL_CHECK;
  in.stepExcessLifetimeMax = 4001;
  in.selfTestFirstHallWindow = 46;
  in.selfTestFirstStepsPerRev = 2038;
  in.selfTestLastHallWindow = 12;
  in.selfTestLastStepsPerRev = 2044;

  uint8_t block[EE_HEALTH_BLOCK_LEN];
  unitEeHealthEncode(in, block);

  UnitLifetimeHealth out = {};
  TEST_ASSERT_TRUE(unitEeHealthDecode(block, out));
  TEST_ASSERT_EQUAL_UINT8(3, out.brownoutCount);
  TEST_ASSERT_EQUAL_UINT8(7, out.watchdogCount);
  TEST_ASSERT_EQUAL_UINT8(11, out.homeFailedCount);
  TEST_ASSERT_EQUAL_UINT8(UNIT_GATE_IDLE_HALL_CHECK, out.featureGates);
  TEST_ASSERT_EQUAL_UINT16(4001, out.stepExcessLifetimeMax);
  TEST_ASSERT_EQUAL_UINT16(46, out.selfTestFirstHallWindow);
  TEST_ASSERT_EQUAL_UINT16(2038, out.selfTestFirstStepsPerRev);
  TEST_ASSERT_EQUAL_UINT16(12, out.selfTestLastHallWindow);
  TEST_ASSERT_EQUAL_UINT16(2044, out.selfTestLastStepsPerRev);
}

static void test_health_erased_block_decodes_to_zeros_not_255() {
  // The #139 lesson, restated: erased EEPROM reads 0xFF on every byte, and
  // adopting that pinned every fresh unit at 255/255 saturated resets.
  uint8_t block[EE_HEALTH_BLOCK_LEN];
  memset(block, 0xFF, sizeof(block));
  UnitLifetimeHealth out = {};
  out.brownoutCount = 9;  // must be overwritten, not left dirty
  TEST_ASSERT_FALSE(unitEeHealthDecode(block, out));
  TEST_ASSERT_EQUAL_UINT8(0, out.brownoutCount);
  TEST_ASSERT_EQUAL_UINT8(0, out.watchdogCount);
  TEST_ASSERT_EQUAL_UINT8(0, out.homeFailedCount);
  TEST_ASSERT_EQUAL_UINT8(0, out.featureGates);
  TEST_ASSERT_EQUAL_UINT16(0, out.stepExcessLifetimeMax);
}

static void test_health_bad_checksum_decodes_to_zeros() {
  UnitLifetimeHealth in = {};
  in.brownoutCount = 5;
  in.stepExcessLifetimeMax = 1000;
  uint8_t block[EE_HEALTH_BLOCK_LEN];
  unitEeHealthEncode(in, block);
  block[4] ^= 0x08;

  UnitLifetimeHealth out = {};
  TEST_ASSERT_FALSE(unitEeHealthDecode(block, out));
  TEST_ASSERT_EQUAL_UINT8(0, out.brownoutCount);
  TEST_ASSERT_EQUAL_UINT16(0, out.stepExcessLifetimeMax);
}

static void test_health_ships_with_the_motion_gate_off() {
  // Epic #407's risk defusal: one reflash, motion behaviour OFF, switched on
  // over the wire once the wall is proven.
  UnitLifetimeHealth fresh = {};
  TEST_ASSERT_FALSE(unitGateEnabled(fresh.featureGates, UNIT_GATE_IDLE_HALL_CHECK));
}

static void test_gate_lookup_isolates_the_bit_it_is_asked_about() {
  uint8_t gates = 0;
  TEST_ASSERT_FALSE(unitGateEnabled(gates, UNIT_GATE_IDLE_HALL_CHECK));
  gates |= UNIT_GATE_IDLE_HALL_CHECK;
  TEST_ASSERT_TRUE(unitGateEnabled(gates, UNIT_GATE_IDLE_HALL_CHECK));
  // A neighbouring bit must never read through as this one.
  TEST_ASSERT_FALSE(unitGateEnabled(0xFE, UNIT_GATE_IDLE_HALL_CHECK));
}

// #269's scheduled verification re-home turned out to be a MASTER-side
// feature — the master broadcasts CMD_HOME — so 0x02 never needed unit
// behaviour and is RETIRED rather than reserved (#458). The masters already
// refuse to send it; this is the unit's own half of that refusal, so a unit
// cannot persist and then report through GET_LIFETIME a feature that exists
// nowhere in this firmware.
static void test_the_retired_scheduled_rehome_bit_is_refused() {
  TEST_ASSERT_FALSE(unitGateBitsKnown(0x02));
  TEST_ASSERT_FALSE(unitGateBitsKnown(UNIT_GATE_IDLE_HALL_CHECK | 0x02));
}

// A unit must never persist a bit its own firmware has no code for (#409):
// /units/health would then report a feature as enabled that does not exist
// here, and a SET_GATES from a newer master would look like it landed.
static void test_only_gates_this_firmware_implements_are_accepted() {
  TEST_ASSERT_TRUE(unitGateBitsKnown(0));
  TEST_ASSERT_TRUE(unitGateBitsKnown(UNIT_GATE_IDLE_HALL_CHECK));
  TEST_ASSERT_TRUE(unitGateBitsKnown(UNIT_GATE_ALL));
  TEST_ASSERT_FALSE(unitGateBitsKnown(0x04));
  TEST_ASSERT_FALSE(unitGateBitsKnown(0xFF));
  TEST_ASSERT_FALSE(unitGateBitsKnown(UNIT_GATE_IDLE_HALL_CHECK | 0x80));
}

// --- odometer ring init marker (#417) --------------------------------------
// The day-0 init (#406) deliberately left the ring unwritten, reasoning that
// erased slots read 0xFFFFFFFF and are rejected anyway. True of a factory-
// fresh Nano, false of every unit on this wall: #406 rewrites bytes 0..22
// only, and the ring moved from 16 slots at byte 8 to 128 slots at byte 64 —
// so the new scan reads EEPROM the old ring wrote and the init never cleared.
// One stale slot satisfied its checksum and a1 booted claiming 1010580540
// revolutions.
//
// The sweep that fixes it cannot hang off UNIT_EE_LAYOUT_VERSION: bumping
// that re-runs the erase, which destroys every calibration offset again
// (#407's restore list). So the ring carries its OWN marker, in the reserved
// scalars that #406 never touched.

static void test_ring_init_marker_roundtrip() {
  uint8_t block[EE_RING_INIT_BLOCK_LEN];
  unitEeRingInitEncode(block);
  TEST_ASSERT_TRUE(unitEeRingInitDone(block));
}

static void test_erased_eeprom_asks_for_the_sweep() {
  // The wall's units: reserved bytes never written, so 0xFF or old ring data.
  uint8_t block[EE_RING_INIT_BLOCK_LEN];
  memset(block, 0xFF, sizeof(block));
  TEST_ASSERT_FALSE(unitEeRingInitDone(block));
}

static void test_zeroed_eeprom_asks_for_the_sweep() {
  uint8_t block[EE_RING_INIT_BLOCK_LEN] = {0};
  TEST_ASSERT_FALSE(unitEeRingInitDone(block));
}

static void test_torn_marker_asks_for_the_sweep() {
  uint8_t block[EE_RING_INIT_BLOCK_LEN];
  unitEeRingInitEncode(block);
  block[EE_RING_INIT_BLOCK_LEN - 1] ^= 0x08;
  TEST_ASSERT_FALSE(unitEeRingInitDone(block));
}

static void test_a_foreign_marker_version_asks_for_the_sweep() {
  // Why a VERSION and not a flag: if the ring geometry ever moves again, this
  // bumps and every unit self-heals once more — without touching
  // UNIT_EE_LAYOUT_VERSION and therefore without erasing calibration.
  uint8_t block[EE_RING_INIT_BLOCK_LEN];
  block[0] = (uint8_t)(UNIT_EE_RING_INIT_VERSION + 1);
  block[1] = unitEeBlockChecksum(block, EE_RING_INIT_BLOCK_LEN - 1,
                                 EE_RING_INIT_CHECKSUM_MASK);
  TEST_ASSERT_FALSE(unitEeRingInitDone(block));
}

// The wall is on marker version 1 (#417). #463 shrinks the ring, so every
// unit must sweep exactly once more — which is the whole point of versioning
// the marker rather than flagging it: a geometry change costs one boot, not a
// UNIT_EE_LAYOUT_VERSION bump and 21 destroyed calibration offsets.
static void test_the_previous_marker_version_asks_for_the_sweep() {
  uint8_t block[EE_RING_INIT_BLOCK_LEN];
  block[0] = 1;
  block[1] = unitEeBlockChecksum(block, EE_RING_INIT_BLOCK_LEN - 1,
                                 EE_RING_INIT_CHECKSUM_MASK);
  TEST_ASSERT_FALSE(unitEeRingInitDone(block));
}

static void test_marker_sits_in_the_reserved_region_ahead_of_the_ring() {
  TEST_ASSERT_TRUE(EE_RING_INIT_VERSION >= EE_RESERVED_BASE);
  TEST_ASSERT_TRUE(EE_RING_INIT_VERSION + EE_RING_INIT_BLOCK_LEN <=
                   EE_ODO_RING_BASE);
}

// --- saturating counters --------------------------------------------------

static void test_counters_saturate_rather_than_wrap() {
  uint8_t c = 254;
  unitEeBumpSaturating(c);
  TEST_ASSERT_EQUAL_UINT8(255, c);
  unitEeBumpSaturating(c);
  TEST_ASSERT_EQUAL_UINT8(255, c);  // never wraps to 0
}

// --- self-test trend capture ----------------------------------------------
// The a15 lesson: its healthy hall_window of 46 survived only in a session
// note. The FIRST reading is the baseline and must never be overwritten.

static void test_self_test_first_reading_becomes_the_baseline() {
  UnitLifetimeHealth h = {};
  unitEeRecordSelfTest(h, 46, 2038);
  TEST_ASSERT_EQUAL_UINT16(46, h.selfTestFirstHallWindow);
  TEST_ASSERT_EQUAL_UINT16(2038, h.selfTestFirstStepsPerRev);
  TEST_ASSERT_EQUAL_UINT16(46, h.selfTestLastHallWindow);
  TEST_ASSERT_EQUAL_UINT16(2038, h.selfTestLastStepsPerRev);
}

static void test_self_test_later_readings_only_move_last() {
  UnitLifetimeHealth h = {};
  unitEeRecordSelfTest(h, 46, 2038);
  unitEeRecordSelfTest(h, 12, 2044);
  TEST_ASSERT_EQUAL_UINT16(46, h.selfTestFirstHallWindow);   // baseline held
  TEST_ASSERT_EQUAL_UINT16(2038, h.selfTestFirstStepsPerRev);
  TEST_ASSERT_EQUAL_UINT16(12, h.selfTestLastHallWindow);
  TEST_ASSERT_EQUAL_UINT16(2044, h.selfTestLastStepsPerRev);
}

static void test_self_test_zero_hall_window_is_not_a_baseline() {
  // A failed test that measured nothing must not become the baseline the
  // unit compares its whole life against.
  UnitLifetimeHealth h = {};
  unitEeRecordSelfTest(h, 0, 0);
  TEST_ASSERT_EQUAL_UINT16(0, h.selfTestFirstHallWindow);
  unitEeRecordSelfTest(h, 46, 2038);
  TEST_ASSERT_EQUAL_UINT16(46, h.selfTestFirstHallWindow);
}

static void test_step_excess_max_is_lifetime_high_water() {
  UnitLifetimeHealth h = {};
  TEST_ASSERT_TRUE(unitEeRecordStepExcess(h, 40));
  TEST_ASSERT_EQUAL_UINT16(40, h.stepExcessLifetimeMax);
  TEST_ASSERT_FALSE(unitEeRecordStepExcess(h, 12));  // no EEPROM write
  TEST_ASSERT_EQUAL_UINT16(40, h.stepExcessLifetimeMax);
  TEST_ASSERT_TRUE(unitEeRecordStepExcess(h, 41));
  TEST_ASSERT_EQUAL_UINT16(41, h.stepExcessLifetimeMax);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_layout_blocks_do_not_overlap);
  RUN_TEST(test_reserved_scalar_headroom_counts_down_as_fields_land);
  RUN_TEST(test_ring_fits_the_device);
  RUN_TEST(test_the_ring_does_not_dominate_the_device);
  RUN_TEST(test_sweep_extent_fits_the_device);
  RUN_TEST(test_the_previous_marker_version_asks_for_the_sweep);
  RUN_TEST(test_block_checksum_is_masked_xor);
  RUN_TEST(test_block_checksum_of_zeros_is_the_bare_mask);
  RUN_TEST(test_block_masks_are_distinct);
  RUN_TEST(test_identity_roundtrip_returns_the_burned_address);
  RUN_TEST(test_identity_erased_eeprom_falls_back_to_dip);
  RUN_TEST(test_identity_zeroed_eeprom_falls_back_to_dip);
  RUN_TEST(test_identity_bad_checksum_falls_back_to_dip);
  RUN_TEST(test_identity_unprovisioned_flag_falls_back_to_dip);
  RUN_TEST(test_identity_rejects_reserved_addresses);
  RUN_TEST(test_identity_foreign_layout_version_falls_back_to_dip);
  RUN_TEST(test_blank_detection_needs_no_magic_constant);
  RUN_TEST(test_calibration_roundtrip_signed);
  RUN_TEST(test_calibration_bad_checksum_reports_failure);
  RUN_TEST(test_calibration_erased_eeprom_reports_failure);
  RUN_TEST(test_health_roundtrip_all_fields);
  RUN_TEST(test_health_erased_block_decodes_to_zeros_not_255);
  RUN_TEST(test_health_bad_checksum_decodes_to_zeros);
  RUN_TEST(test_health_ships_with_the_motion_gate_off);
  RUN_TEST(test_gate_lookup_isolates_the_bit_it_is_asked_about);
  RUN_TEST(test_only_gates_this_firmware_implements_are_accepted);
  RUN_TEST(test_the_retired_scheduled_rehome_bit_is_refused);
  RUN_TEST(test_ring_init_marker_roundtrip);
  RUN_TEST(test_erased_eeprom_asks_for_the_sweep);
  RUN_TEST(test_zeroed_eeprom_asks_for_the_sweep);
  RUN_TEST(test_torn_marker_asks_for_the_sweep);
  RUN_TEST(test_a_foreign_marker_version_asks_for_the_sweep);
  RUN_TEST(test_marker_sits_in_the_reserved_region_ahead_of_the_ring);
  RUN_TEST(test_counters_saturate_rather_than_wrap);
  RUN_TEST(test_self_test_first_reading_becomes_the_baseline);
  RUN_TEST(test_self_test_later_readings_only_move_last);
  RUN_TEST(test_self_test_zero_hall_window_is_not_a_baseline);
  RUN_TEST(test_step_excess_max_is_lifetime_high_water);
  UNITY_END();
  return 0;
}
