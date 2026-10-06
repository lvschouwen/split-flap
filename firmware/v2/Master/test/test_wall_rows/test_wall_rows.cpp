// Native tests for WallRows.h: the table of boards that make up this
// Split-Flap, as the master stores it.
#include <ArduinoFake.h>
#include <unity.h>

#include "../../WallRows.h"

void setUp() {}
void tearDown() {}

static WallRowsTable parsed(const char* stored) {
  WallRowsTable t;
  TEST_ASSERT_TRUE_MESSAGE(wallRowsFromString(stored, t), stored);
  return t;
}

static void test_an_empty_string_is_a_master_on_its_own() {
  WallRowsTable t = parsed("");
  TEST_ASSERT_EQUAL(0, t.count);
  TEST_ASSERT_EQUAL_STRING("", wallRowsToString(t).c_str());
}

static void test_a_table_survives_storing_and_reading_back() {
  const char* stored = "||0|0|16;split-flap-row1|192.168.1.50|1|0|5";
  WallRowsTable t = parsed(stored);
  TEST_ASSERT_EQUAL(2, t.count);
  TEST_ASSERT_TRUE(wallRowIsOwn(t.rows[0]));
  TEST_ASSERT_EQUAL(16, t.rows[0].width);
  TEST_ASSERT_FALSE(wallRowIsOwn(t.rows[1]));
  TEST_ASSERT_EQUAL_STRING("split-flap-row1", t.rows[1].id);
  TEST_ASSERT_EQUAL_STRING("192.168.1.50", t.rows[1].host);
  TEST_ASSERT_EQUAL(1, t.rows[1].row);
  TEST_ASSERT_EQUAL(5, t.rows[1].width);
  TEST_ASSERT_EQUAL_STRING(stored, wallRowsToString(t).c_str());
}

static void test_a_damaged_string_is_refused() {
  WallRowsTable t;
  TEST_ASSERT_FALSE(wallRowsFromString("row1|192.168.1.50|1|0", t));       // a field short
  TEST_ASSERT_FALSE(wallRowsFromString("row1|192.168.1.50|1|0|5|9", t));   // a field over
  TEST_ASSERT_FALSE(wallRowsFromString("row1|192.168.1.50|x|0|5", t));
  TEST_ASSERT_FALSE(wallRowsFromString("row1|192.168.1.50|1|0|300", t));
  TEST_ASSERT_FALSE(wallRowsFromString("row1|192.168.1.50|1|0|5;", t));    // an empty entry
  TEST_ASSERT_FALSE(wallRowsFromString(
      "a-name-that-is-longer-than-any-board-may-carry|192.168.1.50|1|0|5", t));
  TEST_ASSERT_FALSE(wallRowsFromString(
      "a|1.1.1.1|0|0|1;b|1.1.1.2|1|0|1;c|1.1.1.3|2|0|1;d|1.1.1.4|3|0|1;e|1.1.1.5|4|0|1;"
      "f|1.1.1.6|5|0|1;g|1.1.1.7|6|0|1;h|1.1.1.8|7|0|1;i|1.1.1.9|8|0|1", t));  // nine boards
}

static void test_a_row_is_found_by_its_id_and_the_own_row_never_is() {
  WallRowsTable t = parsed("||0|0|16;row-a|192.168.1.50|1|0|5;row-b|192.168.1.51|2|0|5");
  TEST_ASSERT_EQUAL(1, wallRowsFind(t, "row-a"));
  TEST_ASSERT_EQUAL(2, wallRowsFind(t, "row-b"));
  TEST_ASSERT_EQUAL(-1, wallRowsFind(t, "row-c"));
  TEST_ASSERT_EQUAL(-1, wallRowsFind(t, ""));
  TEST_ASSERT_EQUAL(0, wallRowsOwn(t));
  TEST_ASSERT_EQUAL(-1, wallRowsOwn(parsed("row-a|192.168.1.50|0|0|5")));
}

static const char* verdict(const char* stored) {
  WallRowsTable t = parsed(stored);
  ClusterGrid grid;
  return wallRowsValidate(t, grid).message;
}

static void test_a_sound_table_passes_and_gives_the_grid() {
  WallRowsTable t = parsed("||0|0|16;row-a|192.168.1.50|1|0|5");
  ClusterGrid grid;
  TEST_ASSERT_TRUE(wallRowsValidate(t, grid).ok);
  TEST_ASSERT_EQUAL(2, grid.rows);
  TEST_ASSERT_EQUAL(16, grid.rowWidth[0]);
  TEST_ASSERT_EQUAL(5, grid.rowWidth[1]);
}

static void test_boards_side_by_side_on_one_grid_row_are_allowed() {
  WallRowsTable t = parsed("||0|0|8;row-a|192.168.1.50|0|10|8");
  ClusterGrid grid;
  TEST_ASSERT_TRUE(wallRowsValidate(t, grid).ok);
  TEST_ASSERT_EQUAL(1, grid.rows);
  TEST_ASSERT_EQUAL(16, grid.rowWidth[0]);
  TEST_ASSERT_EQUAL(18, grid.rowExtent[0]);
}

static void test_what_makes_a_table_unsound() {
  TEST_ASSERT_EQUAL_STRING("Only one row can be the master's own",
                           verdict("||0|0|16;||1|0|16"));
  TEST_ASSERT_EQUAL_STRING("Two rows carry the same id",
                           verdict("row-a|192.168.1.50|0|0|5;row-a|192.168.1.51|1|0|5"));
  TEST_ASSERT_EQUAL_STRING("A row id is not a board name",
                           verdict("row a!|192.168.1.50|0|0|5"));
  TEST_ASSERT_EQUAL_STRING("A row's address is not on the local network",
                           verdict("row-a|8.8.8.8|0|0|5"));
  TEST_ASSERT_EQUAL_STRING("A row's address is not on the local network",
                           verdict("row-a||0|0|5"));
  // The address is compared with where a connection comes from: only one way
  // of writing it is taken, and never a name.
  for (const char* host : {"row-a.local", "localhost", "192.168.001.050", "192.168.1.50.", "192.168.1",
                           "192.168.1.256", " 192.168.1.50", "0xC0.168.1.50"}) {
    WallRowsTable t;
    strcpy(t.rows[0].id, "row-a");
    strcpy(t.rows[0].host, host);
    t.rows[0].width = 5;
    t.count = 1;
    ClusterGrid grid;
    TEST_ASSERT_EQUAL_STRING_MESSAGE("A row's address is not on the local network",
                                     wallRowsValidate(t, grid).message, host);
  }
  TEST_ASSERT_EQUAL_STRING("The master's own row has no address",
                           verdict("|192.168.1.50|0|0|5"));
  TEST_ASSERT_EQUAL_STRING("Members overlap",
                           verdict("||0|0|16;row-a|192.168.1.50|0|8|16"));
  TEST_ASSERT_EQUAL_STRING("Rows must be contiguous from 0",
                           verdict("||0|0|16;row-a|192.168.1.50|2|0|5"));
}

static void test_the_layout_table_keeps_order_and_marks_only_the_own_row_as_self() {
  WallRowsTable t = parsed("row-a|192.168.1.50|0|0|5;||1|2|16");
  ClusterMemberTable m = wallRowsLayout(t);
  TEST_ASSERT_EQUAL(2, m.count);
  TEST_ASSERT_TRUE(m.members[0].host[0] != 0);
  TEST_ASSERT_EQUAL(0, m.members[0].row);
  TEST_ASSERT_EQUAL(5, m.members[0].width);
  TEST_ASSERT_TRUE(m.members[1].host[0] == 0);
  TEST_ASSERT_EQUAL(2, m.members[1].col);
  TEST_ASSERT_EQUAL(16, m.members[1].width);
}

// ---- pairing a row into the table ------------------------------------------------

static WallRowPlace anywhere() { return WallRowPlace{}; }

static void test_the_first_row_paired_brings_the_masters_own_row_with_it() {
  WallRowsTable out;
  ClusterVerdict v = wallRowsWithPaired(parsed(""), "row-a", "192.168.1.50", 5, anywhere(), 16, out);
  TEST_ASSERT_TRUE_MESSAGE(v.ok, v.message);
  TEST_ASSERT_EQUAL_STRING("||0|0|16;row-a|192.168.1.50|1|0|5", wallRowsToString(out).c_str());
}

static void test_a_master_without_units_gets_no_row_of_its_own() {
  WallRowsTable out;
  TEST_ASSERT_TRUE(wallRowsWithPaired(parsed(""), "row-a", "192.168.1.50", 5, anywhere(), 0, out).ok);
  TEST_ASSERT_EQUAL_STRING("row-a|192.168.1.50|0|0|5", wallRowsToString(out).c_str());
}

static void test_the_next_row_goes_below_the_last() {
  WallRowsTable out;
  TEST_ASSERT_TRUE(wallRowsWithPaired(parsed("||0|0|16;row-a|192.168.1.50|1|0|5"), "row-b",
                                      "192.168.1.51", 8, anywhere(), 16, out).ok);
  TEST_ASSERT_EQUAL_STRING("||0|0|16;row-a|192.168.1.50|1|0|5;row-b|192.168.1.51|2|0|8",
                           wallRowsToString(out).c_str());
}

static void test_a_place_can_be_named() {
  WallRowsTable out;
  WallRowPlace place;
  place.row = 0;
  place.col = 18;
  place.width = 4;
  TEST_ASSERT_TRUE(wallRowsWithPaired(parsed("||0|0|16"), "row-a", "192.168.1.50", 5, place, 16, out).ok);
  TEST_ASSERT_EQUAL_STRING("||0|0|16;row-a|192.168.1.50|0|18|4", wallRowsToString(out).c_str());
}

// Pairing a row that is already in the table (its address moved, or the
// operator pairs it again) keeps its place and takes the new address.
static void test_pairing_a_known_row_again_keeps_its_place() {
  WallRowsTable out;
  TEST_ASSERT_TRUE(wallRowsWithPaired(parsed("||0|0|16;row-a|192.168.1.50|1|3|5"), "row-a",
                                      "192.168.1.77", 5, anywhere(), 16, out).ok);
  TEST_ASSERT_EQUAL_STRING("||0|0|16;row-a|192.168.1.77|1|3|5", wallRowsToString(out).c_str());
}

static void test_a_pairing_that_makes_the_table_unsound_is_refused_whole() {
  WallRowsTable out;
  WallRowPlace place;
  place.row = 0;
  place.col = 8;  // into the master's own 16
  TEST_ASSERT_EQUAL_STRING("Members overlap",
                           wallRowsWithPaired(parsed("||0|0|16"), "row-a", "192.168.1.50", 5, place,
                                              16, out).message);
  TEST_ASSERT_EQUAL_STRING("A row without units has no place on the wall",
                           wallRowsWithPaired(parsed("||0|0|16"), "row-a", "192.168.1.50", 0,
                                              anywhere(), 16, out).message);
  WallRowsTable full = parsed(
      "||0|0|1;b|10.0.0.2|1|0|1;c|10.0.0.3|2|0|1;d|10.0.0.4|3|0|1;e|10.0.0.5|4|0|1;"
      "f|10.0.0.6|5|0|1;g|10.0.0.7|6|0|1;h|10.0.0.8|7|0|1");
  TEST_ASSERT_EQUAL_STRING("A Split-Flap has at most 8 boards",
                           wallRowsWithPaired(full, "row-a", "192.168.1.50", 5, anywhere(), 1, out)
                               .message);
}

// ---- letting a row go --------------------------------------------------------------

static void test_a_released_row_leaves_and_the_last_one_leaves_a_master_on_its_own() {
  WallRowsTable out;
  TEST_ASSERT_TRUE(wallRowsWithout(parsed("||0|0|16;row-a|192.168.1.50|1|0|5;row-b|192.168.1.51|2|0|5"),
                                   "row-b", out).ok);
  TEST_ASSERT_EQUAL_STRING("||0|0|16;row-a|192.168.1.50|1|0|5", wallRowsToString(out).c_str());
  TEST_ASSERT_TRUE(wallRowsWithout(out, "row-a", out).ok);
  TEST_ASSERT_EQUAL_STRING("", wallRowsToString(out).c_str());
}

// A grid row left empty closes up: the rows below it move up one.
static void test_releasing_a_row_from_the_middle_moves_the_rows_below_it_up() {
  WallRowsTable out;
  TEST_ASSERT_TRUE(wallRowsWithout(
      parsed("||0|0|16;row-a|192.168.1.50|1|0|5;row-b|192.168.1.51|2|0|5"), "row-a", out).ok);
  TEST_ASSERT_EQUAL_STRING("||0|0|16;row-b|192.168.1.51|1|0|5", wallRowsToString(out).c_str());
  // A neighbour on the same grid row keeps that row in place.
  TEST_ASSERT_TRUE(wallRowsWithout(
      parsed("||0|0|8;row-a|192.168.1.50|0|10|8;row-b|192.168.1.51|1|0|5"), "row-a", out).ok);
  TEST_ASSERT_EQUAL_STRING("||0|0|8;row-b|192.168.1.51|1|0|5", wallRowsToString(out).c_str());
  TEST_ASSERT_EQUAL_STRING("No such row", wallRowsWithout(parsed("||0|0|16"), "row-x", out).message);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_an_empty_string_is_a_master_on_its_own);
  RUN_TEST(test_a_table_survives_storing_and_reading_back);
  RUN_TEST(test_a_damaged_string_is_refused);
  RUN_TEST(test_a_row_is_found_by_its_id_and_the_own_row_never_is);
  RUN_TEST(test_a_sound_table_passes_and_gives_the_grid);
  RUN_TEST(test_boards_side_by_side_on_one_grid_row_are_allowed);
  RUN_TEST(test_what_makes_a_table_unsound);
  RUN_TEST(test_the_layout_table_keeps_order_and_marks_only_the_own_row_as_self);
  RUN_TEST(test_the_first_row_paired_brings_the_masters_own_row_with_it);
  RUN_TEST(test_a_master_without_units_gets_no_row_of_its_own);
  RUN_TEST(test_the_next_row_goes_below_the_last);
  RUN_TEST(test_a_place_can_be_named);
  RUN_TEST(test_pairing_a_known_row_again_keeps_its_place);
  RUN_TEST(test_a_pairing_that_makes_the_table_unsound_is_refused_whole);
  RUN_TEST(test_a_released_row_leaves_and_the_last_one_leaves_a_master_on_its_own);
  RUN_TEST(test_releasing_a_row_from_the_middle_moves_the_rows_below_it_up);
  return UNITY_END();
}
