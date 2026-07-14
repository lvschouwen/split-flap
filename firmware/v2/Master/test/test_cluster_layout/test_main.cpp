// Host-side tests for the cluster grid layout engine (#271) — the pure
// core the leader runs before any fan-out: member-table validation +
// derived grid, word-wrap/alignment/slicing into pre-positioned segments,
// and the cluster clock frame. Followers render segments verbatim, so
// every expected string here is exact, including its padding.

#include <unity.h>

#include "../../ClusterLayout.h"

void setUp() {}
void tearDown() {}

#define BLANK16 "                "

// N rows of one `width`-wide member each ({row i, col 0}).
static ClusterMemberTable makeRows(int rows, int width) {
  ClusterMemberTable t;
  t.count = (uint8_t)rows;
  for (int i = 0; i < rows; i++) {
    snprintf(t.members[i].host, sizeof(t.members[i].host), "row%d.local", i);
    t.members[i].row = (uint8_t)i;
    t.members[i].col = 0;
    t.members[i].width = (uint8_t)width;
  }
  return t;
}

// 2 rows x 32 units: two 16-wide members side by side per row.
static ClusterMemberTable makeWide2x32() {
  ClusterMemberTable t;
  t.count = 4;
  const uint8_t rows[4] = {0, 0, 1, 1};
  const uint8_t cols[4] = {0, 16, 0, 16};
  for (int i = 0; i < 4; i++) {
    snprintf(t.members[i].host, sizeof(t.members[i].host), "m%d.local", i);
    t.members[i].row = rows[i];
    t.members[i].col = cols[i];
    t.members[i].width = 16;
  }
  return t;
}

// Two members at identical {row 0, col 0, width 16} — a mirror wall.
static ClusterMemberTable makeMirror() {
  ClusterMemberTable t = makeRows(1, 16);
  t.count = 2;
  snprintf(t.members[1].host, sizeof(t.members[1].host), "twin.local");
  t.members[1].row = 0;
  t.members[1].col = 0;
  t.members[1].width = 16;
  return t;
}

// --- validation + derived grid --------------------------------------------------

static void test_valid_2x16_derives_grid() {
  ClusterGrid grid;
  ClusterVerdict v = validateMemberTable(makeRows(2, 16), grid);
  TEST_ASSERT_TRUE(v.ok);
  TEST_ASSERT_EQUAL(2, grid.rows);
  TEST_ASSERT_EQUAL(16, grid.rowWidth[0]);
  TEST_ASSERT_EQUAL(16, grid.rowWidth[1]);
}

static void test_valid_6x16_derives_grid() {
  ClusterGrid grid;
  TEST_ASSERT_TRUE(validateMemberTable(makeRows(6, 16), grid).ok);
  TEST_ASSERT_EQUAL(6, grid.rows);
  for (int r = 0; r < 6; r++) TEST_ASSERT_EQUAL(16, grid.rowWidth[r]);
}

static void test_wide_rows_compose_from_side_by_side_members() {
  ClusterGrid grid;
  TEST_ASSERT_TRUE(validateMemberTable(makeWide2x32(), grid).ok);
  TEST_ASSERT_EQUAL(2, grid.rows);
  TEST_ASSERT_EQUAL(32, grid.rowWidth[0]);
  TEST_ASSERT_EQUAL(32, grid.rowWidth[1]);
}

static void test_empty_table_rejected() {
  ClusterGrid grid;
  ClusterMemberTable t;
  TEST_ASSERT_FALSE(validateMemberTable(t, grid).ok);
}

static void test_count_over_max_rejected() {
  ClusterGrid grid;
  ClusterMemberTable t = makeRows(8, 16);
  t.count = CLUSTER_MAX_MEMBERS + 1;
  TEST_ASSERT_FALSE(validateMemberTable(t, grid).ok);
}

static void test_zero_width_member_rejected() {
  ClusterGrid grid;
  ClusterMemberTable t = makeRows(2, 16);
  t.members[1].width = 0;
  TEST_ASSERT_FALSE(validateMemberTable(t, grid).ok);
}

static void test_missing_row_rejected() {
  ClusterGrid grid;
  ClusterMemberTable t = makeRows(2, 16);
  t.members[1].row = 2;  // rows {0, 2}: row 1 missing
  TEST_ASSERT_FALSE(validateMemberTable(t, grid).ok);
}

static void test_table_without_row_zero_rejected() {
  ClusterGrid grid;
  ClusterMemberTable t = makeRows(1, 16);
  t.members[0].row = 1;
  TEST_ASSERT_FALSE(validateMemberTable(t, grid).ok);
}

static void test_row_index_at_member_cap_rejected() {
  ClusterGrid grid;
  ClusterMemberTable t = makeRows(1, 16);
  t.members[0].row = CLUSTER_MAX_MEMBERS;
  TEST_ASSERT_FALSE(validateMemberTable(t, grid).ok);
}

static void test_row_not_starting_at_col_zero_rejected() {
  ClusterGrid grid;
  ClusterMemberTable t = makeRows(1, 12);
  t.members[0].col = 4;
  TEST_ASSERT_FALSE(validateMemberTable(t, grid).ok);
}

static void test_gap_between_members_rejected() {
  ClusterGrid grid;
  ClusterMemberTable t = makeWide2x32();
  t.members[1].col = 20;  // row 0: [0,16) + [20,36) leaves a hole
  TEST_ASSERT_FALSE(validateMemberTable(t, grid).ok);
}

static void test_partial_overlap_rejected() {
  ClusterGrid grid;
  ClusterMemberTable t = makeWide2x32();
  t.members[1].col = 8;  // row 0: [0,16) + [8,24) overlap
  TEST_ASSERT_FALSE(validateMemberTable(t, grid).ok);
}

static void test_same_col_different_width_rejected() {
  ClusterGrid grid;
  ClusterMemberTable t = makeMirror();
  t.members[1].width = 8;  // coincident start, different span: not a mirror
  TEST_ASSERT_FALSE(validateMemberTable(t, grid).ok);
}

static void test_mirror_coincident_members_accepted() {
  ClusterGrid grid;
  TEST_ASSERT_TRUE(validateMemberTable(makeMirror(), grid).ok);
  TEST_ASSERT_EQUAL(1, grid.rows);
  // Tiled extent, not sum: the twin must not double the row width.
  TEST_ASSERT_EQUAL(16, grid.rowWidth[0]);
}

// --- layoutGridText: wrap + alignment + slicing ----------------------------------

static void test_short_text_left_fills_row_zero() {
  String seg[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_TRUE(
      layoutGridText("HELLO WORLD", DisplayAlignment::Left, makeRows(2, 16), seg));
  TEST_ASSERT_EQUAL_STRING("HELLO WORLD     ", seg[0].c_str());
  TEST_ASSERT_EQUAL_STRING(BLANK16, seg[1].c_str());
}

static void test_wrap_breaks_on_spaces() {
  String seg[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_TRUE(layoutGridText("SPLIT FLAP DISPLAY CLUSTER",
                                  DisplayAlignment::Left, makeRows(2, 16), seg));
  TEST_ASSERT_EQUAL_STRING("SPLIT FLAP      ", seg[0].c_str());
  TEST_ASSERT_EQUAL_STRING("DISPLAY CLUSTER ", seg[1].c_str());
}

static void test_center_alignment_per_row() {
  String seg[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_TRUE(
      layoutGridText("HI THERE", DisplayAlignment::Center, makeRows(2, 16), seg));
  TEST_ASSERT_EQUAL_STRING("    HI THERE    ", seg[0].c_str());
  TEST_ASSERT_EQUAL_STRING(BLANK16, seg[1].c_str());
}

static void test_right_alignment_per_row() {
  String seg[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_TRUE(
      layoutGridText("HI", DisplayAlignment::Right, makeRows(2, 16), seg));
  TEST_ASSERT_EQUAL_STRING("              HI", seg[0].c_str());
  TEST_ASSERT_EQUAL_STRING(BLANK16, seg[1].c_str());
}

static void test_overlong_word_hard_splits_from_row_start() {
  String seg[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_TRUE(layoutGridText("ABCDEFGHIJKLMNOPQRST", DisplayAlignment::Left,
                                  makeRows(2, 16), seg));
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNOP", seg[0].c_str());
  TEST_ASSERT_EQUAL_STRING("QRST            ", seg[1].c_str());
}

static void test_overlong_word_hard_splits_mid_row() {
  // A word wider than any fresh row fills the current row's remainder
  // instead of wasting it.
  String seg[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_TRUE(layoutGridText("HI ABCDEFGHIJKLMNOPQRST",
                                  DisplayAlignment::Left, makeRows(2, 16), seg));
  TEST_ASSERT_EQUAL_STRING("HI ABCDEFGHIJKLM", seg[0].c_str());
  TEST_ASSERT_EQUAL_STRING("NOPQRST         ", seg[1].c_str());
}

static void test_words_continue_after_hard_split() {
  String seg[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_TRUE(layoutGridText("ABCDEFGHIJKLMNOPQRST OK",
                                  DisplayAlignment::Left, makeRows(2, 16), seg));
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNOP", seg[0].c_str());
  TEST_ASSERT_EQUAL_STRING("QRST OK         ", seg[1].c_str());
}

static void test_overflowing_text_truncates() {
  String seg[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_TRUE(layoutGridText("AAAA BBBB CCCC DDDD EEEE FFFF GGGG",
                                  DisplayAlignment::Left, makeRows(2, 16), seg));
  TEST_ASSERT_EQUAL_STRING("AAAA BBBB CCCC  ", seg[0].c_str());
  TEST_ASSERT_EQUAL_STRING("DDDD EEEE FFFF  ", seg[1].c_str());
}

static void test_space_runs_collapse_at_wrap() {
  String seg[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_TRUE(
      layoutGridText("A  B", DisplayAlignment::Left, makeRows(2, 16), seg));
  TEST_ASSERT_EQUAL_STRING("A B             ", seg[0].c_str());
}

static void test_empty_text_blanks_all_segments() {
  String seg[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_TRUE(
      layoutGridText("", DisplayAlignment::Left, makeRows(2, 16), seg));
  TEST_ASSERT_EQUAL_STRING(BLANK16, seg[0].c_str());
  TEST_ASSERT_EQUAL_STRING(BLANK16, seg[1].c_str());
}

static void test_wide_row_slices_across_side_by_side_members() {
  String seg[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_TRUE(layoutGridText("THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG",
                                  DisplayAlignment::Left, makeWide2x32(), seg));
  TEST_ASSERT_EQUAL_STRING("THE QUICK BROWN ", seg[0].c_str());
  TEST_ASSERT_EQUAL_STRING("FOX JUMPS OVER  ", seg[1].c_str());
  TEST_ASSERT_EQUAL_STRING("THE LAZY DOG    ", seg[2].c_str());
  TEST_ASSERT_EQUAL_STRING(BLANK16, seg[3].c_str());
}

static void test_mirror_members_get_identical_segments() {
  String seg[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_TRUE(
      layoutGridText("MIRROR", DisplayAlignment::Left, makeMirror(), seg));
  TEST_ASSERT_EQUAL_STRING("MIRROR          ", seg[0].c_str());
  TEST_ASSERT_EQUAL_STRING("MIRROR          ", seg[1].c_str());
}

static void test_charset_passes_through_untouched() {
  // Wire-encoded umlauts ($ & #), digits, punctuation and CASE all pass
  // through — flapFrameBuild owns uppercasing and index mapping.
  String seg[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_TRUE(layoutGridText("$&# hello 12:34", DisplayAlignment::Left,
                                  makeRows(1, 16), seg));
  TEST_ASSERT_EQUAL_STRING("$&# hello 12:34 ", seg[0].c_str());
}

static void test_layout_refuses_invalid_table() {
  String seg[CLUSTER_MAX_MEMBERS];
  ClusterMemberTable t = makeRows(2, 16);
  t.members[1].row = 2;
  TEST_ASSERT_FALSE(layoutGridText("HI", DisplayAlignment::Left, t, seg));
}

// --- clusterClockSegments ---------------------------------------------------------

static void test_clock_time_row_zero_date_row_one() {
  String seg[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_TRUE(clusterClockSegments("12:34", "13 Jul 26",
                                        DisplayAlignment::Center,
                                        makeRows(2, 16), seg));
  TEST_ASSERT_EQUAL_STRING("     12:34      ", seg[0].c_str());
  TEST_ASSERT_EQUAL_STRING("   13 Jul 26    ", seg[1].c_str());
}

static void test_clock_rows_beyond_date_stay_blank() {
  String seg[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_TRUE(clusterClockSegments("12:34", "13 Jul 26",
                                        DisplayAlignment::Center,
                                        makeRows(6, 16), seg));
  for (int m = 2; m < 6; m++) TEST_ASSERT_EQUAL_STRING(BLANK16, seg[m].c_str());
}

static void test_clock_single_row_grid_drops_date() {
  String seg[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_TRUE(clusterClockSegments("12:34", "13 Jul 26",
                                        DisplayAlignment::Center,
                                        makeRows(1, 16), seg));
  TEST_ASSERT_EQUAL_STRING("     12:34      ", seg[0].c_str());
}

static void test_clock_wide_row_slices_time_across_members() {
  String seg[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_TRUE(clusterClockSegments("12:34", "13 Jul 26",
                                        DisplayAlignment::Center,
                                        makeWide2x32(), seg));
  TEST_ASSERT_EQUAL_STRING("             12:", seg[0].c_str());
  TEST_ASSERT_EQUAL_STRING("34              ", seg[1].c_str());
}

static void test_clock_refuses_invalid_table() {
  String seg[CLUSTER_MAX_MEMBERS];
  ClusterMemberTable t;
  TEST_ASSERT_FALSE(clusterClockSegments("12:34", "13 Jul 26",
                                         DisplayAlignment::Center, t, seg));
}

static void test_cluster_date_format_is_v1_stamp_date() {
  TEST_ASSERT_EQUAL_STRING("%d %b %y", CLUSTER_DATE_FORMAT);
}

// --- wall mirror reconstruction (#277) -------------------------------------------

// Table with the leader's own units at {row 0, col 0} and one follower row.
static ClusterMemberTable makeSelfPlusFollower() {
  ClusterMemberTable t = makeRows(2, 16);
  t.members[0].host[0] = '\0';  // empty host = this board
  return t;
}

static void test_mirror_rows_rebuilds_wide_rows_from_segments() {
  ClusterMemberTable t = makeWide2x32();
  String seg[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_TRUE(layoutGridText("ABCDEFGHIJKLMNOPQRSTUVWXYZ 123456",
                                  DisplayAlignment::Left, t, seg));
  String rows[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_EQUAL(2, clusterMirrorRows(t, seg, "", DisplayAlignment::Left,
                                         rows));
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNOPQRSTUVWXYZ      ", rows[0].c_str());
  TEST_ASSERT_EQUAL_STRING("123456                          ", rows[1].c_str());
}

static void test_mirror_rows_overlays_self_slot_with_alignment() {
  ClusterMemberTable t = makeSelfPlusFollower();
  String seg[CLUSTER_MAX_MEMBERS];
  seg[1] = "FOLLOWER ROW    ";
  String rows[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_EQUAL(2, clusterMirrorRows(t, seg, "HI",
                                         DisplayAlignment::Center, rows));
  TEST_ASSERT_EQUAL_STRING("       HI       ", rows[0].c_str());
  TEST_ASSERT_EQUAL_STRING("FOLLOWER ROW    ", rows[1].c_str());
}

static void test_mirror_rows_self_overlay_beats_coincident_twin() {
  // Mirror wall where the leader IS one of the twins: the live self text
  // (a transient may own it) must win over the twin's stale segment.
  ClusterMemberTable t = makeMirror();
  t.members[1].host[0] = '\0';  // twin listed AFTER the remote member
  String seg[CLUSTER_MAX_MEMBERS];
  seg[0] = "SEGMENT TEXT    ";
  String rows[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_EQUAL(1, clusterMirrorRows(t, seg, "LIVE",
                                         DisplayAlignment::Left, rows));
  TEST_ASSERT_EQUAL_STRING("LIVE            ", rows[0].c_str());
}

static void test_mirror_rows_blank_segments_render_blanks() {
  ClusterMemberTable t = makeRows(2, 16);
  String seg[CLUSTER_MAX_MEMBERS];
  String rows[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_EQUAL(2, clusterMirrorRows(t, seg, "", DisplayAlignment::Left,
                                         rows));
  TEST_ASSERT_EQUAL_STRING(BLANK16, rows[0].c_str());
  TEST_ASSERT_EQUAL_STRING(BLANK16, rows[1].c_str());
}

static void test_mirror_rows_overlong_self_text_truncates_to_slot() {
  ClusterMemberTable t = makeSelfPlusFollower();
  String seg[CLUSTER_MAX_MEMBERS];
  String rows[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_EQUAL(2, clusterMirrorRows(t, seg, "ABCDEFGHIJKLMNOPQRSTUV",
                                         DisplayAlignment::Left, rows));
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNOP", rows[0].c_str());
}

static void test_mirror_rows_refuses_invalid_table() {
  ClusterMemberTable t;
  String seg[CLUSTER_MAX_MEMBERS];
  String rows[CLUSTER_MAX_MEMBERS];
  TEST_ASSERT_EQUAL(0, clusterMirrorRows(t, seg, "", DisplayAlignment::Left,
                                         rows));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_valid_2x16_derives_grid);
  RUN_TEST(test_valid_6x16_derives_grid);
  RUN_TEST(test_wide_rows_compose_from_side_by_side_members);
  RUN_TEST(test_empty_table_rejected);
  RUN_TEST(test_count_over_max_rejected);
  RUN_TEST(test_zero_width_member_rejected);
  RUN_TEST(test_missing_row_rejected);
  RUN_TEST(test_table_without_row_zero_rejected);
  RUN_TEST(test_row_index_at_member_cap_rejected);
  RUN_TEST(test_row_not_starting_at_col_zero_rejected);
  RUN_TEST(test_gap_between_members_rejected);
  RUN_TEST(test_partial_overlap_rejected);
  RUN_TEST(test_same_col_different_width_rejected);
  RUN_TEST(test_mirror_coincident_members_accepted);
  RUN_TEST(test_short_text_left_fills_row_zero);
  RUN_TEST(test_wrap_breaks_on_spaces);
  RUN_TEST(test_center_alignment_per_row);
  RUN_TEST(test_right_alignment_per_row);
  RUN_TEST(test_overlong_word_hard_splits_from_row_start);
  RUN_TEST(test_overlong_word_hard_splits_mid_row);
  RUN_TEST(test_words_continue_after_hard_split);
  RUN_TEST(test_overflowing_text_truncates);
  RUN_TEST(test_space_runs_collapse_at_wrap);
  RUN_TEST(test_empty_text_blanks_all_segments);
  RUN_TEST(test_wide_row_slices_across_side_by_side_members);
  RUN_TEST(test_mirror_members_get_identical_segments);
  RUN_TEST(test_charset_passes_through_untouched);
  RUN_TEST(test_layout_refuses_invalid_table);
  RUN_TEST(test_clock_time_row_zero_date_row_one);
  RUN_TEST(test_clock_rows_beyond_date_stay_blank);
  RUN_TEST(test_clock_single_row_grid_drops_date);
  RUN_TEST(test_clock_wide_row_slices_time_across_members);
  RUN_TEST(test_clock_refuses_invalid_table);
  RUN_TEST(test_cluster_date_format_is_v1_stamp_date);
  RUN_TEST(test_mirror_rows_rebuilds_wide_rows_from_segments);
  RUN_TEST(test_mirror_rows_overlays_self_slot_with_alignment);
  RUN_TEST(test_mirror_rows_self_overlay_beats_coincident_twin);
  RUN_TEST(test_mirror_rows_blank_segments_render_blanks);
  RUN_TEST(test_mirror_rows_overlong_self_text_truncates_to_slot);
  RUN_TEST(test_mirror_rows_refuses_invalid_table);
  return UNITY_END();
}
