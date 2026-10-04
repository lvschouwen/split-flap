// Host-side tests for shared/BootUpdateOp.h — the in-system twiboot update
// both row masters run. The hooks are a scripted unit, so the order, the
// re-show and the grading are exercised without hardware.

#include <unity.h>

#include <string>
#include <vector>

#include "BootDumpOp.h"
#include "BootUpdateOp.h"

void setUp() {}
void tearDown() {}

namespace {

struct FakeUnit {
  // --- device model ---
  uint8_t state = BOOT_STATE_OLD;
  uint8_t lastResult = BOOT_RESULT_NONE;
  bool infoReadable = true;
  bool idle = true;
  int stage1Ack = 0;
  int stage2Ack = 0;
  bool stage1Refuses = false;       // keeps answering, records a refusal
  uint8_t refusal = BOOT_RESULT_REFUSED_BUSY;
  bool lostAfterStage1 = false;
  uint8_t stateAfterStage1 = BOOT_STATE_PAGE7_INSTALLED;
  bool stage2Lands = true;
  uint8_t stage2Result = BOOT_RESULT_VERIFY_FAILED;
  bool silentAfterStage2 = false;

  // --- runtime ---
  uint32_t now = 0;
  uint32_t offBusUntil = 0;

  // --- observations ---
  std::vector<std::string> calls;
  int reshows = 0;
  int holds = 0;
  int invalidated = 0;
  int homes = 0;
  BootUpdateStep lastStep = BootUpdateStep::ReadInfo;
  MaintReason lastWhy = MaintReason::None;

  bool readBootInfo(uint8_t, BootUpdateReport& out) {
    if (!infoReadable || now < offBusUntil) return false;
    out.state = state;
    out.lastResult = lastResult;
    return true;
  }
  bool waitIdle(uint8_t, uint32_t timeoutMs) {
    if (!idle) {
      now += timeoutMs;
      return false;
    }
    // An idle report needs the unit back in its sketch.
    if (now < offBusUntil) now = offBusUntil;
    now += 100;
    return true;
  }
  int sendStage(uint8_t, uint8_t stage) {
    calls.push_back(stage == 1 ? "stage1" : "stage2");
    if (stage == 1) {
      if (stage1Ack != 0) return stage1Ack;
      if (stage1Refuses) {
        lastResult = refusal;
        return 0;
      }
      offBusUntil = now + 1200;
      state = stateAfterStage1;
      if (lostAfterStage1) infoReadable = false;
      return 0;
    }
    if (stage2Ack != 0) return stage2Ack;
    if (silentAfterStage2) {
      infoReadable = false;
    } else if (stage2Lands) {
      state = BOOT_STATE_NEW;
      lastResult = BOOT_RESULT_STAGE2_OK;
    } else {
      lastResult = stage2Result;
    }
    return 0;
  }
  int home(uint8_t) {
    homes++;
    calls.push_back("home");
    return 0;
  }
  void unitLeftSketch(uint8_t) { invalidated++; }
  void holdProbes() { holds++; }
  void pause(uint32_t ms) { now += ms; }
  uint32_t nowMs() { return now; }
  void reshow() {
    reshows++;
    calls.push_back("reshow");
  }
  void note(BootUpdateStep step, MaintReason why, const BootUpdateReport&) {
    lastStep = step;
    lastWhy = why;
  }
};

void assertGrade(MaintOutcome o, MaintReason r, MaintGrade g) {
  TEST_ASSERT_EQUAL((int)o, (int)g.outcome);
  TEST_ASSERT_EQUAL((int)r, (int)g.reason);
}

}  // namespace

static void test_old_unit_runs_both_stages_and_reshows_once() {
  FakeUnit u;
  MaintGrade g = bootUpdateRun(u, 5);
  assertGrade(MaintOutcome::Ok, MaintReason::None, g);
  std::vector<std::string> want = {"stage1", "home", "stage2", "reshow"};
  TEST_ASSERT_TRUE(want == u.calls);
  TEST_ASSERT_EQUAL(1, u.invalidated);
  TEST_ASSERT_EQUAL(1, u.holds);
  TEST_ASSERT_EQUAL((int)BootUpdateStep::Done, (int)u.lastStep);
}

static void test_already_new_is_ok_with_a_reason_and_touches_nothing() {
  FakeUnit u;
  u.state = BOOT_STATE_NEW;
  assertGrade(MaintOutcome::Ok, MaintReason::BootAlreadyNew,
              bootUpdateRun(u, 5));
  TEST_ASSERT_TRUE(u.calls.empty());
}

static void test_unreadable_report_fails_before_anything_is_sent() {
  FakeUnit u;
  u.infoReadable = false;
  assertGrade(MaintOutcome::PostconditionFail, MaintReason::BootInfoReadFail,
              bootUpdateRun(u, 5));
  TEST_ASSERT_TRUE(u.calls.empty());
}

static void test_moving_drum_ends_the_op_as_busy() {
  FakeUnit u;
  u.idle = false;
  assertGrade(MaintOutcome::PostconditionFail, MaintReason::BootUnitBusy,
              bootUpdateRun(u, 5));
  TEST_ASSERT_TRUE(u.calls.empty());
}

static void test_stage1_nack_is_a_wire_fail_without_reshow() {
  FakeUnit u;
  u.stage1Ack = 2;
  assertGrade(MaintOutcome::WireFail, MaintReason::None, bootUpdateRun(u, 5));
  TEST_ASSERT_EQUAL(0, u.reshows);
  TEST_ASSERT_EQUAL(0, u.invalidated);
}

// A unit that keeps answering never started: report its own refusal, and the
// row was not disturbed, so nothing is re-shown.
static void test_stage1_refusal_reports_what_the_unit_said() {
  FakeUnit u;
  u.stage1Refuses = true;
  assertGrade(MaintOutcome::PostconditionFail, MaintReason::BootUnitBusy,
              bootUpdateRun(u, 5));
  TEST_ASSERT_EQUAL(0, u.reshows);
  TEST_ASSERT_EQUAL(0, u.homes);
}

static void test_stage1_without_a_recorded_refusal_is_not_started() {
  FakeUnit u;
  u.stage1Refuses = true;
  u.refusal = BOOT_RESULT_NONE;
  assertGrade(MaintOutcome::PostconditionFail, MaintReason::BootNotStarted,
              bootUpdateRun(u, 5));
}

static void test_unit_lost_after_stage1_reshows_the_row() {
  FakeUnit u;
  u.lostAfterStage1 = true;
  assertGrade(MaintOutcome::PostconditionFail, MaintReason::BootUnitLost,
              bootUpdateRun(u, 5));
  TEST_ASSERT_EQUAL(1, u.reshows);
}

static void test_stage1_wrong_state_is_a_verify_failure() {
  FakeUnit u;
  u.stateAfterStage1 = BOOT_STATE_OLD;
  assertGrade(MaintOutcome::PostconditionFail, MaintReason::BootVerifyFailed,
              bootUpdateRun(u, 5));
  TEST_ASSERT_EQUAL(1, u.reshows);
  TEST_ASSERT_EQUAL((int)BootUpdateStep::Stage1, (int)u.lastStep);
}

// Resuming a unit that already carries page 7: it is homed first (#516).
static void test_resume_at_stage2_homes_first() {
  FakeUnit u;
  u.state = BOOT_STATE_PAGE7_INSTALLED;
  assertGrade(MaintOutcome::Ok, MaintReason::None, bootUpdateRun(u, 5));
  std::vector<std::string> want = {"home", "stage2", "reshow"};
  TEST_ASSERT_TRUE(want == u.calls);
  TEST_ASSERT_EQUAL(0, u.invalidated);
}

static void test_stage2_nack_reshows_the_homed_row() {
  FakeUnit u;
  u.state = BOOT_STATE_PAGE7_INSTALLED;
  u.stage2Ack = 2;
  assertGrade(MaintOutcome::WireFail, MaintReason::None, bootUpdateRun(u, 5));
  TEST_ASSERT_EQUAL(1, u.reshows);
}

static void test_stage2_failure_names_the_units_own_result() {
  FakeUnit u;
  u.state = BOOT_STATE_PAGE7_INSTALLED;
  u.stage2Lands = false;
  assertGrade(MaintOutcome::PostconditionFail, MaintReason::BootVerifyFailed,
              bootUpdateRun(u, 5));
  TEST_ASSERT_EQUAL(1, u.reshows);
  TEST_ASSERT_EQUAL(0, u.holds);
}

static void test_stage2_silence_is_a_lost_unit_after_the_poll_window() {
  FakeUnit u;
  u.state = BOOT_STATE_PAGE7_INSTALLED;
  u.silentAfterStage2 = true;
  uint32_t before = u.now;
  assertGrade(MaintOutcome::PostconditionFail, MaintReason::BootUnitLost,
              bootUpdateRun(u, 5));
  TEST_ASSERT_TRUE(u.now - before >= BOOT_UPDATE_STAGE2_POLL_MS);
}

// --- BootDumpOp.h ---------------------------------------------------------------

namespace {

struct DumpUnit {
  int enterAck = 0;
  UnitBootReadResult read = UnitBootReadResult::Ok;
  std::vector<std::string> calls;
  int holds = 0;
  int invalidated = 0;

  int enterBootloader(uint8_t) {
    calls.push_back("enter");
    return enterAck;
  }
  void unitLeftSketch(uint8_t) { invalidated++; }
  void holdProbes() { holds++; }
  void pause(uint32_t) {}
  UnitBootReadResult readBootSection(uint8_t, uint8_t* out) {
    calls.push_back("read");
    out[0] = 0xAB;
    return read;
  }
  bool waitIdle(uint8_t, uint32_t) { return true; }
  int home(uint8_t) {
    calls.push_back("home");
    return 0;
  }
  void reshow() { calls.push_back("reshow"); }
};

}  // namespace

static void test_dump_reads_then_homes_and_reshows() {
  DumpUnit u;
  uint8_t out[4] = {0};
  TEST_ASSERT_EQUAL((int)BootDumpOutcome::Ok, (int)bootDumpRun(u, 5, out));
  std::vector<std::string> want = {"enter", "read", "home", "reshow"};
  TEST_ASSERT_TRUE(want == u.calls);
  TEST_ASSERT_EQUAL(1, u.invalidated);
  TEST_ASSERT_EQUAL(1, u.holds);
  TEST_ASSERT_EQUAL_HEX8(0xAB, out[0]);
}

// A NACK does not prove the unit stayed in its sketch: probes are held, and
// nothing else is sent.
static void test_dump_enter_nack_only_holds_the_probes() {
  DumpUnit u;
  u.enterAck = 2;
  uint8_t out[4];
  TEST_ASSERT_EQUAL((int)BootDumpOutcome::EnterFail, (int)bootDumpRun(u, 5, out));
  TEST_ASSERT_EQUAL(1, (int)u.calls.size());
  TEST_ASSERT_EQUAL(0, u.invalidated);
  TEST_ASSERT_EQUAL(1, u.holds);
}

// Whatever the read came to, the unit was restarted: it is homed and the row
// re-shown on every read outcome.
static void test_dump_failed_read_still_homes_and_reshows() {
  const UnitBootReadResult results[] = {UnitBootReadResult::BootloaderSilent,
                                        UnitBootReadResult::ChipMismatch,
                                        UnitBootReadResult::ReadFailed};
  const BootDumpOutcome outcomes[] = {BootDumpOutcome::BootloaderSilent,
                                      BootDumpOutcome::ChipMismatch,
                                      BootDumpOutcome::ReadFail};
  for (int i = 0; i < 3; i++) {
    DumpUnit u;
    u.read = results[i];
    uint8_t out[4];
    TEST_ASSERT_EQUAL((int)outcomes[i], (int)bootDumpRun(u, 5, out));
    TEST_ASSERT_TRUE(u.calls.back() == "reshow");
    TEST_ASSERT_EQUAL(1, u.holds);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_dump_reads_then_homes_and_reshows);
  RUN_TEST(test_dump_enter_nack_only_holds_the_probes);
  RUN_TEST(test_dump_failed_read_still_homes_and_reshows);
  RUN_TEST(test_old_unit_runs_both_stages_and_reshows_once);
  RUN_TEST(test_already_new_is_ok_with_a_reason_and_touches_nothing);
  RUN_TEST(test_unreadable_report_fails_before_anything_is_sent);
  RUN_TEST(test_moving_drum_ends_the_op_as_busy);
  RUN_TEST(test_stage1_nack_is_a_wire_fail_without_reshow);
  RUN_TEST(test_stage1_refusal_reports_what_the_unit_said);
  RUN_TEST(test_stage1_without_a_recorded_refusal_is_not_started);
  RUN_TEST(test_unit_lost_after_stage1_reshows_the_row);
  RUN_TEST(test_stage1_wrong_state_is_a_verify_failure);
  RUN_TEST(test_resume_at_stage2_homes_first);
  RUN_TEST(test_stage2_nack_reshows_the_homed_row);
  RUN_TEST(test_stage2_failure_names_the_units_own_result);
  RUN_TEST(test_stage2_silence_is_a_lost_unit_after_the_poll_window);
  return UNITY_END();
}
