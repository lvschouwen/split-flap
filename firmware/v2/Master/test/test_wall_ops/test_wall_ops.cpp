// Native tests for WallOps.h: the jobs an operator started and can ask about.
#include <unity.h>

#include "../../WallOps.h"

void setUp() {}
void tearDown() {}

static void test_a_job_is_running_until_it_is_finished() {
  WallOps ops;
  const uint32_t id = ops.begin("pair", -1);
  TEST_ASSERT_EQUAL_UINT32(1, id);
  const WallOp* op = ops.find(id);
  TEST_ASSERT_NOT_NULL(op);
  TEST_ASSERT_TRUE(WallOpPhase::Running == op->phase);
  TEST_ASSERT_EQUAL_STRING("pair", op->name);
  TEST_ASSERT_TRUE(ops.finish(id, true, "row-a"));
  TEST_ASSERT_TRUE(WallOpPhase::Done == ops.find(id)->phase);
  TEST_ASSERT_EQUAL_STRING("row-a", ops.find(id)->detail);
}

static void test_a_failed_job_keeps_its_reason() {
  WallOps ops;
  const uint32_t id = ops.begin("pair", -1);
  ops.finish(id, false, "the row obeys another master");
  TEST_ASSERT_TRUE(WallOpPhase::Failed == ops.find(id)->phase);
  TEST_ASSERT_EQUAL_STRING("the row obeys another master", ops.find(id)->detail);
}

static void test_a_job_is_finished_once() {
  WallOps ops;
  const uint32_t id = ops.begin("release", 2);
  TEST_ASSERT_TRUE(ops.finish(id, true, ""));
  TEST_ASSERT_FALSE(ops.finish(id, false, "late"));
  TEST_ASSERT_TRUE(WallOpPhase::Done == ops.find(id)->phase);
  TEST_ASSERT_FALSE(ops.finish(999, true, ""));
}

static void test_ids_count_up_and_zero_is_never_one() {
  WallOps ops;
  TEST_ASSERT_NULL(ops.find(0));
  TEST_ASSERT_EQUAL_UINT32(1, ops.begin("a", -1));
  TEST_ASSERT_EQUAL_UINT32(2, ops.begin("b", -1));
  TEST_ASSERT_NULL(ops.find(3));
}

static void test_the_oldest_finished_job_makes_room_and_is_then_unknown() {
  WallOps ops;
  uint32_t ids[WALL_OPS_KEPT];
  for (int i = 0; i < WALL_OPS_KEPT; i++) {
    ids[i] = ops.begin("job", -1);
    ops.finish(ids[i], true, "");
  }
  const uint32_t next = ops.begin("job", -1);
  TEST_ASSERT_TRUE(next != 0);
  TEST_ASSERT_NULL(ops.find(ids[0]));
  TEST_ASSERT_NOT_NULL(ops.find(ids[1]));
  TEST_ASSERT_NOT_NULL(ops.find(next));
}

static void test_running_jobs_are_never_pushed_out() {
  WallOps ops;
  uint32_t first = 0;
  for (int i = 0; i < WALL_OPS_KEPT; i++) {
    const uint32_t id = ops.begin("job", -1);
    if (i == 0) first = id;
  }
  TEST_ASSERT_EQUAL_UINT32(0, ops.begin("one too many", -1));
  TEST_ASSERT_TRUE(WallOpPhase::Running == ops.find(first)->phase);
  ops.finish(first, true, "");
  TEST_ASSERT_TRUE(ops.begin("fits now", -1) != 0);
}

static void test_the_jobs_of_a_row_that_restarted_fail_together() {
  WallOps ops;
  const uint32_t a = ops.begin("self_test", 1);
  const uint32_t b = ops.begin("home", 2);
  const uint32_t done = ops.begin("home", 1);
  ops.finish(done, true, "");
  TEST_ASSERT_EQUAL(1, ops.failRow(1, "the row restarted"));
  TEST_ASSERT_TRUE(WallOpPhase::Failed == ops.find(a)->phase);
  TEST_ASSERT_EQUAL_STRING("the row restarted", ops.find(a)->detail);
  TEST_ASSERT_TRUE(WallOpPhase::Running == ops.find(b)->phase);
  TEST_ASSERT_TRUE(WallOpPhase::Done == ops.find(done)->phase);
}

static void test_long_words_are_cut_not_overrun() {
  WallOps ops;
  const uint32_t id = ops.begin("a-name-much-longer-than-the-field-holds", -1);
  char reason[200];
  memset(reason, 'x', sizeof reason - 1);
  reason[sizeof reason - 1] = 0;
  ops.finish(id, false, reason);
  TEST_ASSERT_EQUAL(sizeof(ops.find(id)->name) - 1, strlen(ops.find(id)->name));
  TEST_ASSERT_EQUAL(sizeof(ops.find(id)->detail) - 1, strlen(ops.find(id)->detail));
}

static void test_one_row_runs_one_job_and_the_own_row_is_a_row_too() {
  WallOps ops;
  TEST_ASSERT_FALSE(ops.runningOn(1));
  const uint32_t a = ops.begin("home", 1);
  const uint32_t own = ops.begin("home", WALL_OP_OWN_ROW);
  TEST_ASSERT_TRUE(ops.runningOn(1));
  TEST_ASSERT_TRUE(ops.runningOn(WALL_OP_OWN_ROW));
  TEST_ASSERT_FALSE(ops.runningOn(2));
  ops.finish(a, true, "ok");
  TEST_ASSERT_FALSE(ops.runningOn(1));
  ops.finish(own, true, "ok");
  TEST_ASSERT_FALSE(ops.runningOn(WALL_OP_OWN_ROW));
}

static void test_a_changed_table_fails_the_jobs_on_row_boards_only() {
  WallOps ops;
  const uint32_t remote = ops.begin("home", 0);
  const uint32_t own = ops.begin("self-test", WALL_OP_OWN_ROW);
  const uint32_t pair = ops.begin("pair", -1);
  TEST_ASSERT_EQUAL(1, ops.failRowBoards("the wall's boards changed"));
  TEST_ASSERT_TRUE(WallOpPhase::Failed == ops.find(remote)->phase);
  TEST_ASSERT_TRUE(WallOpPhase::Running == ops.find(own)->phase);
  TEST_ASSERT_TRUE(WallOpPhase::Running == ops.find(pair)->phase);
}

static void test_a_unit_update_on_a_row_board_is_seen_while_it_runs() {
  WallOps ops;
  ops.begin("update-units", WALL_OP_OWN_ROW);  // the own row's is the display's to report
  ops.begin("home", 1);
  TEST_ASSERT_FALSE(ops.runningOnARowBoard("update-units"));
  const uint32_t id = ops.begin("update-units", 1);
  TEST_ASSERT_TRUE(ops.runningOnARowBoard("update-units"));
  ops.finish(id, false, "wire-fail");
  TEST_ASSERT_FALSE(ops.runningOnARowBoard("update-units"));
}

static void test_a_job_has_a_place_while_it_is_kept() {
  WallOps ops;
  const uint32_t a = ops.begin("a", -1);
  const uint32_t b = ops.begin("b", -1);
  TEST_ASSERT_EQUAL(0, ops.placeOf(a));
  TEST_ASSERT_EQUAL(1, ops.placeOf(b));
  TEST_ASSERT_EQUAL(-1, ops.placeOf(0));
  TEST_ASSERT_EQUAL(-1, ops.placeOf(99));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_one_row_runs_one_job_and_the_own_row_is_a_row_too);
  RUN_TEST(test_a_changed_table_fails_the_jobs_on_row_boards_only);
  RUN_TEST(test_a_unit_update_on_a_row_board_is_seen_while_it_runs);
  RUN_TEST(test_a_job_has_a_place_while_it_is_kept);
  RUN_TEST(test_a_job_is_running_until_it_is_finished);
  RUN_TEST(test_a_failed_job_keeps_its_reason);
  RUN_TEST(test_a_job_is_finished_once);
  RUN_TEST(test_ids_count_up_and_zero_is_never_one);
  RUN_TEST(test_the_oldest_finished_job_makes_room_and_is_then_unknown);
  RUN_TEST(test_running_jobs_are_never_pushed_out);
  RUN_TEST(test_the_jobs_of_a_row_that_restarted_fail_together);
  RUN_TEST(test_long_words_are_cut_not_overrun);
  return UNITY_END();
}
