// Host-side tests for shared/UnitUpdateJob.h — the part of a row update that
// follows the flash loop: which units get their boot section looked at, and
// the sweep that updates them one at a time.

#include <unity.h>

#include <string.h>

#include <vector>

#include "UnitUpdateJob.h"

void setUp() {}
void tearDown() {}

namespace {

UnitFacts sketchUnit(uint8_t fwStatus, uint8_t bootVerdict) {
  UnitFacts u;
  u.state = 1;
  u.fwStatus = fwStatus;
  u.bootVerdict = bootVerdict;
  return u;
}

struct FakeRow {
  std::vector<MaintGrade> grades;  // one per bootUpdate call, in order
  int stopAfter = -1;              // stopRequested() turns true after N units
  std::vector<uint8_t> updated;
  std::vector<uint8_t> states;     // progress.state seen at each publish
  std::vector<uint8_t> current;    // progress.currentAddr at each publish
  int haltedAt = -1;
  int untouched = -1;
  ReflashProgress* progress = nullptr;

  bool stopRequested() { return stopAfter >= 0 && (int)updated.size() >= stopAfter; }
  MaintGrade bootUpdate(uint8_t addr) {
    MaintGrade g = grades[updated.size()];
    updated.push_back(addr);
    return g;
  }
  void progressChanged() {
    states.push_back((uint8_t)progress->state);
    current.push_back(progress->currentAddr);
  }
  void sweepHalted(uint8_t consecutive, int left) {
    haltedAt = consecutive;
    untouched = left;
  }
};

const MaintGrade kUpdated{MaintOutcome::Ok, MaintReason::None};
const MaintGrade kAlready{MaintOutcome::Ok, MaintReason::BootAlreadyNew};
const MaintGrade kFailed{MaintOutcome::PostconditionFail,
                         MaintReason::BootVerifyFailed};

}  // namespace

static void test_boot_targets_are_current_app_units_not_judged_ok() {
  UnitFacts row[6];
  row[0] = sketchUnit(0, BOOT_INTEGRITY_OUTDATED);  // the case: update it
  row[1] = sketchUnit(0, BOOT_INTEGRITY_OK);        // nothing to do
  row[2] = sketchUnit(1, BOOT_INTEGRITY_OUTDATED);  // old app: not yet
  row[3] = sketchUnit(0, BOOT_INTEGRITY_UNREAD);    // let the op read it
  row[4] = sketchUnit(0, BOOT_INTEGRITY_CORRUPT);   // the op will refuse, visibly
  row[5] = sketchUnit(0, BOOT_INTEGRITY_OUTDATED);
  row[5].state = 2;                                 // sitting in its bootloader
  uint8_t out[6];
  int n = unitUpdateCollectBootTargets(row, 6, 1, out);
  TEST_ASSERT_EQUAL(3, n);
  TEST_ASSERT_EQUAL_UINT8(1, out[0]);
  TEST_ASSERT_EQUAL_UINT8(4, out[1]);
  TEST_ASSERT_EQUAL_UINT8(5, out[2]);
}

static void test_boot_targets_respect_the_row_width() {
  UnitFacts row[4];
  for (auto& u : row) u = sketchUnit(0, BOOT_INTEGRITY_OUTDATED);
  uint8_t out[4];
  TEST_ASSERT_EQUAL(2, unitUpdateCollectBootTargets(row, 2, 1, out));
}

static void test_sketch_units_are_the_ones_waited_on() {
  UnitFacts row[3];
  row[0] = sketchUnit(0, BOOT_INTEGRITY_OK);
  row[1].state = 0;  // absent
  row[2] = sketchUnit(1, BOOT_INTEGRITY_UNREAD);
  uint8_t out[3];
  TEST_ASSERT_EQUAL(2, unitUpdateCollectSketchUnits(row, 3, 1, out));
  TEST_ASSERT_EQUAL_UINT8(1, out[0]);
  TEST_ASSERT_EQUAL_UINT8(3, out[1]);
}

static void test_sweep_updates_each_unit_and_counts_only_real_updates() {
  ReflashProgress p;
  reflashProgressBegin(p, 0);
  FakeRow row;
  row.progress = &p;
  row.grades = {kUpdated, kAlready, kUpdated};
  const uint8_t targets[] = {1, 2, 3};
  BootSweepEnd end = unitUpdateRunBootSweep(row, targets, 3, p);
  TEST_ASSERT_FALSE(end.cancelled);
  TEST_ASSERT_FALSE(end.halted);
  TEST_ASSERT_EQUAL(3, (int)row.updated.size());
  TEST_ASSERT_EQUAL_UINT8(2, p.bootDone);    // the already-current one is not "done"
  TEST_ASSERT_EQUAL_UINT8(0, p.bootFailed);
  TEST_ASSERT_EQUAL_UINT8(0, p.currentAddr);
  // The gate stays closed for the whole sweep and names the unit in hand.
  TEST_ASSERT_TRUE(reflashInProgress(p));
  TEST_ASSERT_EQUAL(6, (int)row.states.size());  // before and after each unit
  for (uint8_t seen : row.states) {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)ReflashState::BootUpdate, seen);
  }
  TEST_ASSERT_EQUAL_UINT8(1, row.current[0]);
  TEST_ASSERT_EQUAL_UINT8(3, row.current[4]);
}

static void test_one_failure_does_not_stop_the_sweep() {
  ReflashProgress p;
  FakeRow row;
  row.progress = &p;
  row.grades = {kFailed, kUpdated, kFailed, kUpdated};
  const uint8_t targets[] = {1, 2, 3, 4};
  BootSweepEnd end = unitUpdateRunBootSweep(row, targets, 4, p);
  TEST_ASSERT_FALSE(end.halted);
  TEST_ASSERT_EQUAL(4, (int)row.updated.size());
  TEST_ASSERT_EQUAL_UINT8(2, p.bootDone);
  TEST_ASSERT_EQUAL_UINT8(2, p.bootFailed);
}

static void test_two_failures_in_a_row_halt_the_sweep() {
  ReflashProgress p;
  FakeRow row;
  row.progress = &p;
  row.grades = {kUpdated, kFailed, kFailed, kUpdated, kUpdated};
  const uint8_t targets[] = {1, 2, 3, 4, 5};
  BootSweepEnd end = unitUpdateRunBootSweep(row, targets, 5, p);
  TEST_ASSERT_TRUE(end.halted);
  TEST_ASSERT_EQUAL(3, (int)row.updated.size());  // 4 and 5 never touched
  TEST_ASSERT_EQUAL(2, row.haltedAt);
  TEST_ASSERT_EQUAL(2, row.untouched);
  TEST_ASSERT_EQUAL_UINT8(1, p.bootDone);
  TEST_ASSERT_EQUAL_UINT8(2, p.bootFailed);
}

static void test_stop_ends_the_sweep_before_the_next_unit() {
  ReflashProgress p;
  FakeRow row;
  row.progress = &p;
  row.stopAfter = 1;
  row.grades = {kUpdated, kUpdated};
  const uint8_t targets[] = {1, 2};
  BootSweepEnd end = unitUpdateRunBootSweep(row, targets, 2, p);
  TEST_ASSERT_TRUE(end.cancelled);
  TEST_ASSERT_EQUAL(1, (int)row.updated.size());
}

static void test_empty_sweep_touches_nothing() {
  ReflashProgress p;
  reflashProgressBegin(p, 0);
  FakeRow row;
  row.progress = &p;
  BootSweepEnd end = unitUpdateRunBootSweep(row, nullptr, 0, p);
  TEST_ASSERT_FALSE(end.cancelled || end.halted);
  TEST_ASSERT_EQUAL(0, (int)row.states.size());
  TEST_ASSERT_EQUAL_UINT8((uint8_t)ReflashState::Entering, (uint8_t)p.state);
}

static void test_a_failed_boot_update_fails_the_job() {
  ReflashProgress p;
  reflashProgressBegin(p, 2);
  p.done = 2;
  p.bootFailed = 1;
  reflashProgressFinish(p, false, false);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)ReflashState::Failed, (uint8_t)p.state);
  MaintReason why;
  TEST_ASSERT_TRUE(classifyReflashOutcome(p, why) ==
                   MaintOutcome::PostconditionFail);
}

static void test_progress_json_carries_the_boot_counters() {
  ReflashProgress p;
  reflashProgressBegin(p, 16);
  p.done = 16;
  p.state = ReflashState::BootUpdate;
  p.currentAddr = 9;
  p.bootDone = 8;
  char buf[REFLASH_JSON_CAP];
  buildReflashJson(buf, sizeof(buf), p);
  TEST_ASSERT_EQUAL_STRING(
      "{\"state\":\"bootloader\",\"total\":16,\"done\":16,\"failed\":0,"
      "\"cur\":9,\"halted\":false,\"boot\":8,\"bootFailed\":0}",
      buf);
  // Widest object the fields can produce must fit the declared cap.
  p.state = ReflashState::Cancelled;
  p.total = p.done = p.failed = p.currentAddr = p.bootDone = p.bootFailed = 255;
  p.halted = false;
  buildReflashJson(buf, sizeof(buf), p);
  TEST_ASSERT_TRUE(strlen(buf) < REFLASH_JSON_CAP - 1);
  TEST_ASSERT_EQUAL_CHAR('}', buf[strlen(buf) - 1]);
}

static void test_begin_clears_the_boot_counters() {
  ReflashProgress p;
  p.bootDone = 3;
  p.bootFailed = 2;
  reflashProgressBegin(p, 1);
  TEST_ASSERT_EQUAL_UINT8(0, p.bootDone);
  TEST_ASSERT_EQUAL_UINT8(0, p.bootFailed);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_boot_targets_are_current_app_units_not_judged_ok);
  RUN_TEST(test_boot_targets_respect_the_row_width);
  RUN_TEST(test_sketch_units_are_the_ones_waited_on);
  RUN_TEST(test_sweep_updates_each_unit_and_counts_only_real_updates);
  RUN_TEST(test_one_failure_does_not_stop_the_sweep);
  RUN_TEST(test_two_failures_in_a_row_halt_the_sweep);
  RUN_TEST(test_stop_ends_the_sweep_before_the_next_unit);
  RUN_TEST(test_empty_sweep_touches_nothing);
  RUN_TEST(test_a_failed_boot_update_fails_the_job);
  RUN_TEST(test_progress_json_carries_the_boot_counters);
  RUN_TEST(test_begin_clears_the_boot_counters);
  return UNITY_END();
}
