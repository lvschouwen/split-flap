// Native tests for WallFind.h: which boards a search of the network lists.
#include <ArduinoFake.h>
#include <unity.h>

#include <string.h>

#include "WallFind.h"

void setUp() {}
void tearDown() {}

static WallFoundBoard row(const char* id, const char* address) {
  WallFoundBoard b;
  b.id = id;
  b.address = address;
  b.rev = "de38289";
  b.plat = "esp01";
  b.units = 5;
  return b;
}

static WallRowsTable wallWith(const char* id) {
  WallRowsTable table;
  table.count = 2;
  table.rows[0].width = 16;  // the master's own row
  strcpy(table.rows[1].id, id);
  strcpy(table.rows[1].host, "192.168.1.50");
  table.rows[1].row = 1;
  table.rows[1].width = 5;
  return table;
}

static void test_a_row_board_that_is_not_on_the_wall_is_listed() {
  WallFound found;
  TEST_ASSERT_TRUE(wallFoundAdd(found, row("split-flap-aaaaaa", "192.168.1.51"), wallWith("split-flap-261bb6")));
  char json[256];
  TEST_ASSERT_TRUE(wallFoundJson(found, json, sizeof(json)) > 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"boards\":[{\"id\":\"split-flap-aaaaaa\",\"address\":\"192.168.1.51\","
      "\"rev\":\"de38289\",\"units\":5}]}",
      json);
}

static void test_a_board_of_this_wall_is_left_out() {
  WallFound found;
  TEST_ASSERT_FALSE(wallFoundAdd(found, row("split-flap-261bb6", "192.168.1.50"), wallWith("split-flap-261bb6")));
  TEST_ASSERT_EQUAL(0, found.count);
}

static void test_a_board_that_is_no_row_board_is_left_out() {
  WallFound found;
  WallFoundBoard master = row("split-flap-c8a746", "192.168.1.60");
  master.plat = "";  // an S3 names no platform
  TEST_ASSERT_FALSE(wallFoundAdd(found, master, WallRowsTable()));
  master.plat = "esp32s3";
  TEST_ASSERT_FALSE(wallFoundAdd(found, master, WallRowsTable()));
}

static void test_a_board_without_a_name_or_a_lan_address_is_left_out() {
  WallFound found;
  TEST_ASSERT_FALSE(wallFoundAdd(found, row("", "192.168.1.51"), WallRowsTable()));
  TEST_ASSERT_FALSE(wallFoundAdd(found, row("split-flap-aaaaaa", ""), WallRowsTable()));
  TEST_ASSERT_FALSE(wallFoundAdd(found, row("split-flap-aaaaaa", "8.8.8.8"), WallRowsTable()));
  TEST_ASSERT_FALSE(wallFoundAdd(found, row("a-name-longer-than-a-row-id-may-be-x", "192.168.1.51"),
                                 WallRowsTable()));
  TEST_ASSERT_FALSE(wallFoundAdd(found, row("not\"a\\board name", "192.168.1.51"), WallRowsTable()));
}

// Anything on the network can answer: what it says is held to a board's shapes.
static void test_a_build_or_unit_count_no_board_would_say_is_left_out_of_the_line() {
  WallFound found;
  WallFoundBoard odd = row("split-flap-aaaaaa", "192.168.1.51");
  odd.rev = "\"\"\"\"\"\"\"\"";
  odd.units = 70000;
  TEST_ASSERT_TRUE(wallFoundAdd(found, odd, WallRowsTable()));
  WallFoundBoard lengthy = row("split-flap-bbbbbb", "192.168.1.52");
  lengthy.rev = "0123456789abcdef0";
  TEST_ASSERT_TRUE(wallFoundAdd(found, lengthy, WallRowsTable()));
  TEST_ASSERT_EQUAL_STRING("", found.boards[0].rev.c_str());
  TEST_ASSERT_EQUAL(0, found.boards[0].units);
  TEST_ASSERT_EQUAL_STRING("", found.boards[1].rev.c_str());
  TEST_ASSERT_TRUE(wallFoundRevPlain("de38289-dirty"));
  TEST_ASSERT_TRUE(wallFoundRevPlain("v2026.10.07"));
}

static void test_a_full_list_of_the_longest_answers_fits() {
  WallFound found;
  for (int i = 0; i < WALL_FIND_MAX; i++) {
    char id[WALL_ROW_ID_MAX + 1];
    char address[20];
    memset(id, 'a', sizeof(id));
    id[0] = (char)('a' + i);
    id[DEVICE_NAME_MAX_LEN < WALL_ROW_ID_MAX ? DEVICE_NAME_MAX_LEN : WALL_ROW_ID_MAX] = 0;
    snprintf(address, sizeof(address), "192.168.100.%d", 200 + i);
    WallFoundBoard board = row(id, address);
    board.rev = "0123456789abcdef";
    board.units = 255;
    TEST_ASSERT_TRUE(wallFoundAdd(found, board, WallRowsTable()));
  }
  char json[WALL_FIND_JSON_MAX];
  TEST_ASSERT_TRUE(wallFoundJson(found, json, sizeof(json)) > 0);
}

static void test_a_board_that_answers_twice_is_listed_once() {
  WallFound found;
  TEST_ASSERT_TRUE(wallFoundAdd(found, row("split-flap-aaaaaa", "192.168.1.51"), WallRowsTable()));
  TEST_ASSERT_FALSE(wallFoundAdd(found, row("split-flap-aaaaaa", "192.168.1.51"), WallRowsTable()));
  TEST_ASSERT_EQUAL(1, found.count);
}

static void test_no_more_boards_are_kept_than_a_wall_can_have() {
  WallFound found;
  for (int i = 0; i < WALL_FIND_MAX + 2; i++) {
    char id[24];
    char address[20];
    snprintf(id, sizeof(id), "split-flap-%06d", i);
    snprintf(address, sizeof(address), "192.168.1.%d", 60 + i);
    TEST_ASSERT_EQUAL(i < WALL_FIND_MAX, wallFoundAdd(found, row(id, address), WallRowsTable()));
  }
  TEST_ASSERT_EQUAL(WALL_FIND_MAX, found.count);
}

static void test_nothing_found_is_an_empty_list_and_a_short_buffer_is_refused() {
  WallFound found;
  char json[32];
  TEST_ASSERT_TRUE(wallFoundJson(found, json, sizeof(json)) > 0);
  TEST_ASSERT_EQUAL_STRING("{\"boards\":[]}", json);
  wallFoundAdd(found, row("split-flap-aaaaaa", "192.168.1.51"), WallRowsTable());
  TEST_ASSERT_EQUAL(0, wallFoundJson(found, json, sizeof(json)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_row_board_that_is_not_on_the_wall_is_listed);
  RUN_TEST(test_a_board_of_this_wall_is_left_out);
  RUN_TEST(test_a_board_that_is_no_row_board_is_left_out);
  RUN_TEST(test_a_board_without_a_name_or_a_lan_address_is_left_out);
  RUN_TEST(test_a_build_or_unit_count_no_board_would_say_is_left_out_of_the_line);
  RUN_TEST(test_a_full_list_of_the_longest_answers_fits);
  RUN_TEST(test_a_board_that_answers_twice_is_listed_once);
  RUN_TEST(test_no_more_boards_are_kept_than_a_wall_can_have);
  RUN_TEST(test_nothing_found_is_an_empty_list_and_a_short_buffer_is_refused);
  return UNITY_END();
}
