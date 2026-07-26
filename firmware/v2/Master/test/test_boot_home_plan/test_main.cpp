// Host-side unit tests for the pure batched boot-home target selection in
// BootHomePlan.h (#309 master side): which units get homed and the batch math.
// The Wire glue (send HOME, wait for idle, rail-settle) is bench tier.

#include <unity.h>
#include <stdint.h>
#include "BootHomePlan.h"

void setUp() {}
void tearDown() {}

static void mkSketch(UnitFacts& u, bool homed) {
  u.state = 1;
  u.statusValid = true;
  u.status.flags = homed ? UNIT_FLAG_HOMED : 0;
}

// --- target selection ------------------------------------------------------

static void test_collects_only_unhomed_sketch_units() {
  UnitFacts units[4];
  mkSketch(units[0], false);  // unhomed sketch -> target
  mkSketch(units[1], true);   // already homed  -> skip
  units[2].state = 2;         // bootloader     -> skip
  mkSketch(units[3], false);  // unhomed sketch -> target
  uint8_t out[4];
  int n = bootHomeCollectTargets(units, 4, /*base=*/1, out);
  TEST_ASSERT_EQUAL_INT(2, n);
  TEST_ASSERT_EQUAL_UINT8(1, out[0]);  // base + col 0
  TEST_ASSERT_EQUAL_UINT8(4, out[1]);  // base + col 3
}

static void test_invalid_status_is_not_a_target() {
  // A sketch slot we never read (statusValid false) is unknown, not unhomed —
  // don't blind-home it; the poll will establish its state first.
  UnitFacts units[1];
  units[0].state = 1;
  units[0].statusValid = false;
  uint8_t out[1];
  TEST_ASSERT_EQUAL_INT(0, bootHomeCollectTargets(units, 1, 1, out));
}

static void test_all_homed_yields_no_targets() {
  UnitFacts units[3];
  for (int i = 0; i < 3; i++) mkSketch(units[i], true);
  uint8_t out[3];
  TEST_ASSERT_EQUAL_INT(0, bootHomeCollectTargets(units, 3, 1, out));
}

static void test_base_offsets_addresses() {
  UnitFacts units[2];
  mkSketch(units[0], false);
  mkSketch(units[1], false);
  uint8_t out[2];
  bootHomeCollectTargets(units, 2, /*base=*/1, out);
  TEST_ASSERT_EQUAL_UINT8(1, out[0]);
  TEST_ASSERT_EQUAL_UINT8(2, out[1]);
}

// --- batch math ------------------------------------------------------------

static void test_batch_count() {
  TEST_ASSERT_EQUAL_INT(0, bootHomeBatchCount(0, 1));
  TEST_ASSERT_EQUAL_INT(5, bootHomeBatchCount(5, 1));
  TEST_ASSERT_EQUAL_INT(3, bootHomeBatchCount(5, 2));  // ceil(5/2)
  TEST_ASSERT_EQUAL_INT(2, bootHomeBatchCount(5, 4));  // ceil(5/4)
  TEST_ASSERT_EQUAL_INT(1, bootHomeBatchCount(4, 4));
}

static void test_batch_count_clamps_bad_size() {
  // A zero/negative batch size falls back to 1 rather than dividing by zero.
  TEST_ASSERT_EQUAL_INT(3, bootHomeBatchCount(3, 0));
  TEST_ASSERT_EQUAL_INT(3, bootHomeBatchCount(3, -2));
}

static void test_default_batch_size_is_one() {
  // Spec: start at N=1, grow only once bench vmin proves the rail holds.
  TEST_ASSERT_EQUAL_INT(1, BOOT_HOME_BATCH_SIZE);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_collects_only_unhomed_sketch_units);
  RUN_TEST(test_invalid_status_is_not_a_target);
  RUN_TEST(test_all_homed_yields_no_targets);
  RUN_TEST(test_base_offsets_addresses);
  RUN_TEST(test_batch_count);
  RUN_TEST(test_batch_count_clamps_bad_size);
  RUN_TEST(test_default_batch_size_is_one);
  return UNITY_END();
}
