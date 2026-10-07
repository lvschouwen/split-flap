// Native tests for WallJobs.h: the unit jobs of POST /api/v2/action.
#include <unity.h>

#include "WallJobs.h"

void setUp() {}
void tearDown() {}

static const char* build(const char* name, bool haveUnit, long unit, bool haveArg, long arg,
                         wl_Op& op) {
  const WallJobKind* kind = wallJobFind(name);
  TEST_ASSERT_NOT_NULL(kind);
  return wallJobBuild(*kind, haveUnit, unit, haveArg, arg, op);
}

static void test_a_job_is_found_by_its_name_and_by_its_code() {
  TEST_ASSERT_NULL(wallJobFind("pair"));
  TEST_ASSERT_NULL(wallJobFind(""));
  TEST_ASSERT_EQUAL(wl_OpCode_OPC_SELF_TEST, wallJobFind("self-test")->opcode);
  TEST_ASSERT_EQUAL_STRING("boot-dump", wallJobFind(wl_OpCode_OPC_BOOT_DUMP)->name);
  TEST_ASSERT_NULL(wallJobFind(wl_OpCode_OPC_NONE));
}

static void test_every_name_and_code_appears_once() {
  for (const WallJobKind& a : WALL_JOB_KINDS) {
    TEST_ASSERT_TRUE(&a == wallJobFind(a.name));
    TEST_ASSERT_TRUE(&a == wallJobFind(a.opcode));
    // A name fits a job's record (WallOp.name).
    TEST_ASSERT_TRUE(strlen(a.name) < 16);
  }
}

static void test_a_job_on_one_unit_carries_its_address() {
  wl_Op op;
  TEST_ASSERT_NULL(build("home", true, 7, false, 0, op));
  TEST_ASSERT_EQUAL(wl_OpCode_OPC_HOME, op.opcode);
  TEST_ASSERT_EQUAL_UINT32(7, op.address);
  TEST_ASSERT_EQUAL_UINT32(0, op.op_id);
  TEST_ASSERT_NOT_NULL(build("home", false, 0, false, 0, op));
  TEST_ASSERT_NOT_NULL(build("home", true, 0, false, 0, op));
  TEST_ASSERT_NOT_NULL(build("home", true, 127, false, 0, op));
  TEST_ASSERT_NOT_NULL(build("home", true, 7, true, 1, op));  // takes no value
}

static void test_values_take_the_shared_checks() {
  wl_Op op;
  TEST_ASSERT_NULL(build("jog", true, 2, true, -127, op));
  TEST_ASSERT_EQUAL_INT32(-127, op.arg);
  TEST_ASSERT_NOT_NULL(build("jog", true, 2, true, 128, op));
  TEST_ASSERT_NOT_NULL(build("jog", true, 2, false, 0, op));  // steps are required
  TEST_ASSERT_NULL(build("set-offset", true, 2, true, 2038, op));
  TEST_ASSERT_NOT_NULL(build("set-offset", true, 2, true, 2039, op));
  TEST_ASSERT_NULL(build("set-gates", true, 2, true, 0, op));
  TEST_ASSERT_NOT_NULL(build("set-gates", true, 2, true, 256, op));
}

static void test_updating_units_is_one_or_all_and_forced_only_one_at_a_time() {
  wl_Op op;
  TEST_ASSERT_NULL(build("update-units", false, 0, false, 0, op));
  TEST_ASSERT_EQUAL_UINT32(0, op.address);
  TEST_ASSERT_NULL(build("update-units", true, 4, true, 1, op));
  TEST_ASSERT_EQUAL_UINT32(4, op.address);
  TEST_ASSERT_EQUAL_INT32(1, op.arg);
  TEST_ASSERT_NOT_NULL(build("update-units", false, 0, true, 1, op));
  TEST_ASSERT_NULL(build("update-units", false, 0, true, 0, op));
  TEST_ASSERT_NOT_NULL(build("update-units", true, 4, true, 2, op));
}

static void test_a_rescan_is_for_the_whole_row() {
  wl_Op op;
  TEST_ASSERT_NULL(build("probe", false, 0, false, 0, op));
  TEST_ASSERT_NOT_NULL(build("probe", true, 3, false, 0, op));
}

static void test_how_a_job_ended_reads_as_the_op_result_words() {
  char text[96];
  wallJobOutcomeText(text, sizeof text, true, 0, 0);
  TEST_ASSERT_EQUAL_STRING("ok", text);
  wallJobOutcomeText(text, sizeof text, true, 0, (uint32_t)MaintReason::BootAlreadyNew);
  TEST_ASSERT_EQUAL_STRING("boot-already-new", text);
  wallJobOutcomeText(text, sizeof text, false, (uint32_t)MaintOutcome::WireFail, 0);
  TEST_ASSERT_EQUAL_STRING("wire-fail", text);
  wallJobOutcomeText(text, sizeof text, false, (uint32_t)MaintOutcome::PostconditionFail,
                     (uint32_t)MaintReason::BootVerifyFailed);
  TEST_ASSERT_EQUAL_STRING("postcondition-fail: boot-verify-failed", text);
  wallJobOutcomeText(text, sizeof text, false, (uint32_t)MaintOutcome::Pending, 0);
  TEST_ASSERT_EQUAL_STRING("the result was lost on the board", text);
}

static void test_every_refusal_has_its_own_words() {
  const char* fallback = wallJobRefusalText(999);
  for (uint32_t r = wl_OpRefusal_REFUSAL_UNKNOWN_OP; r <= wl_OpRefusal_REFUSAL_NO_MEMORY; r++) {
    TEST_ASSERT_TRUE(strcmp(fallback, wallJobRefusalText(r)) != 0);
    TEST_ASSERT_TRUE(strlen(wallJobRefusalText(r)) < 96);  // fits WallOp.detail
  }
}

static void test_every_job_has_a_number_for_the_event_record() {
  TEST_ASSERT_EQUAL_UINT8(1, wallJobNumber("home"));
  TEST_ASSERT_EQUAL_UINT8(12, wallJobNumber("update-units"));
  TEST_ASSERT_EQUAL_UINT8(200, wallJobNumber("pair"));
  TEST_ASSERT_EQUAL_UINT8(201, wallJobNumber("release"));
  TEST_ASSERT_EQUAL_UINT8(202, wallJobNumber("arrange"));
  TEST_ASSERT_EQUAL_UINT8(203, wallJobNumber("update"));
  TEST_ASSERT_EQUAL_UINT8(0, wallJobNumber("no-such-job"));
  for (const WallJobKind& kind : WALL_JOB_KINDS) {
    const uint8_t number = wallJobNumber(kind.name);
    TEST_ASSERT_TRUE(number > 0 && number < 200);
    TEST_ASSERT_EQUAL_STRING(kind.name, wallJobNumberName(number));
  }
  TEST_ASSERT_EQUAL_STRING("pair", wallJobNumberName(200));
  TEST_ASSERT_EQUAL_STRING("?", wallJobNumberName(0));
  TEST_ASSERT_EQUAL_STRING("?", wallJobNumberName(250));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_job_is_found_by_its_name_and_by_its_code);
  RUN_TEST(test_every_name_and_code_appears_once);
  RUN_TEST(test_a_job_on_one_unit_carries_its_address);
  RUN_TEST(test_values_take_the_shared_checks);
  RUN_TEST(test_updating_units_is_one_or_all_and_forced_only_one_at_a_time);
  RUN_TEST(test_a_rescan_is_for_the_whole_row);
  RUN_TEST(test_how_a_job_ended_reads_as_the_op_result_words);
  RUN_TEST(test_every_refusal_has_its_own_words);
  RUN_TEST(test_every_job_has_a_number_for_the_event_record);
  return UNITY_END();
}
