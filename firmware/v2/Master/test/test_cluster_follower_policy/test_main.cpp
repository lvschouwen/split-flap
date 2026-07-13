// Host-side tests for the follower-side cluster state machine (#272) —
// boot gating, grace/fallback decay, reclaim, epoch/seq staleness armor,
// producer-gate predicates and the commitAt flip-sync delay.

#include <unity.h>

#include "../../ClusterFollowerPolicy.h"

void setUp() {}
void tearDown() {}

// A follower that booted with a membership, got joined and saw one render.
static ClusterFollowerState makeClustered(uint32_t nowMs, uint32_t epoch) {
  ClusterFollowerState st;
  clusterFollowerBoot(st, nowMs, true);
  clusterFollowerJoin(st, nowMs, epoch);
  return st;
}

// --- boot ------------------------------------------------------------------------

static void test_boot_without_membership_is_standalone() {
  ClusterFollowerState st;
  clusterFollowerBoot(st, 1000, false);
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Standalone, st.phase);
}

static void test_boot_with_membership_gates_in_grace() {
  ClusterFollowerState st;
  clusterFollowerBoot(st, 1000, true);
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Grace, st.phase);
}

static void test_boot_grace_decays_to_local_fallback() {
  ClusterFollowerState st;
  clusterFollowerBoot(st, 1000, true);
  TEST_ASSERT_FALSE(clusterFollowerTick(st, 1000 + CLUSTER_GRACE_MS - 1));
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Grace, st.phase);
  TEST_ASSERT_TRUE(clusterFollowerTick(st, 1000 + CLUSTER_GRACE_MS));
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::LocalFallback, st.phase);
}

// --- join / contact / decay -------------------------------------------------------

static void test_join_enters_clustered() {
  ClusterFollowerState st;
  clusterFollowerBoot(st, 1000, false);
  clusterFollowerJoin(st, 2000, 0xAABBCCDD);
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Clustered, st.phase);
}

static void test_clustered_holds_while_pings_arrive() {
  ClusterFollowerState st = makeClustered(1000, 7);
  for (uint32_t t = 11000; t <= 61000; t += 10000) {
    TEST_ASSERT_TRUE(clusterFollowerContact(st, t));
    TEST_ASSERT_FALSE(clusterFollowerTick(st, t + 1000));
  }
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Clustered, st.phase);
}

static void test_missed_pings_park_in_grace() {
  ClusterFollowerState st = makeClustered(1000, 7);
  TEST_ASSERT_FALSE(clusterFollowerTick(st, 1000 + CLUSTER_CONTACT_FRESH_MS - 1));
  TEST_ASSERT_TRUE(clusterFollowerTick(st, 1000 + CLUSTER_CONTACT_FRESH_MS));
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Grace, st.phase);
}

static void test_grace_decays_to_local_fallback_at_grace_deadline() {
  ClusterFollowerState st = makeClustered(1000, 7);
  clusterFollowerTick(st, 1000 + CLUSTER_CONTACT_FRESH_MS);
  // The grace clock runs from the LAST CONTACT, not from entering Grace.
  TEST_ASSERT_FALSE(clusterFollowerTick(st, 1000 + CLUSTER_GRACE_MS - 1));
  TEST_ASSERT_TRUE(clusterFollowerTick(st, 1000 + CLUSTER_GRACE_MS));
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::LocalFallback, st.phase);
}

static void test_contact_in_grace_reclaims_to_clustered() {
  ClusterFollowerState st = makeClustered(1000, 7);
  clusterFollowerTick(st, 1000 + CLUSTER_CONTACT_FRESH_MS);
  TEST_ASSERT_TRUE(clusterFollowerContact(st, 50000));
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Clustered, st.phase);
}

static void test_contact_in_local_fallback_reclaims_to_clustered() {
  ClusterFollowerState st = makeClustered(1000, 7);
  clusterFollowerTick(st, 1000 + CLUSTER_GRACE_MS);
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::LocalFallback, st.phase);
  TEST_ASSERT_TRUE(clusterFollowerContact(st, 200000));
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Clustered, st.phase);
}

static void test_contact_in_standalone_is_refused() {
  ClusterFollowerState st;
  clusterFollowerBoot(st, 1000, false);
  TEST_ASSERT_FALSE(clusterFollowerContact(st, 2000));
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Standalone, st.phase);
}

static void test_millis_rollover_does_not_fake_a_deadline() {
  ClusterFollowerState st = makeClustered(0xFFFFF000UL, 7);
  clusterFollowerContact(st, 0xFFFFF000UL);
  // 8 s of real elapsed time across the wrap: still fresh.
  TEST_ASSERT_FALSE(clusterFollowerTick(st, 0xFFFFF000UL + 8000UL));
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Clustered, st.phase);
}

// --- render acceptance (epoch/seq armor) -------------------------------------------

static void test_render_in_standalone_is_not_clustered() {
  ClusterFollowerState st;
  clusterFollowerBoot(st, 1000, false);
  TEST_ASSERT_EQUAL(ClusterRenderVerdict::NotClustered,
                    clusterFollowerAcceptRender(st, 2000, 7, 1));
}

static void test_first_render_after_join_applies() {
  ClusterFollowerState st = makeClustered(1000, 7);
  TEST_ASSERT_EQUAL(ClusterRenderVerdict::Apply,
                    clusterFollowerAcceptRender(st, 2000, 7, 1));
  TEST_ASSERT_EQUAL(1, st.lastSeq);
}

static void test_same_epoch_higher_seq_applies() {
  ClusterFollowerState st = makeClustered(1000, 7);
  clusterFollowerAcceptRender(st, 2000, 7, 5);
  TEST_ASSERT_EQUAL(ClusterRenderVerdict::Apply,
                    clusterFollowerAcceptRender(st, 3000, 7, 6));
  TEST_ASSERT_EQUAL(6, st.lastSeq);
}

static void test_same_epoch_stale_seq_is_duplicate() {
  ClusterFollowerState st = makeClustered(1000, 7);
  clusterFollowerAcceptRender(st, 2000, 7, 5);
  TEST_ASSERT_EQUAL(ClusterRenderVerdict::Duplicate,
                    clusterFollowerAcceptRender(st, 3000, 7, 5));
  TEST_ASSERT_EQUAL(ClusterRenderVerdict::Duplicate,
                    clusterFollowerAcceptRender(st, 4000, 7, 3));
  TEST_ASSERT_EQUAL(5, st.lastSeq);
}

static void test_duplicate_still_feeds_the_grace_timer() {
  ClusterFollowerState st = makeClustered(1000, 7);
  clusterFollowerAcceptRender(st, 2000, 7, 5);
  clusterFollowerTick(st, 2000 + CLUSTER_CONTACT_FRESH_MS);
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Grace, st.phase);
  // A delayed retry of an applied seq is still live leader contact.
  clusterFollowerAcceptRender(st, 60000, 7, 5);
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Clustered, st.phase);
}

static void test_new_epoch_any_seq_applies() {
  ClusterFollowerState st = makeClustered(1000, 7);
  clusterFollowerAcceptRender(st, 2000, 7, 50);
  // Leader rebooted: fresh epoch restarts the sequence space.
  TEST_ASSERT_EQUAL(ClusterRenderVerdict::Apply,
                    clusterFollowerAcceptRender(st, 3000, 9, 2));
  TEST_ASSERT_EQUAL(9, st.epoch);
  TEST_ASSERT_EQUAL(2, st.lastSeq);
}

static void test_render_after_nvs_boot_applies_without_prior_epoch() {
  ClusterFollowerState st;
  clusterFollowerBoot(st, 1000, true);
  TEST_ASSERT_EQUAL(ClusterRenderVerdict::Apply,
                    clusterFollowerAcceptRender(st, 2000, 42, 17));
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Clustered, st.phase);
  TEST_ASSERT_EQUAL(42, st.epoch);
  TEST_ASSERT_EQUAL(17, st.lastSeq);
}

static void test_rejoin_resets_seq_tracking() {
  ClusterFollowerState st = makeClustered(1000, 7);
  clusterFollowerAcceptRender(st, 2000, 7, 50);
  clusterFollowerJoin(st, 3000, 7);  // leader re-joins after degraded
  TEST_ASSERT_EQUAL(ClusterRenderVerdict::Apply,
                    clusterFollowerAcceptRender(st, 4000, 7, 50));
}

static void test_leave_returns_to_standalone() {
  ClusterFollowerState st = makeClustered(1000, 7);
  clusterFollowerLeave(st);
  TEST_ASSERT_EQUAL(ClusterFollowerPhase::Standalone, st.phase);
  TEST_ASSERT_EQUAL(ClusterRenderVerdict::NotClustered,
                    clusterFollowerAcceptRender(st, 2000, 7, 60));
}

// --- gate predicates ----------------------------------------------------------------

static void test_producer_gate_holds_in_every_phase_but_standalone() {
  ClusterFollowerState st;
  clusterFollowerBoot(st, 1000, false);
  TEST_ASSERT_FALSE(clusterFollowerGatesProducers(st));
  st = makeClustered(1000, 7);
  TEST_ASSERT_TRUE(clusterFollowerGatesProducers(st));
  clusterFollowerTick(st, 1000 + CLUSTER_CONTACT_FRESH_MS);
  TEST_ASSERT_TRUE(clusterFollowerGatesProducers(st));
  clusterFollowerTick(st, 1000 + CLUSTER_GRACE_MS);
  TEST_ASSERT_TRUE(clusterFollowerGatesProducers(st));
}

static void test_local_clock_forced_only_in_local_fallback() {
  ClusterFollowerState st = makeClustered(1000, 7);
  TEST_ASSERT_FALSE(clusterFollowerForcesLocalClock(st));
  clusterFollowerTick(st, 1000 + CLUSTER_CONTACT_FRESH_MS);
  TEST_ASSERT_FALSE(clusterFollowerForcesLocalClock(st));  // Grace HOLDS
  clusterFollowerTick(st, 1000 + CLUSTER_GRACE_MS);
  TEST_ASSERT_TRUE(clusterFollowerForcesLocalClock(st));
}

// --- commitAt flip sync ---------------------------------------------------------------

static void test_render_delay_unsynced_clock_is_immediate() {
  TEST_ASSERT_EQUAL_UINT32(0, clusterRenderDelayMs(2000000000400ULL,
                                                   2000000000000ULL, false));
}

static void test_render_delay_past_commit_is_immediate() {
  TEST_ASSERT_EQUAL_UINT32(0, clusterRenderDelayMs(1999999999000ULL,
                                                   2000000000000ULL, true));
}

static void test_render_delay_near_future_waits_exactly() {
  TEST_ASSERT_EQUAL_UINT32(400, clusterRenderDelayMs(2000000000400ULL,
                                                     2000000000000ULL, true));
}

static void test_render_delay_far_future_clamps() {
  TEST_ASSERT_EQUAL_UINT32(CLUSTER_COMMIT_MAX_DELAY_MS,
                           clusterRenderDelayMs(2000000060000ULL,
                                                2000000000000ULL, true));
}

// --- vocabulary -----------------------------------------------------------------------

static void test_phase_names_for_health_json() {
  TEST_ASSERT_EQUAL_STRING(
      "standalone", clusterFollowerPhaseName(ClusterFollowerPhase::Standalone));
  TEST_ASSERT_EQUAL_STRING(
      "clustered", clusterFollowerPhaseName(ClusterFollowerPhase::Clustered));
  TEST_ASSERT_EQUAL_STRING(
      "grace", clusterFollowerPhaseName(ClusterFollowerPhase::Grace));
  TEST_ASSERT_EQUAL_STRING(
      "local-fallback",
      clusterFollowerPhaseName(ClusterFollowerPhase::LocalFallback));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_boot_without_membership_is_standalone);
  RUN_TEST(test_boot_with_membership_gates_in_grace);
  RUN_TEST(test_boot_grace_decays_to_local_fallback);
  RUN_TEST(test_join_enters_clustered);
  RUN_TEST(test_clustered_holds_while_pings_arrive);
  RUN_TEST(test_missed_pings_park_in_grace);
  RUN_TEST(test_grace_decays_to_local_fallback_at_grace_deadline);
  RUN_TEST(test_contact_in_grace_reclaims_to_clustered);
  RUN_TEST(test_contact_in_local_fallback_reclaims_to_clustered);
  RUN_TEST(test_contact_in_standalone_is_refused);
  RUN_TEST(test_millis_rollover_does_not_fake_a_deadline);
  RUN_TEST(test_render_in_standalone_is_not_clustered);
  RUN_TEST(test_first_render_after_join_applies);
  RUN_TEST(test_same_epoch_higher_seq_applies);
  RUN_TEST(test_same_epoch_stale_seq_is_duplicate);
  RUN_TEST(test_duplicate_still_feeds_the_grace_timer);
  RUN_TEST(test_new_epoch_any_seq_applies);
  RUN_TEST(test_render_after_nvs_boot_applies_without_prior_epoch);
  RUN_TEST(test_rejoin_resets_seq_tracking);
  RUN_TEST(test_leave_returns_to_standalone);
  RUN_TEST(test_producer_gate_holds_in_every_phase_but_standalone);
  RUN_TEST(test_local_clock_forced_only_in_local_fallback);
  RUN_TEST(test_render_delay_unsynced_clock_is_immediate);
  RUN_TEST(test_render_delay_past_commit_is_immediate);
  RUN_TEST(test_render_delay_near_future_waits_exactly);
  RUN_TEST(test_render_delay_far_future_clamps);
  RUN_TEST(test_phase_names_for_health_json);
  return UNITY_END();
}
