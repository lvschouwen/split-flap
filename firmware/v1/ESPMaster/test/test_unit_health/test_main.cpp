// Host-side unit tests for the pure unit-health logic in UnitHealth.h
// (#45 web card, #137 MQTT telemetry). No Wire, no networking — only the
// faulty predicate, the faulty-count, and the shared per-unit JSON assembly.

#include <ArduinoFake.h>
#include <unity.h>
#include <cstring>
#include "../../UnitHealth.h"

using namespace fakeit;

void setUp() { ArduinoFakeReset(); }
void tearDown() {}

static void assert_contains(const char* haystack, const char* needle) {
  if (strstr(haystack, needle) == nullptr) {
    char msg[256];
    snprintf(msg, sizeof(msg), "expected to find \"%s\"", needle);
    TEST_FAIL_MESSAGE(msg);
  }
}
static void assert_not_contains(const char* haystack, const char* needle) {
  if (strstr(haystack, needle) != nullptr) {
    char msg[256];
    snprintf(msg, sizeof(msg), "expected NOT to find \"%s\"", needle);
    TEST_FAIL_MESSAGE(msg);
  }
}

static UnitStatus healthyStatus() {
  UnitStatus s{};
  s.flags = 0; s.mcusrAtBoot = 0x01; s.lifetimeBrownoutCount = 0;
  s.lifetimeWatchdogCount = 0; s.uptimeSeconds = 1200; s.badCommandCount = 0;
  s.lastHomingStepCount = 720;
  return s;
}

// ---- unitStatusIsFaulty ----
static void test_faulty_false_when_healthy() {
  TEST_ASSERT_FALSE(unitStatusIsFaulty(healthyStatus()));
}
static void test_faulty_on_last_home_failed() {
  UnitStatus s = healthyStatus();
  s.flags |= UNIT_FLAG_LAST_HOME_FAILED;
  TEST_ASSERT_TRUE(unitStatusIsFaulty(s));
}
static void test_faulty_on_hall_never() {
  UnitStatus s = healthyStatus();
  s.flags |= UNIT_FLAG_HALL_NEVER;
  TEST_ASSERT_TRUE(unitStatusIsFaulty(s));
}
static void test_faulty_on_brownout() {
  UnitStatus s = healthyStatus();
  s.lifetimeBrownoutCount = 1;
  TEST_ASSERT_TRUE(unitStatusIsFaulty(s));
}
static void test_faulty_on_watchdog() {
  UnitStatus s = healthyStatus();
  s.lifetimeWatchdogCount = 3;
  TEST_ASSERT_TRUE(unitStatusIsFaulty(s));
}
static void test_moving_flag_alone_is_not_faulty() {
  UnitStatus s = healthyStatus();
  s.flags |= UNIT_FLAG_MOVING;
  TEST_ASSERT_FALSE(unitStatusIsFaulty(s));
}
static void test_bad_command_count_is_not_faulty() {
  UnitStatus s = healthyStatus();
  s.badCommandCount = 200;
  TEST_ASSERT_FALSE(unitStatusIsFaulty(s));
}

// ---- computeFaultyUnitCount ----
static void test_count_ignores_invalid_units() {
  UnitStatus h[3];
  h[0] = healthyStatus();
  h[1] = healthyStatus(); h[1].lifetimeWatchdogCount = 2;   // faulty, but invalid
  h[2] = healthyStatus(); h[2].flags |= UNIT_FLAG_HALL_NEVER; // faulty, valid
  bool valid[3] = { true, false, true };
  TEST_ASSERT_EQUAL_INT(1, computeFaultyUnitCount(h, valid, 3));
}
static void test_count_zero_when_all_healthy() {
  UnitStatus h[2] = { healthyStatus(), healthyStatus() };
  bool valid[2] = { true, true };
  TEST_ASSERT_EQUAL_INT(0, computeFaultyUnitCount(h, valid, 2));
}

// ---- buildUnitHealthJson ----
static void test_json_headline_and_valid_unit_fields() {
  UnitStatus h[2];
  h[0] = healthyStatus();                               // uptime 1200, mcusr 0x01
  h[1] = healthyStatus(); h[1].lifetimeBrownoutCount = 4;
  bool valid[2] = { true, true };
  int states[2] = { 1, 1 };
  int fw[2] = { 0, 1 };
  char versions[2][9] = { "0fd341f", "abc1234" };
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), h, valid, states, fw, versions, 2, 1, 1);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  assert_contains(buf, "\"width\":2");
  assert_contains(buf, "\"faulty\":1");
  assert_contains(buf, "{\"i\":0,\"a\":1,\"st\":1,\"v\":1");
  assert_contains(buf, "\"up\":1200");
  assert_contains(buf, "\"mc\":1");
  assert_contains(buf, "\"hs\":720");
  assert_contains(buf, "{\"i\":1,\"a\":2,\"st\":1,\"v\":1");
  assert_contains(buf, "\"br\":4");
  // #140: detected unit rev string per valid slot.
  assert_contains(buf, "\"rev\":\"0fd341f\"");
  assert_contains(buf, "\"rev\":\"abc1234\"");
}
static void test_json_invalid_unit_omits_rev() {
  UnitStatus h[2] = { healthyStatus(), {} };
  bool valid[2] = { true, false };
  int states[2] = { 1, 2 };
  int fw[2] = { 0, 2 };
  // A stale rev may linger in the slot; an invalid unit must still emit no rev.
  char versions[2][9] = { "0fd341f", "deadbee" };
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), h, valid, states, fw, versions, 2, 0, 1);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  assert_contains(buf, "{\"i\":1,\"a\":2,\"st\":2,\"v\":0}");
  assert_not_contains(buf, "deadbee");
}
static void test_json_invalid_unit_omits_health_fields() {
  UnitStatus h[2] = { healthyStatus(), {} };
  bool valid[2] = { true, false };
  int states[2] = { 1, 2 };   // second slot in bootloader
  int fw[2] = { 0, 2 };
  char versions[2][9] = { "0fd341f", "" };
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), h, valid, states, fw, versions, 2, 0, 1);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  assert_contains(buf, "{\"i\":1,\"a\":2,\"st\":2,\"v\":0}");
  // The bootloader slot must NOT carry an uptime/fw field.
  assert_contains(buf, "\"v\":0}");
}
static void test_json_truncation_reports_would_be_length() {
  UnitStatus h[4];
  bool valid[4]; int states[4]; int fw[4];
  char versions[4][9];
  for (int i = 0; i < 4; i++) { h[i] = healthyStatus(); valid[i] = true; states[i] = 1; fw[i] = 0; strcpy(versions[i], "0fd341f"); }
  char small[40];
  size_t n = buildUnitHealthJson(small, sizeof(small), h, valid, states, fw, versions, 4, 0, 1);
  // Guard: signalled overflow via n >= cap, and stayed NUL-terminated inside
  // the buffer (snprintf never runs past `cap`).
  TEST_ASSERT_TRUE(n >= sizeof(small));
  TEST_ASSERT_TRUE(strlen(small) < sizeof(small));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_faulty_false_when_healthy);
  RUN_TEST(test_faulty_on_last_home_failed);
  RUN_TEST(test_faulty_on_hall_never);
  RUN_TEST(test_faulty_on_brownout);
  RUN_TEST(test_faulty_on_watchdog);
  RUN_TEST(test_moving_flag_alone_is_not_faulty);
  RUN_TEST(test_bad_command_count_is_not_faulty);
  RUN_TEST(test_count_ignores_invalid_units);
  RUN_TEST(test_count_zero_when_all_healthy);
  RUN_TEST(test_json_headline_and_valid_unit_fields);
  RUN_TEST(test_json_invalid_unit_omits_rev);
  RUN_TEST(test_json_invalid_unit_omits_health_fields);
  RUN_TEST(test_json_truncation_reports_would_be_length);
  return UNITY_END();
}
