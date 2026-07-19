// Host-side tests for HeadlessPolicy.h (#329) — the pure no-units detection
// debounce and deviceRole wire vocabulary behind the headless-mode feature.
// No globals, no hardware: the whole policy runs natively.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../HeadlessPolicy.h"

void setUp() {}
void tearDown() {}

// --- deviceRole validator ---------------------------------------------------

static void test_role_validator_accepts_the_four_roles() {
  TEST_ASSERT_TRUE(isValidDeviceRoleValue(String(DEVICE_ROLE_DISPLAY)));
  TEST_ASSERT_TRUE(isValidDeviceRoleValue(String(DEVICE_ROLE_HEADLESS_BACKUP)));
  TEST_ASSERT_TRUE(isValidDeviceRoleValue(String(DEVICE_ROLE_HEADLESS_MONITOR)));
  TEST_ASSERT_TRUE(isValidDeviceRoleValue(String(DEVICE_ROLE_HEADLESS_SPARE)));
}

static void test_role_validator_rejects_case_empty_and_unknown() {
  TEST_ASSERT_FALSE(isValidDeviceRoleValue(String("Display")));
  TEST_ASSERT_FALSE(isValidDeviceRoleValue(String("")));
  TEST_ASSERT_FALSE(isValidDeviceRoleValue(String("headless")));
  TEST_ASSERT_FALSE(isValidDeviceRoleValue(String("backup")));
}

// --- isHeadlessRole ---------------------------------------------------------

static void test_is_headless_role_true_for_the_three_headless_roles() {
  TEST_ASSERT_TRUE(isHeadlessRole(String(DEVICE_ROLE_HEADLESS_BACKUP)));
  TEST_ASSERT_TRUE(isHeadlessRole(String(DEVICE_ROLE_HEADLESS_MONITOR)));
  TEST_ASSERT_TRUE(isHeadlessRole(String(DEVICE_ROLE_HEADLESS_SPARE)));
}

static void test_is_headless_role_false_for_display_and_junk() {
  TEST_ASSERT_FALSE(isHeadlessRole(String(DEVICE_ROLE_DISPLAY)));
  TEST_ASSERT_FALSE(isHeadlessRole(String("")));
  TEST_ASSERT_FALSE(isHeadlessRole(String("nonsense")));
}

// --- detection debounce -----------------------------------------------------

static void test_detector_needs_threshold_consecutive_zero_probes() {
  HeadlessDetector d;
  // Below threshold: not yet flagged.
  TEST_ASSERT_FALSE(headlessObserveProbe(d, 0, 3));  // streak 1
  TEST_ASSERT_FALSE(headlessObserveProbe(d, 0, 3));  // streak 2
  // The Nth consecutive zero latches the suggestion.
  TEST_ASSERT_TRUE(headlessObserveProbe(d, 0, 3));   // streak 3
}

static void test_detector_stays_true_and_saturates_past_threshold() {
  HeadlessDetector d;
  headlessObserveProbe(d, 0, 2);
  TEST_ASSERT_TRUE(headlessObserveProbe(d, 0, 2));
  // Many more zeros: stays true, streak counter must not overflow unbounded.
  for (int i = 0; i < 1000; i++) {
    TEST_ASSERT_TRUE(headlessObserveProbe(d, 0, 2));
  }
  TEST_ASSERT_TRUE(d.zeroStreak <= 2 + 1);  // saturated at/near threshold
}

static void test_a_single_unit_resets_the_streak() {
  HeadlessDetector d;
  headlessObserveProbe(d, 0, 3);  // streak 1
  headlessObserveProbe(d, 0, 3);  // streak 2
  // A real display whose units briefly reappear must never be flagged.
  TEST_ASSERT_FALSE(headlessObserveProbe(d, 1, 3));  // reset
  TEST_ASSERT_FALSE(headlessObserveProbe(d, 0, 3));  // streak 1 again
  TEST_ASSERT_FALSE(headlessObserveProbe(d, 0, 3));  // streak 2
  TEST_ASSERT_TRUE(headlessObserveProbe(d, 0, 3));   // streak 3
}

static void test_a_unit_after_latching_clears_the_suggestion() {
  HeadlessDetector d;
  headlessObserveProbe(d, 0, 1);  // latched true
  TEST_ASSERT_TRUE(d.zeroStreak >= 1);
  TEST_ASSERT_FALSE(headlessObserveProbe(d, 2, 1));  // units back -> cleared
  TEST_ASSERT_EQUAL_INT(0, d.zeroStreak);
}

// --- headlessShouldSuggest --------------------------------------------------

static void test_suggest_only_when_unitless_and_still_on_display_role() {
  // Detected unit-less AND user hasn't picked a headless role yet -> nudge.
  TEST_ASSERT_TRUE(headlessShouldSuggest(true, String(DEVICE_ROLE_DISPLAY)));
  // Already headless: no nudge.
  TEST_ASSERT_FALSE(headlessShouldSuggest(true, String(DEVICE_ROLE_HEADLESS_SPARE)));
  TEST_ASSERT_FALSE(headlessShouldSuggest(true, String(DEVICE_ROLE_HEADLESS_BACKUP)));
  // Units present: never nudge, whatever the role.
  TEST_ASSERT_FALSE(headlessShouldSuggest(false, String(DEVICE_ROLE_DISPLAY)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_role_validator_accepts_the_four_roles);
  RUN_TEST(test_role_validator_rejects_case_empty_and_unknown);
  RUN_TEST(test_is_headless_role_true_for_the_three_headless_roles);
  RUN_TEST(test_is_headless_role_false_for_display_and_junk);
  RUN_TEST(test_detector_needs_threshold_consecutive_zero_probes);
  RUN_TEST(test_detector_stays_true_and_saturates_past_threshold);
  RUN_TEST(test_a_single_unit_resets_the_streak);
  RUN_TEST(test_a_unit_after_latching_clears_the_suggestion);
  RUN_TEST(test_suggest_only_when_unitless_and_still_on_display_role);
  return UNITY_END();
}
