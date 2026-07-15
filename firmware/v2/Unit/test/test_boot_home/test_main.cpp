// Host-side unit tests for the pure boot-home decision logic in
// BootHomePolicy.h (#309): which time-driven trigger fires when, and the
// stagger/jitter/hard-cap math. The calibrate()/loop() glue that acts on the
// verdict is bench tier.

#include <unity.h>
#include <stdint.h>
#include "../../BootHomePolicy.h"

void setUp() {}
void tearDown() {}

static BootHomeInputs make(bool homed, bool master, uint32_t elapsed,
                           uint8_t dip, uint16_t jitter) {
  BootHomeInputs in;
  in.homed = homed;
  in.masterEverContacted = master;
  in.elapsedMs = elapsed;
  in.dipIndex = dip;
  in.jitterMs = jitter;
  return in;
}

// --- an already-homed unit never self-homes --------------------------------

static void test_homed_never_self_homes() {
  // Even long past every deadline, once homed the trigger is dead.
  TEST_ASSERT_FALSE(bootHomeShouldSelfHome(make(true, false, 999999UL, 0, 0)));
  TEST_ASSERT_FALSE(bootHomeShouldSelfHome(make(true, true, 999999UL, 0, 0)));
}

// --- no-master path: staggered self-home -----------------------------------

static void test_no_master_before_deadline_waits() {
  // dip 0, no jitter -> deadline is exactly SELF_TIMEOUT. One ms early: wait.
  TEST_ASSERT_FALSE(bootHomeShouldSelfHome(
      make(false, false, BOOT_HOME_SELF_TIMEOUT_MS - 1, 0, 0)));
}

static void test_no_master_at_deadline_homes() {
  TEST_ASSERT_TRUE(bootHomeShouldSelfHome(
      make(false, false, BOOT_HOME_SELF_TIMEOUT_MS, 0, 0)));
}

static void test_stagger_scales_with_address() {
  // dip index 3 pushes the deadline out by 3*STAGGER. Just before: wait; at: go.
  uint32_t d = bootHomeSelfHomeDeadline(3, 0);
  TEST_ASSERT_EQUAL_UINT32(BOOT_HOME_SELF_TIMEOUT_MS + 3 * BOOT_HOME_STAGGER_MS, d);
  TEST_ASSERT_FALSE(bootHomeShouldSelfHome(make(false, false, d - 1, 3, 0)));
  TEST_ASSERT_TRUE(bootHomeShouldSelfHome(make(false, false, d, 3, 0)));
}

static void test_jitter_extends_deadline() {
  // Jitter adds directly to the deadline: a unit with 200 ms jitter still waits
  // at the un-jittered deadline.
  uint32_t base = bootHomeSelfHomeDeadline(2, 0);
  TEST_ASSERT_FALSE(bootHomeShouldSelfHome(make(false, false, base, 2, 200)));
  TEST_ASSERT_TRUE(bootHomeShouldSelfHome(make(false, false, base + 200, 2, 200)));
}

// --- master-seen path: hard cap only ---------------------------------------

static void test_master_seen_ignores_self_deadline() {
  // A master contacted us -> we do NOT self-home at the 30 s deadline; the
  // master owns the homing until the hard cap.
  TEST_ASSERT_FALSE(bootHomeShouldSelfHome(
      make(false, true, BOOT_HOME_SELF_TIMEOUT_MS + 5000, 0, 0)));
  // Even a high-address unit past its would-be staggered deadline holds.
  TEST_ASSERT_FALSE(bootHomeShouldSelfHome(
      make(false, true, BOOT_HOME_HARD_CAP_MS - 1, 15, 250)));
}

static void test_master_seen_homes_at_hard_cap() {
  // A driving master that never commanded a home: self-home at the cap so the
  // row is never permanently dark.
  TEST_ASSERT_TRUE(bootHomeShouldSelfHome(
      make(false, true, BOOT_HOME_HARD_CAP_MS, 15, 250)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_homed_never_self_homes);
  RUN_TEST(test_no_master_before_deadline_waits);
  RUN_TEST(test_no_master_at_deadline_homes);
  RUN_TEST(test_stagger_scales_with_address);
  RUN_TEST(test_jitter_extends_deadline);
  RUN_TEST(test_master_seen_ignores_self_deadline);
  RUN_TEST(test_master_seen_homes_at_hard_cap);
  return UNITY_END();
}
