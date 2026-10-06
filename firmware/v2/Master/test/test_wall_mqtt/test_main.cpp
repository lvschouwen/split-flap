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
}

static bool contains(const String& s, const char* needle) {
  return strstr(s.c_str(), needle) != nullptr;
}

static void test_a_healthy_wall_is_no_problem() {
  WallMqttRow rows[2];
  makeHealthy(rows);
  TEST_ASSERT_FALSE(wallMqttProblem(rows, 2));
}

static void test_each_row_board_trouble_is_a_problem() {
  WallMqttRow rows[2];
  makeHealthy(rows);
  rows[1].lost = true;
  TEST_ASSERT_TRUE(wallMqttProblem(rows, 2));
  makeHealthy(rows);
  rows[1].rescue = true;
  TEST_ASSERT_TRUE(wallMqttProblem(rows, 2));
  makeHealthy(rows);
  rows[1].updateBlocked = true;
  TEST_ASSERT_TRUE(wallMqttProblem(rows, 2));
  makeHealthy(rows);
  rows[1].busDead = true;
  TEST_ASSERT_TRUE(wallMqttProblem(rows, 2));
  makeHealthy(rows);
  rows[1].unitsLost = 1;
  TEST_ASSERT_TRUE(wallMqttProblem(rows, 2));
}

static void test_faulty_units_and_a_busy_row_are_no_problem() {
  WallMqttRow rows[2];
  makeHealthy(rows);
  rows[1].unitsFaulty = 3;  // sticky lifetime counters: attributes only
  rows[1].reach = "busy";
  TEST_ASSERT_FALSE(wallMqttProblem(rows, 2));
}

static void test_lost_units_count_only_when_the_row_reported_them() {
  WallMqttRow rows[2];
  makeHealthy(rows);
  rows[1].unitsKnown = false;
  rows[1].unitsLost = 2;  // stale: no report since it was welcomed
  TEST_ASSERT_FALSE(wallMqttProblem(rows, 2));
}

static void test_the_own_row_never_trips_the_sensor() {
  // The master's own units have their own entities (units_faulty).
  WallMqttRow rows[2];
  makeHealthy(rows);
  rows[0].unitsLost = 4;
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
      "\"found\":16,\"faulty\":0,\"lost\":0},"
      "{\"id\":\"row-1\",\"own\":false,\"row\":1,\"col\":0,\"width\":5,\"reach\":\"up\","
      "\"rev\":\"abc1234\",\"rescue\":false,\"updateBlocked\":false,\"busDead\":false,"
      "\"found\":5,\"faulty\":0,\"lost\":0}],\"update\":\"idle\"}",
      json.c_str());
}

static void test_attrs_of_a_row_never_spoken_to_stop_at_its_reach() {
  WallMqttRow rows[2];
  makeHealthy(rows);
  rows[1].welcomed = false;
  rows[1].unitsKnown = false;
  rows[1].reach = "never";
  String json = wallMqttAttrsJson(rows, 2, "idle");
  TEST_ASSERT_TRUE(contains(json, "\"reach\":\"never\"}"));
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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_healthy_wall_is_no_problem);
  RUN_TEST(test_each_row_board_trouble_is_a_problem);
  RUN_TEST(test_faulty_units_and_a_busy_row_are_no_problem);
  RUN_TEST(test_lost_units_count_only_when_the_row_reported_them);
  RUN_TEST(test_the_own_row_never_trips_the_sensor);
  RUN_TEST(test_capacity_is_every_boards_units);
  RUN_TEST(test_attrs_carry_the_boards);
  RUN_TEST(test_attrs_of_a_row_never_spoken_to_stop_at_its_reach);
  RUN_TEST(test_attrs_escape_ids_and_revs);
  RUN_TEST(test_state_text_is_one_line_per_grid_row);
  RUN_TEST(test_boards_side_by_side_join_in_column_order);
  RUN_TEST(test_state_text_keeps_to_255_characters);
  RUN_TEST(test_discovery_names_the_new_topics);
  RUN_TEST(test_the_retired_entities_are_the_two_old_sensors);
  UNITY_END();
  return 0;
}
