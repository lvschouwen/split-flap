// Host-side test for the unit facts buffer (FollowerOps.h, #519): the widest
// row the serializer can write, with the wear and unit-update splices, must
// fit the buffer this board allocates for that width.

#include <unity.h>

#include <cstring>

#include "../../FollowerOps.h"
#include "UnitFactsWidest.h"

void setUp() {}
void tearDown() {}

static void test_health_json_follower_worst_case_fits_local_buf() {
  static char buf[16384];
  for (int width = 1; width <= 16; width++) {
    const size_t cap = followerHealthBufCap(width, 16);
    TEST_ASSERT_TRUE(cap <= sizeof(buf));
    TEST_ASSERT_TRUE(unitFactsWidestDoc(buf, cap, width, false) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"wear\":{"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"reflash\":{"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"errAge\":4294967295"));
  }
  // A 5-unit row gets well under half of what a 16-unit worst case would take.
  TEST_ASSERT_TRUE(followerHealthBufCap(5, 16) < 4096);
  // Out-of-range widths clamp instead of under- or over-allocating.
  TEST_ASSERT_EQUAL_size_t(followerHealthBufCap(0, 16), followerHealthBufCap(-3, 16));
  TEST_ASSERT_EQUAL_size_t(followerHealthBufCap(16, 16), followerHealthBufCap(99, 16));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_health_json_follower_worst_case_fits_local_buf);
  return UNITY_END();
}
