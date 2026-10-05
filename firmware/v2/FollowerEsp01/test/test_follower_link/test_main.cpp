// Native tests for FollowerLinkPolicy.h: the timing and pairing rules of the
// row board's wall link.
#include <unity.h>

#include "FollowerLinkPolicy.h"

void setUp() {}
void tearDown() {}

static void test_backoff_starts_at_one_second_and_stops_at_eight() {
  uint32_t b = followerLinkNextBackoffMs(0);
  TEST_ASSERT_EQUAL_UINT32(1000, b);
  b = followerLinkNextBackoffMs(b);
  TEST_ASSERT_EQUAL_UINT32(2000, b);
  b = followerLinkNextBackoffMs(b);
  TEST_ASSERT_EQUAL_UINT32(4000, b);
  b = followerLinkNextBackoffMs(b);
  TEST_ASSERT_EQUAL_UINT32(8000, b);
  TEST_ASSERT_EQUAL_UINT32(8000, followerLinkNextBackoffMs(b));
}

static void test_elapsed_survives_the_millisecond_counter_wrapping() {
  TEST_ASSERT_FALSE(followerLinkElapsed(5000, 1000, 10000));
  TEST_ASSERT_TRUE(followerLinkElapsed(11000, 1000, 10000));
  TEST_ASSERT_FALSE(followerLinkElapsed(100, 0xFFFFFF00u, 10000));   // 356 ms later
  TEST_ASSERT_TRUE(followerLinkElapsed(10000, 0xFFFFFF00u, 10000));  // 10256 ms later
}

static void test_host_part_drops_a_port_from_the_stored_address() {
  char host[48];
  TEST_ASSERT_TRUE(followerLinkHostPart("192.168.1.42", host, sizeof host));
  TEST_ASSERT_EQUAL_STRING("192.168.1.42", host);
  TEST_ASSERT_TRUE(followerLinkHostPart("192.168.1.42:8801", host, sizeof host));
  TEST_ASSERT_EQUAL_STRING("192.168.1.42", host);
  TEST_ASSERT_FALSE(followerLinkHostPart("", host, sizeof host));
  TEST_ASSERT_FALSE(followerLinkHostPart(":8801", host, sizeof host));
  char tiny[8];
  TEST_ASSERT_FALSE(followerLinkHostPart("192.168.1.42", tiny, sizeof tiny));
}

static void test_only_the_paired_master_is_accepted() {
  TEST_ASSERT_TRUE(followerLinkMasterAccepted("split-flap-c8a746", "split-flap-c8a746"));
  TEST_ASSERT_FALSE(followerLinkMasterAccepted("split-flap-c8a746", "split-flap-a47dee"));
  TEST_ASSERT_FALSE(followerLinkMasterAccepted("", ""));  // not paired: nobody is the master
  TEST_ASSERT_FALSE(followerLinkMasterAccepted("split-flap-c8a746", ""));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_backoff_starts_at_one_second_and_stops_at_eight);
  RUN_TEST(test_elapsed_survives_the_millisecond_counter_wrapping);
  RUN_TEST(test_host_part_drops_a_port_from_the_stored_address);
  RUN_TEST(test_only_the_paired_master_is_accepted);
  return UNITY_END();
}
