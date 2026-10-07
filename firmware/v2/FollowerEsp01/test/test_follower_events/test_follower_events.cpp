// Native tests for FollowerEvents.h: what waits to be told to the master.
#include <unity.h>

#include "../../FollowerEvents.h"

void setUp() {}
void tearDown() {}

static void test_events_wait_in_order_until_they_are_sent() {
  FollowerEvents events;
  TEST_ASSERT_NULL(events.front());
  TEST_ASSERT_TRUE(events.put(1, 0, 0x0204, 3, 2));
  TEST_ASSERT_TRUE(events.put(3, 0, 3900, 0, 500));
  const FollowerEvent* e = events.front();
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_EQUAL_UINT8(1, e->code);
  TEST_ASSERT_EQUAL_UINT32(0x0204, e->a);
  TEST_ASSERT_EQUAL_UINT32(3, e->b);
  // Not sent yet: it is still the one in front.
  TEST_ASSERT_EQUAL_UINT8(1, events.front()->code);
  events.pop();
  TEST_ASSERT_EQUAL_UINT8(3, events.front()->code);
  TEST_ASSERT_EQUAL_UINT32(3900, events.front()->a);
  events.pop();
  TEST_ASSERT_NULL(events.front());
  events.pop();  // nothing to take: no harm
  TEST_ASSERT_EQUAL_UINT8(0, events.count);
}

static void test_when_every_place_is_taken_the_newer_event_goes() {
  FollowerEvents events;
  for (int i = 0; i < FOLLOWER_EVENTS_CAP; i++) {
    TEST_ASSERT_TRUE(events.put((uint8_t)(i + 1), 0, 0, 0, 10));
  }
  TEST_ASSERT_FALSE(events.put(99, 0, 0, 0, 20));
  TEST_ASSERT_EQUAL_UINT8(FOLLOWER_EVENTS_CAP, events.count);
  for (int i = 0; i < FOLLOWER_EVENTS_CAP; i++) {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(i + 1), events.front()->code);
    events.pop();
  }
  TEST_ASSERT_TRUE(events.put(99, 0, 0, 0, 30));
}

static void test_an_event_says_how_long_ago_it_was() {
  FollowerEvents events;
  events.put(1, 0, 0, 0, 2);
  TEST_ASSERT_EQUAL_UINT32(43, followerEventAgeS(*events.front(), 45));
  TEST_ASSERT_EQUAL_UINT32(0, followerEventAgeS(*events.front(), 2));
  TEST_ASSERT_EQUAL_UINT32(0, followerEventAgeS(*events.front(), 1));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_events_wait_in_order_until_they_are_sent);
  RUN_TEST(test_when_every_place_is_taken_the_newer_event_goes);
  RUN_TEST(test_an_event_says_how_long_ago_it_was);
  return UNITY_END();
}
