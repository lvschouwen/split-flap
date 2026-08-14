// Host-side unit tests for the pure odometer logic in UnitOdometer.h
// (#231, re-geometried by #406): step->revolution accumulation, EEPROM ring
// slot rotation, and boot-time recovery. The EEPROM/I2C glue in the .ino
// files is bench tier.

#include <unity.h>
#include <stdint.h>
#include "../../UnitOdometer.h"

// The unit's real steps-per-revolution (Unit.ino STEPS). Tests pass it
// explicitly — the header stays pure and never includes sketch globals.
static const uint16_t kStepsPerRev = 2038;

void setUp() {}
void tearDown() {}

// --- odometerAddSteps: step accumulation -> revolutions -----------------

static void test_add_steps_below_one_rev_accumulates_only() {
  OdometerState s = {0, 0};
  odometerAddSteps(s, 2037, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT32(0, s.revolutions);
  TEST_ASSERT_EQUAL_UINT16(2037, s.stepAccumulator);
}

static void test_add_steps_rolls_over_at_steps_per_rev() {
  OdometerState s = {0, 0};
  odometerAddSteps(s, 2037, kStepsPerRev);
  odometerAddSteps(s, 3, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT32(1, s.revolutions);
  TEST_ASSERT_EQUAL_UINT16(2, s.stepAccumulator);
}

static void test_add_steps_negative_counts_magnitude() {
  // Jog backwards is still mechanical wear.
  OdometerState s = {0, 0};
  odometerAddSteps(s, -2038, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT32(1, s.revolutions);
  TEST_ASSERT_EQUAL_UINT16(0, s.stepAccumulator);
}

static void test_add_steps_large_move_spans_multiple_revs() {
  OdometerState s = {5, 100};
  odometerAddSteps(s, 3 * 2038 + 50, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT32(8, s.revolutions);
  TEST_ASSERT_EQUAL_UINT16(150, s.stepAccumulator);
}

// --- #406 geometry --------------------------------------------------------
// The measured failure this re-geometry fixes: 19 of 21 units had never
// crossed the old 128-revolution persist boundary in their lives, so their
// count was not merely undercounted but total — reset to 0 by every power
// loss. Persisting every revolution makes the boundary unreachable.

static void test_persists_every_revolution() {
  TEST_ASSERT_EQUAL_UINT32(1, ODO_PERSIST_INTERVAL_REVS);
  TEST_ASSERT_TRUE(odometerShouldPersist(1, 0));
  TEST_ASSERT_FALSE(odometerShouldPersist(1, 1));
  TEST_ASSERT_TRUE(odometerShouldPersist(2, 1));
}

static void test_ring_endurance_outlives_the_drum_it_measures() {
  // The bar here used to be "1000x the 10,000-revolution wear flag", and that
  // is what sized the ring at 62.5% of the EEPROM (#463). The flag is not a
  // life expectancy: WearPolicy.h says plainly that nobody has real 28BYJ-48
  // flap-drum life data and that 10,000 is a floor to keep young displays
  // quiet. A margin multiplied against an invented number cannot be wrong,
  // which is exactly why it kept growing.
  //
  // The honest anchor is the drum. One revolution is ~3.4 s of motion; a
  // generous thousand-hour gearbox is on the order of 1e6 revolutions and the
  // flap tabs give out before the gears do. The ring must outlive that with
  // margin, and no more — this bound is a CEILING on ambition as much as a
  // floor on safety, and it is what pins ODO_RING_SLOTS at 16 rather than 8.
  const uint32_t kPlausibleDrumLifeRevs = 1000000UL;
  uint32_t endurance = (uint32_t)ODO_RING_SLOTS * 100000UL * ODO_PERSIST_INTERVAL_REVS;
  TEST_ASSERT_TRUE(endurance >= kPlausibleDrumLifeRevs);
  // And still far clear of the wear flag, so the odometer stays readable
  // across the whole range anyone would act on.
  TEST_ASSERT_TRUE(endurance / 10000UL >= 100UL);
}

static void test_sweep_extent_covers_every_geometry_ever_shipped() {
  // Shrinking the ring strands old slots beyond the live geometry. They still
  // satisfy their own checksums, so a future GROW would read them back into
  // range — #417 re-armed by construction. The one-shot self-heal sweep
  // therefore clears the HIGH-WATER extent, not the live ring.
  TEST_ASSERT_TRUE(ODO_RING_SWEEP_BYTES >= ODO_RING_BYTES);
  TEST_ASSERT_EQUAL_UINT16(640, ODO_RING_SWEEP_BYTES);  // 128 x 5, the widest shipped
}

static void test_ring_slot_count_is_a_power_of_two() {
  // Keeps the slot index a mask rather than a division on an 8-bit MCU.
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)ODO_RING_SLOTS & (ODO_RING_SLOTS - 1));
}

// --- odometerSlotIndex / odometerSlotOffset: ring rotation ----------------

static void test_slot_index_rotates_every_persist_interval() {
  TEST_ASSERT_EQUAL_UINT8(0, odometerSlotIndex(0));
  TEST_ASSERT_EQUAL_UINT8(1, odometerSlotIndex(ODO_PERSIST_INTERVAL_REVS));
  TEST_ASSERT_EQUAL_UINT8(15, odometerSlotIndex(15UL * ODO_PERSIST_INTERVAL_REVS));
}

static void test_slot_index_wraps_after_full_ring() {
  TEST_ASSERT_EQUAL_UINT8(
      0, odometerSlotIndex((uint32_t)ODO_RING_SLOTS * ODO_PERSIST_INTERVAL_REVS));
  TEST_ASSERT_EQUAL_UINT8(
      1, odometerSlotIndex((uint32_t)(ODO_RING_SLOTS + 1) * ODO_PERSIST_INTERVAL_REVS));
}

static void test_slot_offset_is_interleaved_base_plus_stride() {
  // Count and checksum adjacent: one contiguous 5-byte write per persist
  // instead of two writes at distant addresses.
  TEST_ASSERT_EQUAL_UINT16(0, odometerSlotOffset(0));
  TEST_ASSERT_EQUAL_UINT16(ODO_SLOT_STRIDE, odometerSlotOffset(1));
  TEST_ASSERT_EQUAL_UINT16(ODO_SLOT_STRIDE * 5, odometerSlotOffset(5));
  // The last slot's checksum byte must be the ring's last byte.
  TEST_ASSERT_EQUAL_UINT16(ODO_RING_BYTES,
                           odometerSlotOffset(ODO_RING_SLOTS - 1) + ODO_SLOT_STRIDE);
}

// --- slot checksum --------------------------------------------------------

static void test_slot_checksum_masked_xor() {
  TEST_ASSERT_EQUAL_UINT8((0x01 ^ 0x02 ^ 0x03 ^ 0x04) ^ ODO_SLOT_CHECKSUM_MASK,
                          odometerSlotChecksum(0x04030201UL));
  // Zero count: checksum is the bare mask — never 0x00 or 0xFF.
  TEST_ASSERT_EQUAL_UINT8(ODO_SLOT_CHECKSUM_MASK, odometerSlotChecksum(0));
}

// --- odometerBootScan: streaming boot recovery -----------------------------
// #406 grew the ring to 128 slots. The old array-at-once API would have
// needed a 512-byte stack buffer on a 2 KB Nano, so recovery folds slot by
// slot as the .ino reads them: O(1) RAM, same max-of-valid-slots rule.

static void test_boot_scan_is_max_of_valid_slots() {
  OdometerBootScan s;
  odometerBootScanInit(s);
  odometerBootScanSlot(s, 512, odometerSlotChecksum(512));
  odometerBootScanSlot(s, 640, odometerSlotChecksum(640));  // most recent
  odometerBootScanSlot(s, 128, odometerSlotChecksum(128));  // older lap
  TEST_ASSERT_EQUAL_UINT32(640, odometerBootScanResult(s));
}

static void test_boot_scan_empty_ring_reads_zero() {
  OdometerBootScan s;
  odometerBootScanInit(s);
  TEST_ASSERT_EQUAL_UINT32(0, odometerBootScanResult(s));
}

static void test_boot_scan_ignores_torn_garbage() {
  // #354: power loss mid-write leaves a huge garbage value whose checksum
  // byte no longer matches — it must not become the wear count.
  OdometerBootScan s;
  odometerBootScanInit(s);
  odometerBootScanSlot(s, 900, odometerSlotChecksum(900));
  odometerBootScanSlot(s, 0x7F00FFFFUL, odometerSlotChecksum(1024));  // torn
  TEST_ASSERT_EQUAL_UINT32(900, odometerBootScanResult(s));
}

static void test_boot_scan_rejects_erased_ff_even_if_checksum_matches() {
  // A slot reading 0xFFFFFFFF is erased EEPROM, not a count — adopting it
  // would pin the odometer at 4 billion and trip the wear alert forever
  // (the #139 fresh-EEPROM lesson). Unconditional, checksum notwithstanding.
  OdometerBootScan s;
  odometerBootScanInit(s);
  odometerBootScanSlot(s, 0xFFFFFFFFUL, 0xFF);
  TEST_ASSERT_EQUAL_UINT32(0, odometerBootScanResult(s));
  odometerBootScanSlot(s, 0xFFFFFFFFUL, odometerSlotChecksum(0xFFFFFFFFUL));
  TEST_ASSERT_EQUAL_UINT32(0, odometerBootScanResult(s));
}

static void test_boot_scan_over_a_blank_day_zero_ring() {
  // Day 0: the whole ring is erased 0xFF. Recovery must read 0, and the
  // first persist then writes slot 0.
  OdometerBootScan s;
  odometerBootScanInit(s);
  for (uint16_t i = 0; i < ODO_RING_SLOTS; i++) {
    odometerBootScanSlot(s, 0xFFFFFFFFUL, 0xFF);
  }
  TEST_ASSERT_EQUAL_UINT32(0, odometerBootScanResult(s));
}

static void test_boot_scan_full_ring_wrap_takes_the_high_lap() {
  // A wrapped ring holds one lap of old counts and one of new; max wins
  // because counts are monotonic. The lap boundary is expressed against the
  // ring size: it used to be a hard-coded 40, which stopped covering the wrap
  // at all the moment the ring was right-sized below that (#463) — the loop
  // simply never reached the old-lap branch.
  const uint16_t newLap = ODO_RING_SLOTS / 2;
  OdometerBootScan s;
  odometerBootScanInit(s);
  for (uint16_t i = 0; i < ODO_RING_SLOTS; i++) {
    uint32_t v = (i < newLap) ? (uint32_t)(1000 + i)
                              : (uint32_t)(1000 - ODO_RING_SLOTS + i);
    odometerBootScanSlot(s, v, odometerSlotChecksum(v));
  }
  TEST_ASSERT_EQUAL_UINT32(1000 + newLap - 1, odometerBootScanResult(s));
}

// --- odometerEncodeReply: I2C wire format ----------------------------------
// 5 bytes: uint32 LE + XOR-of-payload ^ 0xA5. The masked checksum rejects
// what an old unit (unknown opcode -> 1-byte status reply padded by the bus)
// would produce: all-0xFF, all-0x00 and repeated 0x01 all fail (#106 class).

static void test_encode_reply_little_endian_with_checksum() {
  uint8_t buf[5];
  odometerEncodeReply(0x04030201UL, buf);
  TEST_ASSERT_EQUAL_UINT8(0x01, buf[0]);
  TEST_ASSERT_EQUAL_UINT8(0x02, buf[1]);
  TEST_ASSERT_EQUAL_UINT8(0x03, buf[2]);
  TEST_ASSERT_EQUAL_UINT8(0x04, buf[3]);
  TEST_ASSERT_EQUAL_UINT8((0x01 ^ 0x02 ^ 0x03 ^ 0x04) ^ 0xA5, buf[4]);
}

static void test_encode_reply_zero_count_checksum_is_mask() {
  uint8_t buf[5];
  odometerEncodeReply(0, buf);
  TEST_ASSERT_EQUAL_UINT8(0xA5, buf[4]);  // never matches a 0x00-padded reply
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_add_steps_below_one_rev_accumulates_only);
  RUN_TEST(test_add_steps_rolls_over_at_steps_per_rev);
  RUN_TEST(test_add_steps_negative_counts_magnitude);
  RUN_TEST(test_add_steps_large_move_spans_multiple_revs);
  RUN_TEST(test_persists_every_revolution);
  RUN_TEST(test_ring_endurance_outlives_the_drum_it_measures);
  RUN_TEST(test_sweep_extent_covers_every_geometry_ever_shipped);
  RUN_TEST(test_ring_slot_count_is_a_power_of_two);
  RUN_TEST(test_slot_index_rotates_every_persist_interval);
  RUN_TEST(test_slot_index_wraps_after_full_ring);
  RUN_TEST(test_slot_offset_is_interleaved_base_plus_stride);
  RUN_TEST(test_slot_checksum_masked_xor);
  RUN_TEST(test_boot_scan_is_max_of_valid_slots);
  RUN_TEST(test_boot_scan_empty_ring_reads_zero);
  RUN_TEST(test_boot_scan_ignores_torn_garbage);
  RUN_TEST(test_boot_scan_rejects_erased_ff_even_if_checksum_matches);
  RUN_TEST(test_boot_scan_over_a_blank_day_zero_ring);
  RUN_TEST(test_boot_scan_full_ring_wrap_takes_the_high_lap);
  RUN_TEST(test_encode_reply_little_endian_with_checksum);
  RUN_TEST(test_encode_reply_zero_count_checksum_is_mask);
  UNITY_END();
  return 0;
}
