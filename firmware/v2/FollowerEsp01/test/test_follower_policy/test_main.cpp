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
  ClusterFollowerState st;
  clusterFollowerBoot(st, 1000, false);
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Standalone, st.phase);
}

static void test_boot_with_membership_holds_in_grace() {
  ClusterFollowerState st;
  clusterFollowerBoot(st, 1000, true);
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Grace, st.phase);
  TEST_ASSERT_EQUAL_UINT32(1000, st.lastContactMs);
}

// --- join / contact ---------------------------------------------------------------

static void test_join_enters_clustered_and_resets_seq_on_new_epoch() {
  ClusterFollowerState st;
  clusterFollowerBoot(st, 0, false);
  clusterFollowerJoin(st, 1000, 7);
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Clustered, st.phase);
  clusterFollowerAcceptRender(st, 1100, 7, 50);
  clusterFollowerJoin(st, 2000, 7);  // same-epoch rejoin keeps seq tracking
  TEST_ASSERT_EQUAL(ClusterRenderVerdict::Duplicate,
                    clusterFollowerAcceptRender(st, 2100, 7, 50));
  clusterFollowerJoin(st, 3000, 9);  // leader rebooted: fresh seq space
  TEST_ASSERT_EQUAL(ClusterRenderVerdict::Apply,
                    clusterFollowerAcceptRender(st, 3100, 9, 1));
}

static void test_contact_reclaims_from_grace_and_blank() {
  ClusterFollowerState st;
  clusterFollowerBoot(st, 0, true);  // Grace
  TEST_ASSERT_TRUE(clusterFollowerContact(st, 5000));
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Clustered, st.phase);
  st.phase = ClusterFollowerPhase::LeaderLost;
  TEST_ASSERT_TRUE(clusterFollowerContact(st, 9000));
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Clustered, st.phase);
}

static void test_contact_refused_in_standalone() {
  ClusterFollowerState st;
  clusterFollowerBoot(st, 0, false);
  TEST_ASSERT_FALSE(clusterFollowerContact(st, 1000));
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Standalone, st.phase);
}

// --- render acceptance -------------------------------------------------------------

static void test_render_before_join_is_not_clustered() {
  ClusterFollowerState st;
  clusterFollowerBoot(st, 0, false);
  TEST_ASSERT_EQUAL(ClusterRenderVerdict::NotClustered,
                    clusterFollowerAcceptRender(st, 1000, 7, 1));
}

static void test_stale_seq_is_duplicate() {
  ClusterFollowerState st;
  clusterFollowerBoot(st, 0, false);
  clusterFollowerJoin(st, 1000, 7);
  TEST_ASSERT_EQUAL(ClusterRenderVerdict::Apply,
                    clusterFollowerAcceptRender(st, 1100, 7, 5));
  TEST_ASSERT_EQUAL(ClusterRenderVerdict::Duplicate,
                    clusterFollowerAcceptRender(st, 1200, 7, 5));
  TEST_ASSERT_EQUAL(ClusterRenderVerdict::Apply,
                    clusterFollowerAcceptRender(st, 1300, 7, 6));
}

// --- silence decay: Clustered -> Grace -> Blank -------------------------------------

static void test_silence_parks_clustered_into_grace_then_blank() {
  ClusterFollowerState st;
  clusterFollowerBoot(st, 0, false);
  clusterFollowerJoin(st, 1000, 7);
  TEST_ASSERT_FALSE(clusterFollowerTick(st, 1000 + CLUSTER_CONTACT_FRESH_MS - 1));
  TEST_ASSERT_TRUE(clusterFollowerTick(st, 1000 + CLUSTER_CONTACT_FRESH_MS));
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Grace, st.phase);
  TEST_ASSERT_FALSE(clusterFollowerTick(st, 1000 + CLUSTER_GRACE_MS - 1));
  TEST_ASSERT_TRUE(clusterFollowerTick(st, 1000 + CLUSTER_GRACE_MS));
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::LeaderLost, st.phase);
}

static void test_starved_tick_cascades_straight_to_blank() {
  // The phase tracks total silence, not tick cadence.
  ClusterFollowerState st;
  clusterFollowerBoot(st, 0, false);
  clusterFollowerJoin(st, 1000, 7);
  TEST_ASSERT_TRUE(clusterFollowerTick(st, 1000 + CLUSTER_GRACE_MS + 5000));
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::LeaderLost, st.phase);
}

// --- leave --------------------------------------------------------------------------

static void test_leave_returns_to_standalone() {
  ClusterFollowerState st;
  clusterFollowerBoot(st, 0, false);
  clusterFollowerJoin(st, 1000, 7);
  clusterFollowerLeave(st);
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Standalone, st.phase);
  TEST_ASSERT_EQUAL(ClusterRenderVerdict::NotClustered,
                    clusterFollowerAcceptRender(st, 2000, 7, 99));
}

// --- sticky leadership (#295 semantics kept) ----------------------------------------

static void test_foreign_join_conflicts_only_while_leader_fresh() {
  ClusterFollowerState st;
  clusterFollowerBoot(st, 0, false);
  clusterFollowerJoin(st, 1000, 7);
  TEST_ASSERT_TRUE(clusterFollowerJoinConflicts(st, 2000, false));
  TEST_ASSERT_FALSE(clusterFollowerJoinConflicts(st, 2000, true));
  // Silence past the fresh window: a successor may claim this row.
  TEST_ASSERT_FALSE(
      clusterFollowerJoinConflicts(st, 1000 + CLUSTER_CONTACT_FRESH_MS, false));
}

// --- phase names + blank rule --------------------------------------------------------

static void test_phase_names() {
  TEST_ASSERT_EQUAL_STRING("standalone",
                           followerPhaseName(ClusterFollowerPhase::Standalone));
  TEST_ASSERT_EQUAL_STRING("clustered",
                           followerPhaseName(ClusterFollowerPhase::Clustered));
  TEST_ASSERT_EQUAL_STRING("grace", followerPhaseName(ClusterFollowerPhase::Grace));
  TEST_ASSERT_EQUAL_STRING("blank", followerPhaseName(ClusterFollowerPhase::LeaderLost));
}

static void test_blank_and_standalone_phases_blank_the_row() {
  // Grace HOLDS the last text; Blank and Standalone show nothing — the row
  // is meaningless without its leader.
  TEST_ASSERT_TRUE(followerPhaseShowsBlank(ClusterFollowerPhase::Standalone));
  TEST_ASSERT_FALSE(followerPhaseShowsBlank(ClusterFollowerPhase::Clustered));
  TEST_ASSERT_FALSE(followerPhaseShowsBlank(ClusterFollowerPhase::Grace));
  TEST_ASSERT_TRUE(followerPhaseShowsBlank(ClusterFollowerPhase::LeaderLost));
}

// --- commitAt flip sync ---------------------------------------------------------------

static void test_render_delay_math() {
  // Unsynced or past commitAt renders immediately; far-future clamps.
  TEST_ASSERT_EQUAL_UINT32(0, clusterRenderDelayMs(2000, 1000, false));
  TEST_ASSERT_EQUAL_UINT32(0, clusterRenderDelayMs(1000, 2000, true));
  TEST_ASSERT_EQUAL_UINT32(400, clusterRenderDelayMs(2400, 2000, true));
  TEST_ASSERT_EQUAL_UINT32(
      CLUSTER_COMMIT_MAX_DELAY_MS,
      clusterRenderDelayMs(2000 + CLUSTER_COMMIT_MAX_DELAY_MS + 1000, 2000,
                            true));
}

// #515: the TX ladder's "traffic confirmed" — a real leader contact, recent.
static void test_leader_contact_fresh_needs_a_real_recent_contact() {
  ClusterFollowerState st;
  clusterFollowerBoot(st, 1000, true);  // stored membership -> Grace
  // Boot stamps lastContactMs for the grace window; that is not a contact.
  TEST_ASSERT_FALSE(clusterFollowerContactFresh(st, 1500));
  clusterFollowerJoin(st, 5000, 7);
  TEST_ASSERT_TRUE(clusterFollowerContactFresh(st, 5001));
  TEST_ASSERT_TRUE(
      clusterFollowerContactFresh(st, 5000 + CLUSTER_CONTACT_FRESH_MS - 1));
  TEST_ASSERT_FALSE(
      clusterFollowerContactFresh(st, 5000 + CLUSTER_CONTACT_FRESH_MS));
  ClusterFollowerState standalone;
  clusterFollowerBoot(standalone, 1000, false);
  TEST_ASSERT_FALSE(clusterFollowerContactFresh(standalone, 1001));
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
  RUN_TEST(test_leader_contact_fresh_needs_a_real_recent_contact);
  return UNITY_END();
}
