// Host-side unit tests for the pure sub-frame render stagger in
// RenderStagger.h (#324): the batch decision that spreads flap inrush across a
// bulk render. The Wire glue + the actual settle delay are bench tier.

#include <unity.h>
#include <stdint.h>
#include "RenderStagger.h"

void setUp() {}
void tearDown() {}

// --- batch decision --------------------------------------------------------

static void test_no_settle_before_the_first_unit() {
  // commandedSoFar == 0: the first unit opens group 1, never preceded by a
  // settle (nothing has drawn inrush yet).
  TEST_ASSERT_FALSE(renderStaggerShouldSettle(0, 4));
}

static void test_settles_when_a_full_group_precedes_the_next_unit() {
  // Placed BEFORE the write: true means "pause, the current group is full".
  TEST_ASSERT_TRUE(renderStaggerShouldSettle(4, 4));   // before the 5th unit
  TEST_ASSERT_TRUE(renderStaggerShouldSettle(8, 4));
  TEST_ASSERT_TRUE(renderStaggerShouldSettle(12, 4));
}

static void test_no_settle_mid_group() {
  for (int c = 1; c <= 3; c++) TEST_ASSERT_FALSE(renderStaggerShouldSettle(c, 4));
  TEST_ASSERT_FALSE(renderStaggerShouldSettle(5, 4));
  TEST_ASSERT_FALSE(renderStaggerShouldSettle(7, 4));
}

static void test_full_row_yields_batch_minus_one_pauses() {
  // 16 units, batch 4 -> pauses before units 5/9/13 == 3 gaps between 4 groups,
  // NO trailing settle after the final unit (the loop simply ends).
  int pauses = 0;
  int commanded = 0;
  for (int slot = 0; slot < 16; slot++) {
    if (renderStaggerShouldSettle(commanded, 4)) pauses++;
    commanded++;  // every slot is a present, commanded unit here
  }
  TEST_ASSERT_EQUAL_INT(3, pauses);
}

static void test_odd_tail_group_still_separated() {
  // 5 commanded units, batch 4 -> one pause splitting [4][1]: the lone tail
  // unit's inrush is kept off the first group's.
  int pauses = 0;
  for (int c = 0; c < 5; c++) if (renderStaggerShouldSettle(c, 4)) pauses++;
  TEST_ASSERT_EQUAL_INT(1, pauses);
}

static void test_bad_batch_size_falls_back_to_one() {
  // A zero/negative batch settles between every unit rather than dividing by
  // zero — degrades to maximum spread, never crashes.
  TEST_ASSERT_TRUE(renderStaggerShouldSettle(1, 0));
  TEST_ASSERT_TRUE(renderStaggerShouldSettle(3, -2));
  TEST_ASSERT_FALSE(renderStaggerShouldSettle(0, 0));
}

// --- constants -------------------------------------------------------------

static void test_starting_constants() {
  // Bench-tuned starting points (#324): 4 units per group, 100 ms rail-settle.
  TEST_ASSERT_EQUAL_INT(4, RENDER_STAGGER_BATCH);
  TEST_ASSERT_EQUAL_UINT32(100UL, RENDER_STAGGER_SETTLE_MS);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_no_settle_before_the_first_unit);
  RUN_TEST(test_settles_when_a_full_group_precedes_the_next_unit);
  RUN_TEST(test_no_settle_mid_group);
  RUN_TEST(test_full_row_yields_batch_minus_one_pauses);
  RUN_TEST(test_odd_tail_group_still_separated);
  RUN_TEST(test_bad_batch_size_falls_back_to_one);
  RUN_TEST(test_starting_constants);
  return UNITY_END();
}
