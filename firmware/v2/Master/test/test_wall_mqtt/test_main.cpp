// Native tests for WallMqtt.h: what Home Assistant is told about a
// Split-Flap of several boards.
#include <ArduinoFake.h>
#include <unity.h>

#include <cstring>

#include "../../WallMqtt.h"

void setUp() {}
void tearDown() {}

// The master's 16 units on grid row 0, one healthy row board of 5 below.
static void makeHealthy(WallMqttRow rows[2]) {
  rows[0] = WallMqttRow{};
  rows[0].own = true;
  rows[0].width = 16;
  rows[0].text = "12:34";
  rows[0].unitsKnown = true;
  rows[0].unitsFound = 16;
  rows[0].judged = true;
  rows[0].level = "working";
  rows[0].reason = "working";
  rows[1] = WallMqttRow{};
  rows[1].id = "row-1";
  rows[1].row = 1;
  rows[1].width = 5;
  rows[1].reach = "up";
  rows[1].welcomed = true;
  rows[1].rev = "abc1234";
  rows[1].text = "06.10";
  rows[1].unitsKnown = true;
  rows[1].unitsFound = 5;
  rows[1].judged = true;
  rows[1].level = "note";
  rows[1].reason = "units-note";
}

static bool contains(const String& s, const char* needle) {
  return strstr(s.c_str(), needle) != nullptr;
}

static void test_a_healthy_wall_is_no_problem() {
  WallMqttRow rows[2];
  makeHealthy(rows);
  TEST_ASSERT_FALSE(wallMqttProblem(rows, 2));
}

static void test_a_board_at_fault_is_a_problem_whichever_board() {
  WallMqttRow rows[2];
  makeHealthy(rows);
  rows[1].fault = true;
  TEST_ASSERT_TRUE(wallMqttProblem(rows, 2));
  makeHealthy(rows);
  rows[0].fault = true;  // the master's own row
  TEST_ASSERT_TRUE(wallMqttProblem(rows, 2));
}

static void test_a_board_not_judged_yet_is_no_problem() {
  WallMqttRow rows[2];
  makeHealthy(rows);
  rows[1].judged = false;
  rows[1].fault = true;  // left over: nothing stands behind it
  TEST_ASSERT_FALSE(wallMqttProblem(rows, 2));
}

static void test_the_facts_alone_trip_nothing() {
  // What is wrong is the verdict's to say (BoardVerdict.h), not this sensor's.
  WallMqttRow rows[2];
  makeHealthy(rows);
  rows[1].lost = true;
  rows[1].rescue = true;
  rows[1].busDead = true;
  rows[1].unitsFaulty = 3;
  rows[1].unitsLost = 2;
  TEST_ASSERT_FALSE(wallMqttProblem(rows, 2));
}

static void test_capacity_is_every_boards_units() {
  WallMqttRow rows[2];
  makeHealthy(rows);
  TEST_ASSERT_EQUAL(21, wallMqttCapacity(rows, 2));
}

static void test_attrs_carry_the_boards() {
  WallMqttRow rows[2];
  makeHealthy(rows);
  String json = wallMqttAttrsJson(rows, 2, "idle");
  TEST_ASSERT_EQUAL_STRING(
      "{\"boards\":[{\"id\":\"\",\"own\":true,\"row\":0,\"col\":0,\"width\":16,"
      "\"level\":\"working\",\"reason\":\"working\","
      "\"found\":16,\"faulty\":0,\"lost\":0},"
      "{\"id\":\"row-1\",\"own\":false,\"row\":1,\"col\":0,\"width\":5,\"reach\":\"up\","
      "\"rev\":\"abc1234\",\"rescue\":false,\"updateBlocked\":false,\"busDead\":false,"
      "\"level\":\"note\",\"reason\":\"units-note\","
      "\"found\":5,\"faulty\":0,\"lost\":0}],\"update\":\"idle\"}",
      json.c_str());
}

static void test_attrs_of_a_row_never_spoken_to_stop_at_its_reach() {
  WallMqttRow rows[2];
  makeHealthy(rows);
  rows[1].welcomed = false;
  rows[1].unitsKnown = false;
  rows[1].reach = "never";
  rows[1].level = "fault";
  rows[1].reason = "never-seen";
  String json = wallMqttAttrsJson(rows, 2, "idle");
  TEST_ASSERT_TRUE(contains(
      json, "\"reach\":\"never\",\"level\":\"fault\",\"reason\":\"never-seen\"}"));
  TEST_ASSERT_FALSE(contains(json, "\"rev\""));
}

static void test_attrs_escape_ids_and_revs() {
  WallMqttRow rows[2];
  makeHealthy(rows);
  rows[1].id = "a\"b";
  rows[1].rev = "x\\y";
  String json = wallMqttAttrsJson(rows, 2, "idle");
  TEST_ASSERT_TRUE(contains(json, "\"id\":\"a\\\"b\""));
  TEST_ASSERT_TRUE(contains(json, "\"rev\":\"x\\\\y\""));
}

static void test_state_text_is_one_line_per_grid_row() {
  WallMqttRow rows[2];
  makeHealthy(rows);
  TEST_ASSERT_EQUAL_STRING("12:34\n06.10", wallMqttStateText(rows, 2).c_str());
}

static void test_boards_side_by_side_join_in_column_order() {
  WallMqttRow rows[3];
  makeHealthy(rows);
  rows[2] = WallMqttRow{};
  rows[2].id = "row-2";
  rows[2].row = 1;
  rows[2].col = 5;
  rows[2].width = 5;
  rows[2].text = "RIGHT";
  rows[1].text = "LEFT ";
  // Table order is not column order.
  WallMqttRow swapped[3] = {rows[2], rows[0], rows[1]};
  TEST_ASSERT_EQUAL_STRING("12:34\nLEFT RIGHT", wallMqttStateText(swapped, 3).c_str());
}

static void test_state_text_keeps_to_255_characters() {
  char big[301];
  memset(big, 'A', 300);
  big[300] = '\0';
  WallMqttRow rows[2];
  makeHealthy(rows);
  rows[1].text = big;
  TEST_ASSERT_EQUAL(255, (int)wallMqttStateText(rows, 2).length());
}

static void test_discovery_names_the_new_topics() {
  char topic[96];
  size_t n = buildWallProblemDiscoveryTopic(topic, sizeof(topic), "sf1");
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(topic));
  TEST_ASSERT_EQUAL_STRING("homeassistant/binary_sensor/sf1_wall_problem/config", topic);
  char payload[512];
  n = buildWallProblemDiscovery(payload, sizeof(payload), "sf1", "abc1234");
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(payload));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"stat_t\":\"splitflap/sf1/wall_problem\""));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"json_attr_t\":\"splitflap/sf1/wall/attrs\""));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"uniq_id\":\"sf1_wall_problem\""));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"sw\":\"abc1234\""));
}

static void test_the_retired_entities_are_the_two_old_sensors() {
  char topic[96];
  TEST_ASSERT_TRUE(buildRetiredDiscoveryTopic(topic, sizeof(topic), "sf1", 0) > 0);
  TEST_ASSERT_EQUAL_STRING("homeassistant/binary_sensor/sf1_cluster_degraded/config", topic);
  TEST_ASSERT_TRUE(buildRetiredDiscoveryTopic(topic, sizeof(topic), "sf1", 1) > 0);
  TEST_ASSERT_EQUAL_STRING("homeassistant/binary_sensor/sf1_leader_lost/config", topic);
  TEST_ASSERT_EQUAL(0, (int)buildRetiredDiscoveryTopic(topic, sizeof(topic), "sf1", 2));
}

// ---- #623: the wall's supply ----
static UnitFacts unitWithVcc(uint16_t mV) {
  UnitFacts u;
  u.state = 1;
  u.vitalsValid = true;
  u.vitals.vccMin_mV = mV;
  return u;
}

static void test_the_weakest_unit_and_the_count_under_the_floor_are_found_across_boards() {
  UnitFacts own[3] = {unitWithVcc(4300), unitWithVcc(3900), unitWithVcc(0)};
  UnitFacts theirs[2] = {unitWithVcc(3800), unitWithVcc(3950)};
  own[2].vitalsValid = false;
  WallMqttRow rows[2];
  makeHealthy(rows);
  wallMqttUnitAggregates(own, 3, 0x20, rows[0]);
  wallMqttUnitAggregates(theirs, 2, 0x20, rows[1]);
  TEST_ASSERT_EQUAL(3900, rows[0].vccMin);
  TEST_ASSERT_EQUAL(0x21, rows[0].vccMinUnit);
  TEST_ASSERT_EQUAL(1, rows[0].vccUnder);
  const WallMqttSupply s = wallMqttSupply(rows, 2);
  TEST_ASSERT_EQUAL(3800, s.mV);
  TEST_ASSERT_EQUAL(0x20, s.unit);
  TEST_ASSERT_EQUAL(1, s.board);
  TEST_ASSERT_EQUAL(3, s.under);
  // No supply known anywhere.
  WallMqttRow none[1];
  none[0] = WallMqttRow{};
  TEST_ASSERT_EQUAL(0, wallMqttSupply(none, 1).mV);
  TEST_ASSERT_EQUAL(-1, wallMqttSupply(none, 1).board);
}

// ---- #619: the wall's flips ----
static UnitFacts unitWithOdometer(uint32_t turns, bool valid) {
  UnitFacts u;
  u.state = 1;
  u.odometerValid = valid;
  u.odometer = turns;
  return u;
}

static void test_flips_wait_for_every_found_unit_and_never_drop_for_a_silence() {
  WallMqttFlips f;
  UnitFacts a[2] = {unitWithOdometer(100, true), unitWithOdometer(50, false)};
  // A found unit without a reading yet: not complete.
  TEST_ASSERT_FALSE(wallMqttFlipsNote(f, 0, a, 2));
  a[1].odometerValid = true;
  TEST_ASSERT_TRUE(wallMqttFlipsNote(f, 0, a, 2));
  TEST_ASSERT_EQUAL_UINT32(150, wallMqttFlipsTotal(f, 1));
  // The second unit goes silent: it keeps the count it last gave.
  a[1].state = 0;
  a[1].odometerValid = false;
  TEST_ASSERT_TRUE(wallMqttFlipsNote(f, 0, a, 2));
  a[0].odometer = 120;
  TEST_ASSERT_TRUE(wallMqttFlipsNote(f, 0, a, 2));
  TEST_ASSERT_EQUAL_UINT32(170, wallMqttFlipsTotal(f, 1));
  // A unit never read and not found counts nothing and blocks nothing.
  UnitFacts b[1] = {unitWithOdometer(0, false)};
  b[0].state = 0;
  TEST_ASSERT_TRUE(wallMqttFlipsNote(f, 1, b, 1));
  TEST_ASSERT_EQUAL_UINT32(170, wallMqttFlipsTotal(f, 2));
  // A reset odometer is a drop, as it should be.
  a[0].odometer = 0;
  wallMqttFlipsNote(f, 0, a, 2);
  TEST_ASSERT_EQUAL_UINT32(50, wallMqttFlipsTotal(f, 2));
  wallMqttFlipsReset(f);
  TEST_ASSERT_EQUAL_UINT32(0, wallMqttFlipsTotal(f, 2));
  TEST_ASSERT_FALSE(wallMqttFlipsNote(f, WALL_VERDICT_BOARDS, a, 2));
}

// ---- #621: one problem sensor a board ----
static void test_a_board_is_keyed_by_its_id_and_named_by_its_place() {
  WallMqttRow rows[2];
  makeHealthy(rows);
  rows[0].host = "split-flap-row1";
  rows[1].host = "split-flap-row2";
  rows[1].address = "192.168.1.50";
  rows[1].rssiKnown = true;
  rows[1].rssi = -61;
  TEST_ASSERT_EQUAL_STRING("own", wallMqttBoardKey(rows[0]));
  TEST_ASSERT_EQUAL_STRING("row-1", wallMqttBoardKey(rows[1]));
  TEST_ASSERT_FALSE(wallMqttBoardProblem(rows[1]));
  rows[1].fault = true;
  TEST_ASSERT_TRUE(wallMqttBoardProblem(rows[1]));
  char topic[128];
  buildBoardProblemDiscoveryTopic(topic, sizeof(topic), "flappy", wallMqttBoardKey(rows[1]));
  TEST_ASSERT_EQUAL_STRING("homeassistant/binary_sensor/flappy_board_row-1/config", topic);
  char payload[512];
  size_t n = buildBoardProblemDiscovery(payload, sizeof(payload), "flappy", "abc1234", "row-1",
                                        "split-flap-row2");
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(payload));
  TEST_ASSERT_TRUE(contains(payload, "\"name\":\"split-flap-row2\""));
  TEST_ASSERT_TRUE(contains(payload, "\"stat_t\":\"splitflap/flappy/board/row-1/problem\""));
  TEST_ASSERT_TRUE(contains(payload, "\"json_attr_t\":\"splitflap/flappy/board/row-1/attrs\""));
  TEST_ASSERT_TRUE(contains(payload, "\"uniq_id\":\"flappy_board_row-1\""));
  TEST_ASSERT_TRUE(contains(payload, "\"dev_cla\":\"problem\""));
  TEST_ASSERT_TRUE(contains(payload, "\"ids\":[\"flappy\"]"));
  const String attrs = wallMqttBoardAttrsJson(rows[1]);
  TEST_ASSERT_TRUE(contains(attrs, "\"id\":\"row-1\""));
  TEST_ASSERT_TRUE(contains(attrs, "\"host\":\"split-flap-row2\""));
  TEST_ASSERT_TRUE(contains(attrs, "\"address\":\"192.168.1.50\""));
  TEST_ASSERT_TRUE(contains(attrs, "\"rssi\":-61"));
  TEST_ASSERT_TRUE(contains(attrs, "\"rev\":\"abc1234\""));
  TEST_ASSERT_TRUE(contains(attrs, "\"reach\":\"up\""));
  TEST_ASSERT_TRUE(contains(attrs, "\"level\":\"note\",\"reason\":\"units-note\""));
  TEST_ASSERT_TRUE(contains(attrs, "\"found\":5"));
  // The own row: no reach, no address until given.
  const String own = wallMqttBoardAttrsJson(rows[0]);
  TEST_ASSERT_TRUE(contains(own, "\"own\":true"));
  TEST_ASSERT_FALSE(contains(own, "\"reach\""));
  TEST_ASSERT_FALSE(contains(own, "\"address\""));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_healthy_wall_is_no_problem);
  RUN_TEST(test_a_board_at_fault_is_a_problem_whichever_board);
  RUN_TEST(test_a_board_not_judged_yet_is_no_problem);
  RUN_TEST(test_the_facts_alone_trip_nothing);
  RUN_TEST(test_capacity_is_every_boards_units);
  RUN_TEST(test_attrs_carry_the_boards);
  RUN_TEST(test_attrs_of_a_row_never_spoken_to_stop_at_its_reach);
  RUN_TEST(test_attrs_escape_ids_and_revs);
  RUN_TEST(test_state_text_is_one_line_per_grid_row);
  RUN_TEST(test_boards_side_by_side_join_in_column_order);
  RUN_TEST(test_state_text_keeps_to_255_characters);
  RUN_TEST(test_discovery_names_the_new_topics);
  RUN_TEST(test_the_retired_entities_are_the_two_old_sensors);
  RUN_TEST(test_the_weakest_unit_and_the_count_under_the_floor_are_found_across_boards);
  RUN_TEST(test_flips_wait_for_every_found_unit_and_never_drop_for_a_silence);
  RUN_TEST(test_a_board_is_keyed_by_its_id_and_named_by_its_place);
  UNITY_END();
  return 0;
}
