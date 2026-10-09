// Host-side tests for ReflashPlan.h (#205) — the pure planning + progress
// core of the unit reflash job: who gets the enter-bootloader opcode, who
// gets flashed, the progress state machine displayTask publishes, and the
// job-level MaintResult grading.

#include <ArduinoFake.h>
#include <unity.h>

#include "ReflashPlan.h"

void setUp() {}
void tearDown() {}

// --- plan derivation ---------------------------------------------------------

static void test_needs_reboot_only_for_sketch_units_off_the_bundle() {
  UnitFacts u;
  u.state = 1; u.fwStatus = 1;  // sketch, outdated
  TEST_ASSERT_TRUE(reflashUnitNeedsReboot(u));
  u.fwStatus = 2;               // sketch, unknown rev — v1 #114: qualifies
  TEST_ASSERT_TRUE(reflashUnitNeedsReboot(u));
  u.fwStatus = 0;               // already on the bundled rev — skip
  TEST_ASSERT_FALSE(reflashUnitNeedsReboot(u));
  u.state = 0; u.fwStatus = 2;  // silent
  TEST_ASSERT_FALSE(reflashUnitNeedsReboot(u));
  u.state = 2;                  // already in bootloader — no reboot needed
  TEST_ASSERT_FALSE(reflashUnitNeedsReboot(u));
}

// --- protocol-version mismatch (#405) ----------------------------------------
// A unit reporting a wire contract we do not speak cannot be driven at all, so
// converging it is the only way it becomes useful again. Equality-based: the
// master cannot speak a contract it has no code for, so a HIGHER version is
// exactly as un-drivable as a lower one.

static void test_protocol_mismatch_needs_a_successful_read() {
  UnitFacts u;
  u.state = 1;
  u.fwStatus = 0;  // rev is current — only the protocol is wrong
  // Never read: absence of information, not proof of difference.
  u.protocolKnown = false;
  u.protocolVersion = 0;
  TEST_ASSERT_FALSE(reflashUnitProtocolMismatch(u));
  // Read, and it is ours.
  u.protocolKnown = true;
  u.protocolVersion = SFP_PROTOCOL_VERSION;
  TEST_ASSERT_FALSE(reflashUnitProtocolMismatch(u));
  // Read, and it is not ours.
  u.protocolVersion = (uint8_t)(SFP_PROTOCOL_VERSION + 1);
  TEST_ASSERT_TRUE(reflashUnitProtocolMismatch(u));
}

static void test_protocol_mismatch_forces_reboot_even_on_the_bundled_rev() {
  // The rev can match while the contract does not — a unit flashed from a
  // different build line, say. The mismatch alone must make it a target.
  UnitFacts u;
  u.state = 1;
  u.fwStatus = 0;
  u.protocolKnown = true;
  u.protocolVersion = (uint8_t)(SFP_PROTOCOL_VERSION + 1);
  TEST_ASSERT_TRUE(reflashUnitNeedsReboot(u));
}

static void test_protocol_mismatch_is_flashed_regardless_of_direction() {
  // "Newer" is not a thing we can act on, and the rev is a hash so it is not
  // orderable at all. Both directions converge on the master's bundle.
  UnitFacts lower;
  lower.state = 1; lower.fwStatus = 0;
  lower.protocolKnown = true; lower.protocolVersion = 0;
  TEST_ASSERT_TRUE(reflashUnitNeedsReboot(lower));

  UnitFacts higher;
  higher.state = 1; higher.fwStatus = 0;
  higher.protocolKnown = true; higher.protocolVersion = 0xFF;
  TEST_ASSERT_TRUE(reflashUnitNeedsReboot(higher));
}

static void test_protocol_mismatch_is_boot_auto_update_eligible() {
  // Unlike an unreadable rev, a mismatch is PROOF — the read succeeded and
  // reported a contract that is not ours. So it qualifies for the narrow boot
  // auto-update path, while fwStatus==2 (unreadable) still does not.
  UnitFacts facts[UNITS_AMOUNT];
  facts[0].state = 1; facts[0].fwStatus = 2;  // unreadable — still excluded
  facts[1].state = 1; facts[1].fwStatus = 0;
  facts[1].protocolKnown = true;
  facts[1].protocolVersion = (uint8_t)(SFP_PROTOCOL_VERSION + 1);
  uint8_t addrs[UNITS_AMOUNT];
  int n = reflashCollectOutdatedTargets(facts, UNITS_AMOUNT, 1, addrs);
  TEST_ASSERT_EQUAL(1, n);
  TEST_ASSERT_EQUAL_UINT8(2, addrs[0]);  // base 1 + index 1
}

static void test_protocol_mismatch_makes_a_unit_undrivable() {
  // The producer gate: no renders, no polls, no calibration. It stays state==1
  // so the reflash path can still reach it.
  UnitFacts u;
  u.state = 1;
  u.protocolKnown = true;
  u.protocolVersion = SFP_PROTOCOL_VERSION;
  TEST_ASSERT_TRUE(unitDrivable(u));
  u.protocolVersion = (uint8_t)(SFP_PROTOCOL_VERSION + 1);
  TEST_ASSERT_FALSE(unitDrivable(u));
  // Never read: we have no evidence against it, so it stays drivable rather
  // than the whole wall going dark on a transient read failure.
  u.protocolKnown = false;
  TEST_ASSERT_TRUE(unitDrivable(u));
}

static void test_collect_reboot_targets_fills_addresses() {
  UnitFacts facts[UNITS_AMOUNT];
  facts[0].state = 1; facts[0].fwStatus = 0;  // on bundle — skipped
  facts[1].state = 1; facts[1].fwStatus = 1;  // outdated
  facts[3].state = 1; facts[3].fwStatus = 2;  // unknown
  facts[4].state = 2;                         // bootloader — not a reboot target
  uint8_t addrs[UNITS_AMOUNT];
  int n = reflashCollectRebootTargets(facts, UNITS_AMOUNT, 1, addrs);
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL_UINT8(2, addrs[0]);  // base 1 + index 1
  TEST_ASSERT_EQUAL_UINT8(4, addrs[1]);  // base 1 + index 3
}

static void test_collect_flash_targets_takes_bootloader_units_only() {
  UnitFacts facts[UNITS_AMOUNT];
  facts[0].state = 2;
  facts[1].state = 1; facts[1].fwStatus = 1;  // still in sketch — not flashable
  facts[5].state = 2;
  uint8_t addrs[UNITS_AMOUNT];
  int n = reflashCollectFlashTargets(facts, UNITS_AMOUNT, 1, addrs);
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL_UINT8(1, addrs[0]);
  TEST_ASSERT_EQUAL_UINT8(6, addrs[1]);
}

static void test_collect_outdated_targets_skips_unknown_revs() {
  // Boot auto-update (v1 semantics): only units PROVABLY outdated get the
  // forced reboot at every boot — an unreadable rev (2) must not trigger a
  // reflash cycle each power-up; the operator's web job sweeps those.
  UnitFacts facts[UNITS_AMOUNT];
  facts[0].state = 1; facts[0].fwStatus = 1;  // outdated — target
  facts[1].state = 1; facts[1].fwStatus = 2;  // unknown — skipped at boot
  facts[2].state = 1; facts[2].fwStatus = 0;  // on bundle — skipped
  facts[3].state = 2;                         // bootloader — not a reboot target
  uint8_t addrs[UNITS_AMOUNT];
  int n = reflashCollectOutdatedTargets(facts, UNITS_AMOUNT, 1, addrs);
  TEST_ASSERT_EQUAL(1, n);
  TEST_ASSERT_EQUAL_UINT8(1, addrs[0]);
}

static void test_batch_constants() {
  // Batch raised 2 → 4 (#250) — bench-gated on the units' lifetime
  // brownout counters staying flat across a full 16-unit reflash.
  TEST_ASSERT_EQUAL(4, REFLASH_BATCH_SIZE);
  TEST_ASSERT_EQUAL(15000UL, REFLASH_BATCH_SETTLE_MS);
  TEST_ASSERT_EQUAL(500, TWIBOOT_STARTUP_MS);
}

// --- single-unit targeting (#412) --------------------------------------------
// The #407 image is a day-0 EEPROM erase on a contract that has never run on
// hardware, so the campaign flashes one unit, inspects it, and only then moves
// on. The filter composes with all three collectors rather than being threaded
// through each of them.

static void test_zero_address_means_the_whole_fleet() {
  uint8_t addrs[4] = {1, 2, 3, 4};
  TEST_ASSERT_EQUAL(4, reflashFilterToAddress(addrs, 4, 0));
  TEST_ASSERT_EQUAL_UINT8(1, addrs[0]);
  TEST_ASSERT_EQUAL_UINT8(4, addrs[3]);
}

static void test_a_targeted_address_narrows_to_exactly_that_unit() {
  uint8_t addrs[4] = {1, 2, 3, 4};
  TEST_ASSERT_EQUAL(1, reflashFilterToAddress(addrs, 4, 3));
  TEST_ASSERT_EQUAL_UINT8(3, addrs[0]);
}

// The targeted unit is not in the plan — it is already on the bundle, silent,
// or (for the flash phase) never made it into twiboot. An empty plan finishes
// Done/Ok, which is the honest answer: nothing to do at that address.
static void test_a_targeted_address_absent_from_the_plan_yields_nothing() {
  uint8_t addrs[3] = {1, 2, 4};
  TEST_ASSERT_EQUAL(0, reflashFilterToAddress(addrs, 3, 3));
  TEST_ASSERT_EQUAL(0, reflashFilterToAddress(addrs, 0, 1));
}

// The point of filtering BOTH phases: a unit stranded in twiboot by an earlier
// attempt must not be swept up by a run targeting a different address.
static void test_filtering_the_flash_phase_leaves_a_stranded_unit_alone() {
  UnitFacts facts[4];
  for (int i = 0; i < 4; i++) facts[i] = UnitFacts{};
  facts[1].state = 2;  // addr 2 stranded in twiboot from a previous attempt
  facts[2].state = 2;  // addr 3 is the one we are targeting now
  uint8_t addrs[4];
  int n = reflashCollectFlashTargets(facts, 4, 1, addrs);
  TEST_ASSERT_EQUAL(2, n);
  n = reflashFilterToAddress(addrs, n, 3);
  TEST_ASSERT_EQUAL(1, n);
  TEST_ASSERT_EQUAL_UINT8(3, addrs[0]);
}

// --- consecutive-failure halt (#412) ----------------------------------------

// The halt has to be legible over the API, not just on the serial log: a run
// that stopped early and a run that finished with the same failure count are
// otherwise the same JSON, and telling those apart is the whole point.
static void test_halted_is_distinct_from_a_completed_run_with_failures() {
  ReflashProgress halted;
  reflashProgressBegin(halted, 4);
  reflashProgressUnitResult(halted, false);
  reflashProgressUnitResult(halted, false);
  reflashProgressFinish(halted, false, true);

  ReflashProgress ranOut;
  reflashProgressBegin(ranOut, 4);
  reflashProgressUnitResult(ranOut, false);
  reflashProgressUnitResult(ranOut, false);
  reflashProgressFinish(ranOut, false, false);

  // Same counters, same state — only `halted` separates them.
  TEST_ASSERT_EQUAL(halted.failed, ranOut.failed);
  TEST_ASSERT_TRUE(halted.state == ranOut.state);
  TEST_ASSERT_TRUE(halted.halted);
  TEST_ASSERT_FALSE(ranOut.halted);
}

// The operator pulled the plug — that is the reason, not a suspect image.
static void test_cancel_outranks_halted() {
  ReflashProgress p;
  reflashProgressBegin(p, 4);
  reflashProgressUnitResult(p, false);
  reflashProgressFinish(p, true, true);
  TEST_ASSERT_TRUE(p.state == ReflashState::Cancelled);
  TEST_ASSERT_FALSE(p.halted);
}

static void test_begin_clears_a_previous_halt() {
  ReflashProgress p;
  reflashProgressBegin(p, 2);
  reflashProgressUnitResult(p, false);
  reflashProgressFinish(p, false, true);
  TEST_ASSERT_TRUE(p.halted);
  reflashProgressBegin(p, 2);  // next job must not inherit it
  TEST_ASSERT_FALSE(p.halted);
}


static void test_a_lone_failure_does_not_halt_the_run() {
  // a15 is hall-dead. It must not wedge every future fleet converge.
  TEST_ASSERT_FALSE(reflashShouldHalt(0));
  TEST_ASSERT_FALSE(reflashShouldHalt(1));
}

static void test_two_in_a_row_halts() {
  TEST_ASSERT_TRUE(reflashShouldHalt(REFLASH_MAX_CONSECUTIVE_FAILURES));
  TEST_ASSERT_TRUE(reflashShouldHalt(REFLASH_MAX_CONSECUTIVE_FAILURES + 1));
}

// The threshold has to be low enough that a bad image cannot take the row.
static void test_the_halt_threshold_bounds_the_damage() {
  TEST_ASSERT_LESS_THAN_UINT8(REFLASH_BATCH_SIZE,
                              REFLASH_MAX_CONSECUTIVE_FAILURES);
}

// --- progress state machine ----------------------------------------------------

static void test_fresh_progress_is_idle_and_not_in_progress() {
  ReflashProgress p;
  TEST_ASSERT_EQUAL(ReflashState::Idle, p.state);
  TEST_ASSERT_FALSE(reflashInProgress(p));
}

static void test_begin_enters_and_counts() {
  ReflashProgress p;
  reflashProgressBegin(p, 12);
  TEST_ASSERT_EQUAL(ReflashState::Entering, p.state);
  TEST_ASSERT_EQUAL(12, p.total);
  TEST_ASSERT_EQUAL(0, p.done);
  TEST_ASSERT_EQUAL(0, p.failed);
  TEST_ASSERT_TRUE(reflashInProgress(p));
}

static void test_unit_start_and_results_accumulate() {
  ReflashProgress p;
  reflashProgressBegin(p, 3);
  reflashProgressUnitStart(p, 5);
  TEST_ASSERT_EQUAL(ReflashState::Flashing, p.state);
  TEST_ASSERT_EQUAL_UINT8(5, p.currentAddr);
  reflashProgressUnitResult(p, true);
  reflashProgressUnitStart(p, 6);
  reflashProgressUnitResult(p, false);
  TEST_ASSERT_EQUAL(1, p.done);
  TEST_ASSERT_EQUAL(1, p.failed);
  TEST_ASSERT_TRUE(reflashInProgress(p));
}

static void test_settling_is_still_in_progress() {
  ReflashProgress p;
  reflashProgressBegin(p, 4);
  reflashProgressSettling(p);
  TEST_ASSERT_EQUAL(ReflashState::Settling, p.state);
  TEST_ASSERT_TRUE(reflashInProgress(p));
}

static void test_finish_grades_done_cancelled_failed() {
  ReflashProgress p;
  reflashProgressBegin(p, 2);
  reflashProgressUnitStart(p, 1);
  reflashProgressUnitResult(p, true);
  reflashProgressFinish(p, false, false);
  TEST_ASSERT_EQUAL(ReflashState::Done, p.state);
  TEST_ASSERT_EQUAL_UINT8(0, p.currentAddr);
  TEST_ASSERT_FALSE(reflashInProgress(p));

  ReflashProgress c;
  reflashProgressBegin(c, 2);
  reflashProgressFinish(c, true, false);
  TEST_ASSERT_EQUAL(ReflashState::Cancelled, c.state);

  ReflashProgress f;
  reflashProgressBegin(f, 2);
  reflashProgressUnitStart(f, 1);
  reflashProgressUnitResult(f, false);
  reflashProgressFinish(f, false, false);
  TEST_ASSERT_EQUAL(ReflashState::Failed, f.state);
}

static void test_cancel_wins_over_failures_in_grading() {
  ReflashProgress p;
  reflashProgressBegin(p, 3);
  reflashProgressUnitStart(p, 1);
  reflashProgressUnitResult(p, false);
  reflashProgressFinish(p, true, false);
  TEST_ASSERT_EQUAL(ReflashState::Cancelled, p.state);
}

static void test_state_names() {
  TEST_ASSERT_EQUAL_STRING("idle", reflashStateName(ReflashState::Idle));
  TEST_ASSERT_EQUAL_STRING("entering", reflashStateName(ReflashState::Entering));
  TEST_ASSERT_EQUAL_STRING("flashing", reflashStateName(ReflashState::Flashing));
  TEST_ASSERT_EQUAL_STRING("settling", reflashStateName(ReflashState::Settling));
  TEST_ASSERT_EQUAL_STRING("done", reflashStateName(ReflashState::Done));
  TEST_ASSERT_EQUAL_STRING("cancelled", reflashStateName(ReflashState::Cancelled));
  TEST_ASSERT_EQUAL_STRING("failed", reflashStateName(ReflashState::Failed));
}

// --- job-level MaintResult grading ---------------------------------------------

static void test_classify_done_job_is_ok() {
  ReflashProgress p;
  reflashProgressBegin(p, 2);
  reflashProgressUnitStart(p, 1);
  reflashProgressUnitResult(p, true);
  reflashProgressUnitStart(p, 2);
  reflashProgressUnitResult(p, true);
  reflashProgressFinish(p, false, false);
  MaintReason reason;
  TEST_ASSERT_EQUAL(MaintOutcome::Ok, classifyReflashOutcome(p, reason));
  TEST_ASSERT_EQUAL(MaintReason::None, reason);
}

static void test_classify_failed_and_cancelled_jobs() {
  ReflashProgress f;
  reflashProgressBegin(f, 1);
  reflashProgressUnitStart(f, 1);
  reflashProgressUnitResult(f, false);
  reflashProgressFinish(f, false, false);
  MaintReason reason;
  TEST_ASSERT_EQUAL(MaintOutcome::PostconditionFail,
                    classifyReflashOutcome(f, reason));

  ReflashProgress c;
  reflashProgressBegin(c, 1);
  reflashProgressFinish(c, true, false);
  TEST_ASSERT_EQUAL(MaintOutcome::PostconditionFail,
                    classifyReflashOutcome(c, reason));
}

static void test_empty_plan_finishes_done_and_ok() {
  // Every unit already on the bundled rev: begin(0) + finish is a no-op job
  // that must still grade ok — the v1 semantics for "nothing to do".
  ReflashProgress p;
  reflashProgressBegin(p, 0);
  reflashProgressFinish(p, false, false);
  TEST_ASSERT_EQUAL(ReflashState::Done, p.state);
  MaintReason reason;
  TEST_ASSERT_EQUAL(MaintOutcome::Ok, classifyReflashOutcome(p, reason));
}

// --- the flash loop (reflashRunTargets) -----------------------------------------

namespace {

struct LoopHooks {
  // script
  uint8_t failAddrs[8] = {0};
  int failCount = 0;
  uint8_t stopAtAddr = 0;      // flashUnit reports Stopped for this unit
  int stopBeforeUnit = -1;     // stopRequested() turns true at this call
  uint8_t sketchAddrs[8] = {0};  // units running their firmware at the start
  int sketchCount = 0;
  uint8_t deafAddrs[8] = {0};    // do not take the enter-bootloader order
  int deafCount = 0;
  uint8_t stayAddrs[8] = {0};    // take the order and come back in the sketch
  int stayCount = 0;
  uint8_t flakyAddr = 0;         // its flash fails this many times, then works
  int flakyFails = 0;
  uint8_t leavesAddr = 0;        // after a failed flash it is in its sketch again
  uint8_t resetAddr = 0;         // deaf to the order; reset by hand after
  int resetAfterProbes = -1;     // this many more questions
  uint8_t lateAddr = 0;          // takes the order, answers as a bootloader
  int lateProbes = 0;            // only after this many more questions
  bool fits = true;
  // observations
  int asked = 0;
  int flashCalls = 0;
  int settles = 0;
  int settledUnits = 0;
  int publishes = 0;
  int haltedLeft = -1;
  uint8_t flashedAddrs[16] = {0};
  int flashedCount = 0;
  int enterCalls = 0;
  int probes = 0;
  bool lateOrdered = false;
  uint32_t pausedMs = 0;
  uint8_t notEnteredAddrs[16] = {0};
  int notEnteredCount = 0;
  int retries = 0;
  int waitCues = 0;
  char order[96] = "";  // "e<addr>" an order sent, "f<addr>" a flash started

  static bool has(const uint8_t* list, int n, uint8_t addr) {
    for (int i = 0; i < n; i++) {
      if (list[i] == addr) return true;
    }
    return false;
  }
  void note(char what, uint8_t addr) {
    size_t at = strlen(order);
    snprintf(order + at, sizeof(order) - at, "%s%c%u", at ? " " : "", what,
             (unsigned)addr);
  }

  bool stopRequested() { return stopBeforeUnit >= 0 && asked++ >= stopBeforeUnit; }
  bool imageFits() { return fits; }
  bool inBootloader(uint8_t addr) {
    probes++;
    if (addr == resetAddr && resetAfterProbes >= 0 && resetAfterProbes-- == 0) {
      forget(addr);
    }
    if (addr == lateAddr && lateOrdered) {
      if (lateProbes-- > 0) return false;
      forget(addr);
    }
    return !has(sketchAddrs, sketchCount, addr);
  }
  void forget(uint8_t addr) {
    for (int i = 0; i < sketchCount; i++) {
      if (sketchAddrs[i] == addr) sketchAddrs[i] = 0;
    }
  }
  int enterBootloader(uint8_t addr) {
    enterCalls++;
    note('e', addr);
    if (has(deafAddrs, deafCount, addr)) return 2;
    if (has(stayAddrs, stayCount, addr)) return 0;
    if (addr == lateAddr) {
      lateOrdered = true;
      return 0;
    }
    forget(addr);
    return 0;
  }
  void pause(uint32_t ms) { pausedMs += ms; }
  void unitNotEntered(uint8_t addr) { notEnteredAddrs[notEnteredCount++] = addr; }
  void waitingForReset(uint8_t) { waitCues++; }
  void unitRetried(uint8_t, int) { retries++; }
  ReflashUnitOutcome flashUnit(uint8_t addr) {
    flashCalls++;
    note('f', addr);
    if (!fits) return ReflashUnitOutcome::Failed;
    if (addr == stopAtAddr) return ReflashUnitOutcome::Stopped;
    if (addr == flakyAddr && flakyFails > 0) {
      flakyFails--;
      return ReflashUnitOutcome::Failed;
    }
    if (addr == leavesAddr) {
      sketchAddrs[sketchCount++] = addr;
      return ReflashUnitOutcome::Failed;
    }
    if (has(failAddrs, failCount, addr)) return ReflashUnitOutcome::Failed;
    return ReflashUnitOutcome::Flashed;
  }
  void unitFlashed(uint8_t addr) { flashedAddrs[flashedCount++] = addr; }
  void progressChanged() { publishes++; }
  void settleBatch(const uint8_t*, int n) {
    settles++;
    settledUnits += n;
  }
  void runHalted(uint8_t, int left) { haltedLeft = left; }
};

}  // namespace

static void test_loop_flashes_every_target_in_batches() {
  LoopHooks h;
  ReflashProgress p;
  uint8_t targets[5] = {1, 2, 3, 4, 5};
  reflashProgressBegin(p, 5);
  ReflashRunEnd end = reflashRunTargets(h, targets, 5, p);
  TEST_ASSERT_FALSE(end.cancelled);
  TEST_ASSERT_FALSE(end.halted);
  TEST_ASSERT_EQUAL_UINT8(5, end.flashed);
  TEST_ASSERT_EQUAL_UINT8(5, p.done);
  // Every flashed unit is waited for exactly once, full batches then the rest.
  TEST_ASSERT_EQUAL(5, h.settledUnits);
  TEST_ASSERT_EQUAL((5 + REFLASH_BATCH_SIZE - 1) / REFLASH_BATCH_SIZE, h.settles);
  TEST_ASSERT_TRUE(h.publishes > 0);
}

static void test_loop_halts_on_two_failures_in_a_row_and_touches_no_more() {
  LoopHooks h;
  h.failAddrs[0] = 2;
  h.failAddrs[1] = 3;
  h.failCount = 2;
  ReflashProgress p;
  uint8_t targets[5] = {1, 2, 3, 4, 5};
  reflashProgressBegin(p, 5);
  ReflashRunEnd end = reflashRunTargets(h, targets, 5, p);
  TEST_ASSERT_TRUE(end.halted);
  // Unit 1 once, units 2 and 3 as often as a failing unit is tried; units 4
  // and 5 were never touched.
  TEST_ASSERT_EQUAL(1 + 2 * REFLASH_UNIT_ATTEMPTS, h.flashCalls);
  TEST_ASSERT_EQUAL(2, h.haltedLeft);
  TEST_ASSERT_EQUAL_UINT8(2, p.failed);
  // The unit flashed before the halt still gets its settle.
  TEST_ASSERT_EQUAL(1, h.settledUnits);
}

static void test_loop_isolated_failure_does_not_halt() {
  LoopHooks h;
  h.failAddrs[0] = 2;
  h.failAddrs[1] = 4;
  h.failCount = 2;
  ReflashProgress p;
  uint8_t targets[5] = {1, 2, 3, 4, 5};
  reflashProgressBegin(p, 5);
  ReflashRunEnd end = reflashRunTargets(h, targets, 5, p);
  TEST_ASSERT_FALSE(end.halted);
  TEST_ASSERT_EQUAL(3 + 2 * REFLASH_UNIT_ATTEMPTS, h.flashCalls);
  TEST_ASSERT_EQUAL_UINT8(3, p.done);
  TEST_ASSERT_EQUAL_UINT8(2, p.failed);
}

static void test_loop_stop_request_before_a_unit_cancels() {
  LoopHooks h;
  h.stopBeforeUnit = 2;
  ReflashProgress p;
  uint8_t targets[5] = {1, 2, 3, 4, 5};
  reflashProgressBegin(p, 5);
  ReflashRunEnd end = reflashRunTargets(h, targets, 5, p);
  TEST_ASSERT_TRUE(end.cancelled);
  TEST_ASSERT_EQUAL(2, h.flashCalls);
  // Brownout pacing is never shortened: the flashed units are settled.
  TEST_ASSERT_EQUAL(2, h.settledUnits);
}

static void test_loop_unit_stopped_mid_flash_counts_as_failed_and_cancels() {
  LoopHooks h;
  h.stopAtAddr = 2;
  ReflashProgress p;
  uint8_t targets[3] = {1, 2, 3};
  reflashProgressBegin(p, 3);
  ReflashRunEnd end = reflashRunTargets(h, targets, 3, p);
  TEST_ASSERT_TRUE(end.cancelled);
  TEST_ASSERT_FALSE(end.halted);
  TEST_ASSERT_EQUAL_UINT8(1, p.done);
  TEST_ASSERT_EQUAL_UINT8(1, p.failed);
  TEST_ASSERT_EQUAL(2, h.flashCalls);
}

static void test_loop_with_no_targets_does_nothing() {
  LoopHooks h;
  ReflashProgress p;
  reflashProgressBegin(p, 0);
  ReflashRunEnd end = reflashRunTargets(h, nullptr, 0, p);
  TEST_ASSERT_EQUAL_UINT8(0, end.flashed);
  TEST_ASSERT_EQUAL(0, h.settles);
}

// --- a unit enters its bootloader for its own flash only (#577) ---------------

static void test_loop_enters_each_unit_right_before_its_own_flash() {
  LoopHooks h;
  for (uint8_t a = 1; a <= 5; a++) h.sketchAddrs[h.sketchCount++] = a;
  ReflashProgress p;
  uint8_t targets[5] = {1, 2, 3, 4, 5};
  reflashProgressBegin(p, 5);
  ReflashRunEnd end = reflashRunTargets(h, targets, 5, p);
  // No unit waits in its bootloader while another one is flashed.
  TEST_ASSERT_EQUAL_STRING("e1 f1 e2 f2 e3 f3 e4 f4 e5 f5", h.order);
  TEST_ASSERT_EQUAL_UINT32(5 * TWIBOOT_STARTUP_MS, h.pausedMs);
  TEST_ASSERT_EQUAL_UINT8(5, end.flashed);
  TEST_ASSERT_EQUAL_UINT8(0, end.notEntered);
}

static void test_loop_sends_no_order_to_a_unit_already_in_its_bootloader() {
  LoopHooks h;
  h.sketchAddrs[h.sketchCount++] = 2;
  ReflashProgress p;
  uint8_t targets[3] = {1, 2, 3};
  reflashProgressBegin(p, 3);
  reflashRunTargets(h, targets, 3, p);
  TEST_ASSERT_EQUAL_STRING("f1 e2 f2 f3", h.order);
  TEST_ASSERT_EQUAL_UINT32(TWIBOOT_STARTUP_MS, h.pausedMs);
}

static void test_loop_unit_that_refuses_the_order_is_not_flashed() {
  LoopHooks h;
  h.sketchAddrs[h.sketchCount++] = 2;
  h.deafAddrs[h.deafCount++] = 2;
  ReflashProgress p;
  uint8_t targets[3] = {1, 2, 3};
  reflashProgressBegin(p, 3);
  ReflashRunEnd end = reflashRunTargets(h, targets, 3, p);
  TEST_ASSERT_EQUAL_STRING("f1 e2 f3", h.order);
  TEST_ASSERT_EQUAL_UINT32(0, h.pausedMs);  // nothing to wait for
  TEST_ASSERT_EQUAL_UINT8(2, end.flashed);
  TEST_ASSERT_EQUAL_UINT8(1, end.notEntered);
  TEST_ASSERT_EQUAL_UINT8(1, p.failed);  // the job did not bring it up to date
  TEST_ASSERT_EQUAL(1, h.notEnteredCount);
  TEST_ASSERT_EQUAL_UINT8(2, h.notEnteredAddrs[0]);
}

static void test_loop_unit_back_in_its_sketch_after_the_order_is_not_flashed() {
  LoopHooks h;
  h.sketchAddrs[h.sketchCount++] = 2;
  h.stayAddrs[h.stayCount++] = 2;
  ReflashProgress p;
  uint8_t targets[2] = {1, 2};
  reflashProgressBegin(p, 2);
  ReflashRunEnd end = reflashRunTargets(h, targets, 2, p);
  // Pages are never sent to a unit that is running its firmware.
  TEST_ASSERT_EQUAL_STRING("f1 e2", h.order);
  TEST_ASSERT_EQUAL_UINT8(1, end.notEntered);
}

static void test_loop_asks_a_slow_unit_again_before_giving_it_up() {
  LoopHooks h;
  h.sketchAddrs[h.sketchCount++] = 2;
  h.lateAddr = 2;
  h.lateProbes = REFLASH_ENTER_PROBES - 1;  // answers the last question
  ReflashProgress p;
  uint8_t targets[1] = {2};
  reflashProgressBegin(p, 1);
  ReflashRunEnd end = reflashRunTargets(h, targets, 1, p);
  TEST_ASSERT_EQUAL_UINT8(1, end.flashed);
  TEST_ASSERT_EQUAL_STRING("e2 f2", h.order);
  TEST_ASSERT_EQUAL_UINT32(
      TWIBOOT_STARTUP_MS + (REFLASH_ENTER_PROBES - 1) * REFLASH_ENTER_PROBE_GAP_MS,
      h.pausedMs);
}

// A unit whose flash failed holds a part of the image and would start it:
// it is flashed again while it is still in its bootloader.
static void test_loop_flashes_a_failed_unit_again_at_once() {
  LoopHooks h;
  h.flakyAddr = 3;
  h.flakyFails = 2;
  ReflashProgress p;
  uint8_t targets[3] = {2, 3, 4};
  reflashProgressBegin(p, 3);
  ReflashRunEnd end = reflashRunTargets(h, targets, 3, p);
  TEST_ASSERT_EQUAL_UINT8(3, end.flashed);
  TEST_ASSERT_EQUAL(2, h.retries);
  TEST_ASSERT_EQUAL_STRING("f2 f3 f3 f3 f4", h.order);
  TEST_ASSERT_EQUAL_UINT8(0, p.failed);
}

static void test_loop_gives_a_unit_up_after_its_attempts() {
  LoopHooks h;
  h.failAddrs[h.failCount++] = 3;
  ReflashProgress p;
  uint8_t targets[2] = {3, 4};
  reflashProgressBegin(p, 2);
  ReflashRunEnd end = reflashRunTargets(h, targets, 2, p);
  TEST_ASSERT_EQUAL_UINT8(1, end.flashed);
  TEST_ASSERT_EQUAL(REFLASH_UNIT_ATTEMPTS - 1, h.retries);
  TEST_ASSERT_EQUAL_UINT8(1, p.failed);  // one unit, however often it was tried
}

// Pages are only ever sent to a unit that says it is in its bootloader.
static void test_loop_does_not_flash_again_a_unit_that_left_its_bootloader() {
  LoopHooks h;
  h.leavesAddr = 3;
  ReflashProgress p;
  uint8_t targets[1] = {3};
  reflashProgressBegin(p, 1);
  reflashRunTargets(h, targets, 1, p);
  TEST_ASSERT_EQUAL(1, h.flashCalls);
  TEST_ASSERT_EQUAL(0, h.retries);
}

// A unit that no longer listens to the order: a run for that one unit waits
// for a reset by hand and catches the bootloader in its first second.
static void test_a_forced_run_waits_for_a_reset_by_hand() {
  LoopHooks h;
  h.sketchAddrs[h.sketchCount++] = 6;
  h.deafAddrs[h.deafCount++] = 6;
  h.resetAddr = 6;
  h.resetAfterProbes = 40;
  ReflashProgress p;
  uint8_t targets[1] = {6};
  reflashProgressBegin(p, 1);
  ReflashRunEnd end = reflashRunTargets(h, targets, 1, p, REFLASH_HAND_RESET_WAIT_MS);
  TEST_ASSERT_EQUAL(1, h.waitCues);
  TEST_ASSERT_EQUAL_UINT8(1, end.flashed);
  TEST_ASSERT_EQUAL_UINT8(0, end.notEntered);
  // Often enough that one question lands in the bootloader's one second.
  TEST_ASSERT_TRUE(REFLASH_HAND_RESET_GAP_MS * 3 <= 1000);
}

static void test_a_forced_run_gives_up_when_no_reset_comes() {
  LoopHooks h;
  h.sketchAddrs[h.sketchCount++] = 6;
  h.deafAddrs[h.deafCount++] = 6;
  ReflashProgress p;
  uint8_t targets[1] = {6};
  reflashProgressBegin(p, 1);
  ReflashRunEnd end = reflashRunTargets(h, targets, 1, p, REFLASH_HAND_RESET_WAIT_MS);
  TEST_ASSERT_EQUAL_UINT8(1, end.notEntered);
  TEST_ASSERT_EQUAL(0, h.flashCalls);
  TEST_ASSERT_TRUE(h.pausedMs >= REFLASH_HAND_RESET_WAIT_MS);
  TEST_ASSERT_TRUE(h.pausedMs < REFLASH_HAND_RESET_WAIT_MS + 2000);
}

// A stop during the wait cancels the run; the unit did not refuse anything.
static void test_a_stop_during_the_wait_cancels_the_run() {
  LoopHooks h;
  h.sketchAddrs[h.sketchCount++] = 6;
  h.deafAddrs[h.deafCount++] = 6;
  h.stopBeforeUnit = 5;  // the fifth time it is asked: well into the wait
  ReflashProgress p;
  uint8_t targets[1] = {6};
  reflashProgressBegin(p, 1);
  ReflashRunEnd end = reflashRunTargets(h, targets, 1, p, REFLASH_HAND_RESET_WAIT_MS);
  TEST_ASSERT_TRUE(end.cancelled);
  TEST_ASSERT_EQUAL_UINT8(0, end.notEntered);
  TEST_ASSERT_EQUAL(0, h.notEnteredCount);
  TEST_ASSERT_EQUAL(0, h.flashCalls);
  TEST_ASSERT_TRUE(h.pausedMs < 5000);
}

// A sweep of a row does not stand still for a unit that will not enter.
static void test_a_row_run_does_not_wait_for_a_reset() {
  LoopHooks h;
  h.sketchAddrs[h.sketchCount++] = 6;
  h.deafAddrs[h.deafCount++] = 6;
  ReflashProgress p;
  uint8_t targets[1] = {6};
  reflashProgressBegin(p, 1);
  reflashRunTargets(h, targets, 1, p);
  TEST_ASSERT_EQUAL(0, h.waitCues);
  TEST_ASSERT_TRUE(h.pausedMs < 1000);
}

static void test_loop_stops_asking_inside_the_bootloader_window() {
  LoopHooks h;
  h.sketchAddrs[h.sketchCount++] = 2;
  h.stayAddrs[h.stayCount++] = 2;
  ReflashProgress p;
  uint8_t targets[1] = {2};
  reflashProgressBegin(p, 1);
  ReflashRunEnd end = reflashRunTargets(h, targets, 1, p);
  TEST_ASSERT_EQUAL_UINT8(1, end.notEntered);
  TEST_ASSERT_EQUAL(1 + REFLASH_ENTER_PROBES, h.probes);  // one before the order
  // A unit that entered on time is asked while its bootloader still listens:
  // a question after it went back to its firmware could not be told from one
  // that never entered.
  TEST_ASSERT_TRUE(h.pausedMs < 1000);
}

static void test_loop_units_that_do_not_enter_never_halt_the_run() {
  // A unit that is not there to be flashed says nothing about the image.
  LoopHooks h;
  for (uint8_t a = 2; a <= 4; a++) {
    h.sketchAddrs[h.sketchCount++] = a;
    h.deafAddrs[h.deafCount++] = a;
  }
  ReflashProgress p;
  uint8_t targets[5] = {1, 2, 3, 4, 5};
  reflashProgressBegin(p, 5);
  ReflashRunEnd end = reflashRunTargets(h, targets, 5, p);
  TEST_ASSERT_FALSE(end.halted);
  TEST_ASSERT_EQUAL(-1, h.haltedLeft);
  TEST_ASSERT_EQUAL_UINT8(2, end.flashed);
  TEST_ASSERT_EQUAL_UINT8(3, end.notEntered);
  TEST_ASSERT_EQUAL_UINT8(3, p.failed);
}

static void test_loop_unit_that_does_not_enter_keeps_a_failure_streak() {
  // Only a flashed unit speaks for the image.
  LoopHooks h;
  h.failAddrs[h.failCount++] = 1;
  h.failAddrs[h.failCount++] = 3;
  h.sketchAddrs[h.sketchCount++] = 2;
  h.deafAddrs[h.deafCount++] = 2;
  ReflashProgress p;
  uint8_t targets[4] = {1, 2, 3, 4};
  reflashProgressBegin(p, 4);
  ReflashRunEnd end = reflashRunTargets(h, targets, 4, p);
  TEST_ASSERT_TRUE(end.halted);
  TEST_ASSERT_EQUAL(1, h.haltedLeft);
  TEST_ASSERT_EQUAL_STRING("f1 f1 f1 e2 f3 f3 f3", h.order);
}

static void test_loop_image_that_does_not_fit_sends_no_unit_anywhere() {
  LoopHooks h;
  h.fits = false;
  for (uint8_t a = 1; a <= 4; a++) h.sketchAddrs[h.sketchCount++] = a;
  ReflashProgress p;
  uint8_t targets[4] = {1, 2, 3, 4};
  reflashProgressBegin(p, 4);
  ReflashRunEnd end = reflashRunTargets(h, targets, 4, p);
  TEST_ASSERT_EQUAL(0, h.enterCalls);
  TEST_ASSERT_TRUE(end.halted);
  TEST_ASSERT_EQUAL(2, h.flashCalls);
}

static void test_plan_targets_joins_the_sweep_and_bootloader_units_in_order() {
  UnitFacts facts[6] = {};
  facts[0].state = 1;  // unit 1: sketch, not in the sweep
  facts[1].state = 2;  // unit 2: already in its bootloader
  facts[2].state = 1;  // unit 3: sketch, in the sweep
  facts[4].state = 2;  // unit 5: in its bootloader AND named by the sweep
  facts[5].state = 1;  // unit 6: sketch, in the sweep
  uint8_t sweep[3] = {6, 3, 5};
  uint8_t out[6] = {0};
  int n = reflashPlanTargets(facts, 6, 1, sweep, 3, out);
  TEST_ASSERT_EQUAL(4, n);
  uint8_t want[4] = {2, 3, 5, 6};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, 4);
}

static void test_plan_targets_without_a_sweep_is_the_bootloader_units() {
  UnitFacts facts[4] = {};
  facts[0].state = 1;
  facts[2].state = 2;
  uint8_t out[4] = {0};
  TEST_ASSERT_EQUAL(1, reflashPlanTargets(facts, 4, 1, nullptr, 0, out));
  TEST_ASSERT_EQUAL_UINT8(3, out[0]);
}

// --- force (#545): one named unit, whatever revision it reports ------------

static void test_force_targets_a_current_sketch_unit() {
  UnitFacts facts[4] = {};
  facts[1].state = 1;
  facts[1].fwStatus = 0;  // on the bundled rev: no ordinary sweep takes it
  uint8_t out[4];
  TEST_ASSERT_EQUAL(0, reflashCollectRebootTargets(facts, 4, 1, out));
  TEST_ASSERT_EQUAL(1, reflashCollectForcedTarget(facts, 4, 1, 2, out));
  TEST_ASSERT_EQUAL_UINT8(2, out[0]);
}

static void test_force_plans_nothing_without_one_reachable_sketch_unit() {
  UnitFacts facts[4] = {};
  facts[0].state = 1;
  facts[2].state = 2;  // already in twiboot: a flash target without a reboot
  uint8_t out[4];
  TEST_ASSERT_EQUAL(0, reflashCollectForcedTarget(facts, 4, 1, 0, out));  // no address = never the row
  TEST_ASSERT_EQUAL(0, reflashCollectForcedTarget(facts, 4, 1, 2, out));  // absent
  TEST_ASSERT_EQUAL(0, reflashCollectForcedTarget(facts, 4, 1, 3, out));  // in twiboot
  TEST_ASSERT_EQUAL(0, reflashCollectForcedTarget(facts, 4, 1, 5, out));  // past the row
}

static void test_force_value_parses_strictly() {
  bool force = false;
  TEST_ASSERT_TRUE(reflashParseForce("1", force));
  TEST_ASSERT_TRUE(force);
  TEST_ASSERT_TRUE(reflashParseForce("true", force));
  TEST_ASSERT_TRUE(force);
  TEST_ASSERT_TRUE(reflashParseForce("0", force));
  TEST_ASSERT_FALSE(force);
  TEST_ASSERT_TRUE(reflashParseForce("false", force));
  TEST_ASSERT_FALSE(force);
  TEST_ASSERT_FALSE(reflashParseForce("", force));
  TEST_ASSERT_FALSE(reflashParseForce("yes", force));
  TEST_ASSERT_FALSE(reflashParseForce("11", force));
  TEST_ASSERT_FALSE(reflashParseForce(nullptr, force));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_loop_flashes_every_target_in_batches);
  RUN_TEST(test_loop_halts_on_two_failures_in_a_row_and_touches_no_more);
  RUN_TEST(test_loop_isolated_failure_does_not_halt);
  RUN_TEST(test_loop_stop_request_before_a_unit_cancels);
  RUN_TEST(test_loop_unit_stopped_mid_flash_counts_as_failed_and_cancels);
  RUN_TEST(test_loop_with_no_targets_does_nothing);
  RUN_TEST(test_loop_enters_each_unit_right_before_its_own_flash);
  RUN_TEST(test_loop_sends_no_order_to_a_unit_already_in_its_bootloader);
  RUN_TEST(test_loop_unit_that_refuses_the_order_is_not_flashed);
  RUN_TEST(test_loop_unit_back_in_its_sketch_after_the_order_is_not_flashed);
  RUN_TEST(test_loop_asks_a_slow_unit_again_before_giving_it_up);
  RUN_TEST(test_loop_flashes_a_failed_unit_again_at_once);
  RUN_TEST(test_loop_gives_a_unit_up_after_its_attempts);
  RUN_TEST(test_loop_does_not_flash_again_a_unit_that_left_its_bootloader);
  RUN_TEST(test_a_forced_run_waits_for_a_reset_by_hand);
  RUN_TEST(test_a_forced_run_gives_up_when_no_reset_comes);
  RUN_TEST(test_a_stop_during_the_wait_cancels_the_run);
  RUN_TEST(test_a_row_run_does_not_wait_for_a_reset);
  RUN_TEST(test_loop_stops_asking_inside_the_bootloader_window);
  RUN_TEST(test_loop_units_that_do_not_enter_never_halt_the_run);
  RUN_TEST(test_loop_unit_that_does_not_enter_keeps_a_failure_streak);
  RUN_TEST(test_loop_image_that_does_not_fit_sends_no_unit_anywhere);
  RUN_TEST(test_plan_targets_joins_the_sweep_and_bootloader_units_in_order);
  RUN_TEST(test_plan_targets_without_a_sweep_is_the_bootloader_units);
  RUN_TEST(test_needs_reboot_only_for_sketch_units_off_the_bundle);
  RUN_TEST(test_protocol_mismatch_needs_a_successful_read);
  RUN_TEST(test_protocol_mismatch_forces_reboot_even_on_the_bundled_rev);
  RUN_TEST(test_protocol_mismatch_is_flashed_regardless_of_direction);
  RUN_TEST(test_protocol_mismatch_is_boot_auto_update_eligible);
  RUN_TEST(test_protocol_mismatch_makes_a_unit_undrivable);
  RUN_TEST(test_collect_reboot_targets_fills_addresses);
  RUN_TEST(test_collect_flash_targets_takes_bootloader_units_only);
  RUN_TEST(test_collect_outdated_targets_skips_unknown_revs);
  RUN_TEST(test_batch_constants);
  RUN_TEST(test_zero_address_means_the_whole_fleet);
  RUN_TEST(test_a_targeted_address_narrows_to_exactly_that_unit);
  RUN_TEST(test_a_targeted_address_absent_from_the_plan_yields_nothing);
  RUN_TEST(test_filtering_the_flash_phase_leaves_a_stranded_unit_alone);
  RUN_TEST(test_a_lone_failure_does_not_halt_the_run);
  RUN_TEST(test_two_in_a_row_halts);
  RUN_TEST(test_the_halt_threshold_bounds_the_damage);
  RUN_TEST(test_halted_is_distinct_from_a_completed_run_with_failures);
  RUN_TEST(test_cancel_outranks_halted);
  RUN_TEST(test_begin_clears_a_previous_halt);
  RUN_TEST(test_fresh_progress_is_idle_and_not_in_progress);
  RUN_TEST(test_begin_enters_and_counts);
  RUN_TEST(test_unit_start_and_results_accumulate);
  RUN_TEST(test_settling_is_still_in_progress);
  RUN_TEST(test_finish_grades_done_cancelled_failed);
  RUN_TEST(test_cancel_wins_over_failures_in_grading);
  RUN_TEST(test_state_names);
  RUN_TEST(test_classify_done_job_is_ok);
  RUN_TEST(test_classify_failed_and_cancelled_jobs);
  RUN_TEST(test_empty_plan_finishes_done_and_ok);
  RUN_TEST(test_force_targets_a_current_sketch_unit);
  RUN_TEST(test_force_plans_nothing_without_one_reachable_sketch_unit);
  RUN_TEST(test_force_value_parses_strictly);
  return UNITY_END();
}
