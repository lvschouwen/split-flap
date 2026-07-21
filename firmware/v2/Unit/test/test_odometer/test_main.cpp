// Host-side unit tests for the pure odometer logic in UnitOdometer.h
// (#231): step->revolution accumulation, EEPROM ring slot rotation, and
// boot-time recovery. The EEPROM/I2C glue in the .ino files is bench tier.

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

// --- odometerSlotIndex: ring rotation ------------------------------------

static void test_slot_index_rotates_every_persist_interval() {
  TEST_ASSERT_EQUAL_UINT8(0, odometerSlotIndex(0));
  TEST_ASSERT_EQUAL_UINT8(0, odometerSlotIndex(ODO_PERSIST_INTERVAL_REVS - 1));
  TEST_ASSERT_EQUAL_UINT8(1, odometerSlotIndex(ODO_PERSIST_INTERVAL_REVS));
  TEST_ASSERT_EQUAL_UINT8(15, odometerSlotIndex(15UL * ODO_PERSIST_INTERVAL_REVS));
}

static void test_slot_index_wraps_after_full_ring() {
  TEST_ASSERT_EQUAL_UINT8(
      0, odometerSlotIndex((uint32_t)ODO_RING_SLOTS * ODO_PERSIST_INTERVAL_REVS));
  TEST_ASSERT_EQUAL_UINT8(
      1, odometerSlotIndex((uint32_t)(ODO_RING_SLOTS + 1) * ODO_PERSIST_INTERVAL_REVS));
}

// --- odometerBootValue: boot-time recovery --------------------------------

static void test_boot_value_is_max_of_ring() {
  uint32_t slots[ODO_RING_SLOTS] = {0};
  slots[3] = 512;
  slots[4] = 640;   // most recent write
  slots[5] = 128;   // older lap of the ring
  TEST_ASSERT_EQUAL_UINT32(640, odometerBootValue(slots));
}

static void test_boot_value_ignores_erased_ff_slots() {
  // A slot that reads 0xFFFFFFFF is erased/corrupt EEPROM, not a count —
  // trusting it would pin the odometer at 4 billion and trip the wear
  // alert forever (the #139 fresh-EEPROM lesson).
  uint32_t slots[ODO_RING_SLOTS] = {0};
  slots[0] = 0xFFFFFFFF;
  slots[1] = 700;
  TEST_ASSERT_EQUAL_UINT32(700, odometerBootValue(slots));
}

static void test_boot_value_all_erased_reads_zero() {
  uint32_t slots[ODO_RING_SLOTS];
  for (int i = 0; i < ODO_RING_SLOTS; i++) slots[i] = 0xFFFFFFFF;
  TEST_ASSERT_EQUAL_UINT32(0, odometerBootValue(slots));
}

// --- odometerSlotChecksum / odometerBootValueChecked (#354) -----------------
// A power-loss-torn slot write used to yield a large garbage value the boot
// ring-max silently adopted; each slot now carries a masked XOR checksum
// byte and only checksum-valid slots count.

static void test_slot_checksum_masked_xor() {
  TEST_ASSERT_EQUAL_UINT8((0x01 ^ 0x02 ^ 0x03 ^ 0x04) ^ ODO_SLOT_CHECKSUM_MASK,
                          odometerSlotChecksum(0x04030201UL));
  // Zero count: checksum is the bare mask — never 0x00 or 0xFF.
  TEST_ASSERT_EQUAL_UINT8(ODO_SLOT_CHECKSUM_MASK, odometerSlotChecksum(0));
}

static void test_boot_value_checked_takes_max_of_valid_slots() {
  uint32_t slots[ODO_RING_SLOTS] = {0};
  uint8_t sums[ODO_RING_SLOTS];
  slots[3] = 512;
  slots[4] = 640;
  for (int i = 0; i < ODO_RING_SLOTS; i++) sums[i] = odometerSlotChecksum(slots[i]);
  TEST_ASSERT_EQUAL_UINT32(640, odometerBootValueChecked(slots, sums));
}

static void test_boot_value_checked_ignores_torn_garbage() {
  // The #354 failure: power loss mid-write leaves a huge garbage value whose
  // checksum byte no longer matches — it must not become the wear count.
  uint32_t slots[ODO_RING_SLOTS] = {0};
  uint8_t sums[ODO_RING_SLOTS];
  for (int i = 0; i < ODO_RING_SLOTS; i++) sums[i] = odometerSlotChecksum(slots[i]);
  slots[7] = 900;
  sums[7] = odometerSlotChecksum(900);
  slots[8] = 0x7F00FFFFUL;                  // torn write...
  sums[8] = odometerSlotChecksum(1024);     // ...checksum from the old value
  TEST_ASSERT_EQUAL_UINT32(900, odometerBootValueChecked(slots, sums));
}

static void test_boot_value_checked_still_rejects_erased_ff() {
  uint32_t slots[ODO_RING_SLOTS];
  uint8_t sums[ODO_RING_SLOTS];
  for (int i = 0; i < ODO_RING_SLOTS; i++) {
    slots[i] = 0xFFFFFFFFUL;
    sums[i] = 0xFF;  // erased EEPROM reads 0xFF everywhere
  }
  TEST_ASSERT_EQUAL_UINT32(0, odometerBootValueChecked(slots, sums));
  // Even a checksum that happens to match must not validate 0xFFFFFFFF.
  sums[2] = odometerSlotChecksum(0xFFFFFFFFUL);
  TEST_ASSERT_EQUAL_UINT32(0, odometerBootValueChecked(slots, sums));
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

// --- odometerShouldPersist: EEPROM write cadence ---------------------------

static void test_should_persist_only_after_interval() {
  TEST_ASSERT_FALSE(odometerShouldPersist(127, 0));
  TEST_ASSERT_TRUE(odometerShouldPersist(128, 0));
  TEST_ASSERT_FALSE(odometerShouldPersist(255, 128));
  TEST_ASSERT_TRUE(odometerShouldPersist(256, 128));
}

static void test_should_persist_false_when_nothing_new() {
  TEST_ASSERT_FALSE(odometerShouldPersist(128, 128));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_add_steps_below_one_rev_accumulates_only);
  RUN_TEST(test_add_steps_rolls_over_at_steps_per_rev);
  RUN_TEST(test_add_steps_negative_counts_magnitude);
  RUN_TEST(test_add_steps_large_move_spans_multiple_revs);
  RUN_TEST(test_slot_index_rotates_every_persist_interval);
  RUN_TEST(test_slot_index_wraps_after_full_ring);
  RUN_TEST(test_boot_value_is_max_of_ring);
  RUN_TEST(test_boot_value_ignores_erased_ff_slots);
  RUN_TEST(test_boot_value_all_erased_reads_zero);
  RUN_TEST(test_slot_checksum_masked_xor);
  RUN_TEST(test_boot_value_checked_takes_max_of_valid_slots);
  RUN_TEST(test_boot_value_checked_ignores_torn_garbage);
  RUN_TEST(test_boot_value_checked_still_rejects_erased_ff);
  RUN_TEST(test_encode_reply_little_endian_with_checksum);
  RUN_TEST(test_encode_reply_zero_count_checksum_is_mask);
  RUN_TEST(test_should_persist_only_after_interval);
  RUN_TEST(test_should_persist_false_when_nothing_new);
  UNITY_END();
  return 0;
}
