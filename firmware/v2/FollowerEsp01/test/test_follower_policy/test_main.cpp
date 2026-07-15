// Host-side tests for the ESP-01 follower phase machine (#298) — the
// trimmed ClusterFollowerPolicy copy: Standalone → Clustered → Grace →
// Blank (a stale frozen row looks broken; the leader's health strip
// already shows it as lost), epoch/seq armor, sticky-leadership join
// conflict, and the commitAt flip-sync delay math.

#include <unity.h>

#include "../../FollowerPolicy.h"

void setUp() {}
void tearDown() {}

// --- boot ------------------------------------------------------------------------

static void test_boot_without_membership_is_standalone() {
  FollowerClusterState st;
  followerClusterBoot(st, 1000, false);
  TEST_ASSERT_EQUAL(FollowerPhase::Standalone, st.phase);
}

static void test_boot_with_membership_holds_in_grace() {
  FollowerClusterState st;
  followerClusterBoot(st, 1000, true);
  TEST_ASSERT_EQUAL(FollowerPhase::Grace, st.phase);
  TEST_ASSERT_EQUAL_UINT32(1000, st.lastContactMs);
}

// --- join / contact ---------------------------------------------------------------

static void test_join_enters_clustered_and_resets_seq_on_new_epoch() {
  FollowerClusterState st;
  followerClusterBoot(st, 0, false);
  followerClusterJoin(st, 1000, 7);
  TEST_ASSERT_EQUAL(FollowerPhase::Clustered, st.phase);
  followerClusterAcceptRender(st, 1100, 7, 50);
  followerClusterJoin(st, 2000, 7);  // same-epoch rejoin keeps seq tracking
  TEST_ASSERT_EQUAL(FollowerRenderVerdict::Duplicate,
                    followerClusterAcceptRender(st, 2100, 7, 50));
  followerClusterJoin(st, 3000, 9);  // leader rebooted: fresh seq space
  TEST_ASSERT_EQUAL(FollowerRenderVerdict::Apply,
                    followerClusterAcceptRender(st, 3100, 9, 1));
}

static void test_contact_reclaims_from_grace_and_blank() {
  FollowerClusterState st;
  followerClusterBoot(st, 0, true);  // Grace
  TEST_ASSERT_TRUE(followerClusterContact(st, 5000));
  TEST_ASSERT_EQUAL(FollowerPhase::Clustered, st.phase);
  st.phase = FollowerPhase::Blank;
  TEST_ASSERT_TRUE(followerClusterContact(st, 9000));
  TEST_ASSERT_EQUAL(FollowerPhase::Clustered, st.phase);
}

static void test_contact_refused_in_standalone() {
  FollowerClusterState st;
  followerClusterBoot(st, 0, false);
  TEST_ASSERT_FALSE(followerClusterContact(st, 1000));
  TEST_ASSERT_EQUAL(FollowerPhase::Standalone, st.phase);
}

// --- render acceptance -------------------------------------------------------------

static void test_render_before_join_is_not_clustered() {
  FollowerClusterState st;
  followerClusterBoot(st, 0, false);
  TEST_ASSERT_EQUAL(FollowerRenderVerdict::NotClustered,
                    followerClusterAcceptRender(st, 1000, 7, 1));
}

static void test_stale_seq_is_duplicate() {
  FollowerClusterState st;
  followerClusterBoot(st, 0, false);
  followerClusterJoin(st, 1000, 7);
  TEST_ASSERT_EQUAL(FollowerRenderVerdict::Apply,
                    followerClusterAcceptRender(st, 1100, 7, 5));
  TEST_ASSERT_EQUAL(FollowerRenderVerdict::Duplicate,
                    followerClusterAcceptRender(st, 1200, 7, 5));
  TEST_ASSERT_EQUAL(FollowerRenderVerdict::Apply,
                    followerClusterAcceptRender(st, 1300, 7, 6));
}

// --- silence decay: Clustered -> Grace -> Blank -------------------------------------

static void test_silence_parks_clustered_into_grace_then_blank() {
  FollowerClusterState st;
  followerClusterBoot(st, 0, false);
  followerClusterJoin(st, 1000, 7);
  TEST_ASSERT_FALSE(followerClusterTick(st, 1000 + FOLLOWER_CONTACT_FRESH_MS - 1));
  TEST_ASSERT_TRUE(followerClusterTick(st, 1000 + FOLLOWER_CONTACT_FRESH_MS));
  TEST_ASSERT_EQUAL(FollowerPhase::Grace, st.phase);
  TEST_ASSERT_FALSE(followerClusterTick(st, 1000 + FOLLOWER_GRACE_MS - 1));
  TEST_ASSERT_TRUE(followerClusterTick(st, 1000 + FOLLOWER_GRACE_MS));
  TEST_ASSERT_EQUAL(FollowerPhase::Blank, st.phase);
}

static void test_starved_tick_cascades_straight_to_blank() {
  // The phase tracks total silence, not tick cadence.
  FollowerClusterState st;
  followerClusterBoot(st, 0, false);
  followerClusterJoin(st, 1000, 7);
  TEST_ASSERT_TRUE(followerClusterTick(st, 1000 + FOLLOWER_GRACE_MS + 5000));
  TEST_ASSERT_EQUAL(FollowerPhase::Blank, st.phase);
}

// --- leave --------------------------------------------------------------------------

static void test_leave_returns_to_standalone() {
  FollowerClusterState st;
  followerClusterBoot(st, 0, false);
  followerClusterJoin(st, 1000, 7);
  followerClusterLeave(st);
  TEST_ASSERT_EQUAL(FollowerPhase::Standalone, st.phase);
  TEST_ASSERT_EQUAL(FollowerRenderVerdict::NotClustered,
                    followerClusterAcceptRender(st, 2000, 7, 99));
}

// --- sticky leadership (#295 semantics kept) ----------------------------------------

static void test_foreign_join_conflicts_only_while_leader_fresh() {
  FollowerClusterState st;
  followerClusterBoot(st, 0, false);
  followerClusterJoin(st, 1000, 7);
  TEST_ASSERT_TRUE(followerClusterJoinConflicts(st, 2000, false));
  TEST_ASSERT_FALSE(followerClusterJoinConflicts(st, 2000, true));
  // Silence past the fresh window: a successor may claim this row.
  TEST_ASSERT_FALSE(
      followerClusterJoinConflicts(st, 1000 + FOLLOWER_CONTACT_FRESH_MS, false));
}

// --- phase names + blank rule --------------------------------------------------------

static void test_phase_names() {
  TEST_ASSERT_EQUAL_STRING("standalone",
                           followerPhaseName(FollowerPhase::Standalone));
  TEST_ASSERT_EQUAL_STRING("clustered",
                           followerPhaseName(FollowerPhase::Clustered));
  TEST_ASSERT_EQUAL_STRING("grace", followerPhaseName(FollowerPhase::Grace));
  TEST_ASSERT_EQUAL_STRING("blank", followerPhaseName(FollowerPhase::Blank));
}

static void test_blank_and_standalone_phases_blank_the_row() {
  // Grace HOLDS the last text; Blank and Standalone show nothing — the row
  // is meaningless without its leader.
  TEST_ASSERT_TRUE(followerPhaseShowsBlank(FollowerPhase::Standalone));
  TEST_ASSERT_FALSE(followerPhaseShowsBlank(FollowerPhase::Clustered));
  TEST_ASSERT_FALSE(followerPhaseShowsBlank(FollowerPhase::Grace));
  TEST_ASSERT_TRUE(followerPhaseShowsBlank(FollowerPhase::Blank));
}

// --- commitAt flip sync ---------------------------------------------------------------

static void test_render_delay_math() {
  // Unsynced or past commitAt renders immediately; far-future clamps.
  TEST_ASSERT_EQUAL_UINT32(0, followerRenderDelayMs(2000, 1000, false));
  TEST_ASSERT_EQUAL_UINT32(0, followerRenderDelayMs(1000, 2000, true));
  TEST_ASSERT_EQUAL_UINT32(400, followerRenderDelayMs(2400, 2000, true));
  TEST_ASSERT_EQUAL_UINT32(
      FOLLOWER_COMMIT_MAX_DELAY_MS,
      followerRenderDelayMs(2000 + FOLLOWER_COMMIT_MAX_DELAY_MS + 1000, 2000,
                            true));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_boot_without_membership_is_standalone);
  RUN_TEST(test_boot_with_membership_holds_in_grace);
  RUN_TEST(test_join_enters_clustered_and_resets_seq_on_new_epoch);
  RUN_TEST(test_contact_reclaims_from_grace_and_blank);
  RUN_TEST(test_contact_refused_in_standalone);
  RUN_TEST(test_render_before_join_is_not_clustered);
  RUN_TEST(test_stale_seq_is_duplicate);
  RUN_TEST(test_silence_parks_clustered_into_grace_then_blank);
  RUN_TEST(test_starved_tick_cascades_straight_to_blank);
  RUN_TEST(test_leave_returns_to_standalone);
  RUN_TEST(test_foreign_join_conflicts_only_while_leader_fresh);
  RUN_TEST(test_phase_names);
  RUN_TEST(test_blank_and_standalone_phases_blank_the_row);
  RUN_TEST(test_render_delay_math);
  return UNITY_END();
}
