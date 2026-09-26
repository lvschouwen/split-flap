// Host-side tests for the follower's row-wide I2C bus-death detector and
// recovery backoff (#488): one dead unit must never trigger a bus recovery,
// a row where every drivable unit stops answering must, attempts back off,
// and the first good read closes the episode.

#include <unity.h>

#include "../../FollowerBusRecovery.h"

void setUp() {}
void tearDown() {}

static BusRecoveryEvent failRound(BusRecoveryState& s, int width,
                                  uint32_t now) {
  BusRecoveryEvent last = BusRecoveryEvent::None;
  for (int i = 0; i < width; i++) {
    BusRecoveryEvent e = busRecoveryObserve(s, i, false, now, width);
    if (e != BusRecoveryEvent::None) last = e;
  }
  return last;
}

static void test_fresh_state_is_alive_and_not_due() {
  BusRecoveryState s;
  TEST_ASSERT_FALSE(s.dead);
  TEST_ASSERT_FALSE(busRecoveryDue(s, 0));
  TEST_ASSERT_FALSE(busRecoveryDue(s, 1000000));
  TEST_ASSERT_EQUAL_INT(-1, s.lastStatus);
}

static void test_single_dead_unit_among_healthy_never_trips() {
  BusRecoveryState s;
  for (int round = 0; round < 50; round++) {
    for (int i = 0; i < 5; i++) {
      busRecoveryObserve(s, i, i != 2, round * 1000u, 5);
    }
  }
  TEST_ASSERT_FALSE(s.dead);
  TEST_ASSERT_EQUAL_UINT32(0, s.episodes);
}

static void test_single_dead_unit_alone_in_mask_never_trips() {
  // Only one unit ever polled and failing (others not polled yet): the
  // mask has one bit, so this is a unit fault, not a bus fault.
  BusRecoveryState s;
  for (int n = 0; n < 40; n++) busRecoveryObserve(s, 3, false, n, 5);
  TEST_ASSERT_FALSE(s.dead);
}

static void test_whole_row_failing_trips_once() {
  BusRecoveryState s;
  BusRecoveryEvent e = failRound(s, 5, 100);
  e = failRound(s, 5, 200) == BusRecoveryEvent::WentDead
          ? BusRecoveryEvent::WentDead
          : e;
  TEST_ASSERT_TRUE(s.dead);
  TEST_ASSERT_EQUAL_UINT32(1, s.episodes);
  TEST_ASSERT_TRUE(e == BusRecoveryEvent::WentDead);
  // Further failures stay in the same episode.
  TEST_ASSERT_TRUE(failRound(s, 5, 300) == BusRecoveryEvent::None);
  TEST_ASSERT_EQUAL_UINT32(1, s.episodes);
}

static void test_trips_after_threshold_not_before() {
  BusRecoveryState s;
  for (int n = 0; n < BUS_DEAD_MIN_FAILS - 1; n++) {
    busRecoveryObserve(s, n % 5, false, n, 5);
  }
  TEST_ASSERT_FALSE(s.dead);
  TEST_ASSERT_TRUE(busRecoveryObserve(s, 0, false, 99, 5) ==
                   BusRecoveryEvent::WentDead);
  TEST_ASSERT_TRUE(s.dead);
  TEST_ASSERT_EQUAL_UINT32(99, s.deadSinceMs);
}

static void test_one_unit_row_trips_on_its_own_failures() {
  // A 1-wide row can't tell a dead unit from a dead bus; a rate-limited
  // recovery is harmless there, so it must be allowed.
  BusRecoveryState s;
  for (int n = 0; n < BUS_DEAD_MIN_FAILS; n++) {
    busRecoveryObserve(s, 0, false, n, 1);
  }
  TEST_ASSERT_TRUE(s.dead);
}

static void test_wide_row_with_one_polled_unit_never_trips() {
  // Row width 5 but only unit 0 still drivable: its own failures are a unit
  // fault, not bus death.
  BusRecoveryState s;
  for (int n = 0; n < 40; n++) busRecoveryObserve(s, 0, false, n, 5);
  TEST_ASSERT_FALSE(s.dead);
}

static void test_empty_row_opens_one_episode_due_now() {
  BusRecoveryState s;
  busRecoveryNoteEmptyRow(s, 500);
  busRecoveryNoteEmptyRow(s, 900);
  TEST_ASSERT_TRUE(s.dead);
  TEST_ASSERT_EQUAL_UINT32(1, s.episodes);
  TEST_ASSERT_EQUAL_UINT32(500, s.deadSinceMs);
  TEST_ASSERT_TRUE(busRecoveryDue(s, 500));
  busRecoveryNoteAttempt(s, 500, 3);
  busRecoveryNoteEmptyRow(s, 600);  // still empty: must not reset the backoff
  TEST_ASSERT_FALSE(busRecoveryDue(s, 600));
  // A re-probe that finds units closes it.
  TEST_ASSERT_TRUE(busRecoveryObserve(s, 0, true, 2500, 5) ==
                   BusRecoveryEvent::Recovered);
  TEST_ASSERT_EQUAL_UINT32(2000, s.lastDeadMs);
}

static void test_success_resets_the_run() {
  BusRecoveryState s;
  for (int n = 0; n < BUS_DEAD_MIN_FAILS - 1; n++) {
    busRecoveryObserve(s, n % 5, false, n, 5);
  }
  busRecoveryObserve(s, 4, true, 50, 5);
  for (int n = 0; n < BUS_DEAD_MIN_FAILS - 1; n++) {
    busRecoveryObserve(s, n % 5, false, 60 + n, 5);
  }
  TEST_ASSERT_FALSE(s.dead);
}

static void test_due_immediately_then_backs_off() {
  BusRecoveryState s;
  failRound(s, 5, 1000);
  failRound(s, 5, 1000);
  TEST_ASSERT_TRUE(s.dead);
  TEST_ASSERT_TRUE(busRecoveryDue(s, 1000));

  busRecoveryNoteAttempt(s, 1000, 3);
  TEST_ASSERT_EQUAL_INT(3, s.lastStatus);
  TEST_ASSERT_EQUAL_UINT32(1, s.attempts);
  TEST_ASSERT_FALSE(busRecoveryDue(s, 1000 + BUS_RECOVERY_BACKOFF_BASE_MS - 1));
  TEST_ASSERT_TRUE(busRecoveryDue(s, 1000 + BUS_RECOVERY_BACKOFF_BASE_MS));

  uint32_t t = 1000 + BUS_RECOVERY_BACKOFF_BASE_MS;
  busRecoveryNoteAttempt(s, t, 3);
  TEST_ASSERT_FALSE(busRecoveryDue(s, t + 2 * BUS_RECOVERY_BACKOFF_BASE_MS - 1));
  TEST_ASSERT_TRUE(busRecoveryDue(s, t + 2 * BUS_RECOVERY_BACKOFF_BASE_MS));
}

static void test_backoff_is_capped() {
  for (uint8_t n = 0; n < 40; n++) {
    TEST_ASSERT_TRUE(busRecoveryBackoffMs(n) <= BUS_RECOVERY_BACKOFF_MAX_MS);
  }
  TEST_ASSERT_EQUAL_UINT32(BUS_RECOVERY_BACKOFF_MAX_MS,
                           busRecoveryBackoffMs(255));
}

static void test_due_survives_millis_wrap() {
  BusRecoveryState s;
  uint32_t base = 0xFFFFFF00UL;
  failRound(s, 5, base);
  failRound(s, 5, base);
  busRecoveryNoteAttempt(s, base, 3);
  TEST_ASSERT_FALSE(busRecoveryDue(s, base + 10));
  TEST_ASSERT_TRUE(busRecoveryDue(s, base + BUS_RECOVERY_BACKOFF_BASE_MS));
}

static void test_first_success_closes_episode() {
  BusRecoveryState s;
  failRound(s, 5, 1000);
  failRound(s, 5, 1000);
  busRecoveryNoteAttempt(s, 1000, 0);
  TEST_ASSERT_TRUE(busRecoveryObserve(s, 2, true, 7000, 5) ==
                   BusRecoveryEvent::Recovered);
  TEST_ASSERT_FALSE(s.dead);
  TEST_ASSERT_FALSE(busRecoveryDue(s, 7000));
  TEST_ASSERT_EQUAL_UINT32(1, s.recovered);
  TEST_ASSERT_EQUAL_UINT32(6000, s.lastDeadMs);
  // A later episode starts its backoff from the base again.
  failRound(s, 5, 9000);
  failRound(s, 5, 9000);
  TEST_ASSERT_TRUE(busRecoveryDue(s, 9000));
  busRecoveryNoteAttempt(s, 9000, 3);
  TEST_ASSERT_TRUE(busRecoveryDue(s, 9000 + BUS_RECOVERY_BACKOFF_BASE_MS));
  TEST_ASSERT_EQUAL_UINT32(2, s.episodes);
}

static void test_out_of_range_index_is_ignored_safely() {
  BusRecoveryState s;
  for (int n = 0; n < 50; n++) busRecoveryObserve(s, 99, false, n, 5);
  for (int n = 0; n < 50; n++) busRecoveryObserve(s, -1, false, n, 5);
  TEST_ASSERT_FALSE(s.dead);
}

static void test_zero_width_never_trips() {
  BusRecoveryState s;
  for (int n = 0; n < 50; n++) busRecoveryObserve(s, n % 5, false, n, 0);
  TEST_ASSERT_FALSE(s.dead);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fresh_state_is_alive_and_not_due);
  RUN_TEST(test_single_dead_unit_among_healthy_never_trips);
  RUN_TEST(test_single_dead_unit_alone_in_mask_never_trips);
  RUN_TEST(test_whole_row_failing_trips_once);
  RUN_TEST(test_trips_after_threshold_not_before);
  RUN_TEST(test_one_unit_row_trips_on_its_own_failures);
  RUN_TEST(test_wide_row_with_one_polled_unit_never_trips);
  RUN_TEST(test_empty_row_opens_one_episode_due_now);
  RUN_TEST(test_success_resets_the_run);
  RUN_TEST(test_due_immediately_then_backs_off);
  RUN_TEST(test_backoff_is_capped);
  RUN_TEST(test_due_survives_millis_wrap);
  RUN_TEST(test_first_success_closes_episode);
  RUN_TEST(test_out_of_range_index_is_ignored_safely);
  RUN_TEST(test_zero_width_never_trips);
  return UNITY_END();
}
