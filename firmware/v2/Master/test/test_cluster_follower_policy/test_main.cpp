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

static void test_same_epoch_rejoin_preserves_seq_tracking() {
  // Leader re-joins after a degraded spell WITHOUT rebooting (same epoch):
  // a delayed retry of an old render must still be rejected — the leader
  // mints fresh, higher seqs, so its post-rejoin re-send applies anyway.
  ClusterFollowerState st = makeClustered(1000, 7);
  clusterFollowerAcceptRender(st, 2000, 7, 50);
  clusterFollowerJoin(st, 3000, 7);
  TEST_ASSERT_EQUAL(ClusterRenderVerdict::Duplicate,
                    clusterFollowerAcceptRender(st, 4000, 7, 50));
  TEST_ASSERT_EQUAL(ClusterRenderVerdict::Apply,
                    clusterFollowerAcceptRender(st, 5000, 7, 51));
}

static void test_new_epoch_join_resets_seq_tracking() {
  ClusterFollowerState st = makeClustered(1000, 7);
  clusterFollowerAcceptRender(st, 2000, 7, 50);
  clusterFollowerJoin(st, 3000, 9);  // leader rebooted: fresh epoch
  TEST_ASSERT_EQUAL(ClusterRenderVerdict::Apply,
                    clusterFollowerAcceptRender(st, 4000, 9, 1));
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

// --- join conflict (#295 sticky leadership) ---------------------------------------

static void test_join_conflict_rejects_foreign_leader_while_clustered_fresh() {
  ClusterFollowerState st = makeClustered(1000, 42);
  TEST_ASSERT_TRUE(clusterFollowerJoinConflicts(st, 2000, false));
}

static void test_join_conflict_allows_same_leader_always() {
  ClusterFollowerState st = makeClustered(1000, 42);
  TEST_ASSERT_FALSE(clusterFollowerJoinConflicts(st, 2000, true));
}

static void test_join_conflict_allows_takeover_once_contact_is_stale() {
  // The current leader has been silent past the contact-fresh window — a
  // promoted successor must be able to claim the follower.
  ClusterFollowerState st = makeClustered(1000, 42);
  TEST_ASSERT_FALSE(clusterFollowerJoinConflicts(
      st, 1000 + CLUSTER_CONTACT_FRESH_MS, false));
}

static void test_join_conflict_never_fires_outside_clustered() {
  ClusterFollowerState st;
  clusterFollowerBoot(st, 1000, true);  // Grace
  TEST_ASSERT_FALSE(clusterFollowerJoinConflicts(st, 1500, false));
  clusterFollowerLeave(st);  // Standalone
  TEST_ASSERT_FALSE(clusterFollowerJoinConflicts(st, 1500, false));
}

// --- promote gate (#295) -----------------------------------------------------------

static void test_promote_allowed_only_in_local_fallback() {
  ClusterFollowerState st = makeClustered(1000, 42);
  TEST_ASSERT_FALSE(clusterFollowerCanPromote(st));
  clusterFollowerTick(st, 1000 + CLUSTER_CONTACT_FRESH_MS);  // Grace
  TEST_ASSERT_FALSE(clusterFollowerCanPromote(st));
  clusterFollowerTick(st, 1000 + CLUSTER_GRACE_MS);  // LocalFallback
  TEST_ASSERT_TRUE(clusterFollowerCanPromote(st));
  clusterFollowerLeave(st);
  TEST_ASSERT_FALSE(clusterFollowerCanPromote(st));
}

// --- ranked auto-takeover (#321) -----------------------------------------------

static void test_successor_rank_parses_position() {
  TEST_ASSERT_EQUAL(0, clusterSuccessorRank("2,3", 2));
  TEST_ASSERT_EQUAL(1, clusterSuccessorRank("2,3", 3));
  TEST_ASSERT_EQUAL(0, clusterSuccessorRank("4", 4));
  TEST_ASSERT_EQUAL(2, clusterSuccessorRank("5,1,3", 3));
  TEST_ASSERT_EQUAL(-1, clusterSuccessorRank("2,3", 5));   // not a successor
  TEST_ASSERT_EQUAL(-1, clusterSuccessorRank("", 2));      // no list
  TEST_ASSERT_EQUAL(-1, clusterSuccessorRank("2,3", -1));  // no self index
}

// Build a Grace-phase follower whose leader last spoke at t=1000.
static ClusterFollowerState makeGraceSince1000() {
  ClusterFollowerState st = makeClustered(1000, 7);
  clusterFollowerTick(st, 1000 + CLUSTER_CONTACT_FRESH_MS);  // -> Grace
  return st;
}

static void test_auto_promote_ignores_non_successor() {
  ClusterFollowerState st = makeGraceSince1000();
  // Way past any threshold, but rank -1 (an ESP-01 / foreign board) never fires.
  TEST_ASSERT_FALSE(clusterFollowerAutoPromoteDue(st, -1, 0, 1000000));
}

static void test_auto_promote_fires_at_threshold_for_primary() {
  ClusterFollowerState st = makeGraceSince1000();  // lastContactMs == 1000
  // rank 0 → threshold = 30 s of silence.
  TEST_ASSERT_FALSE(clusterFollowerAutoPromoteDue(
      st, 0, 0, 1000 + CLUSTER_AUTO_TAKEOVER_MS - 1));
  TEST_ASSERT_TRUE(clusterFollowerAutoPromoteDue(
      st, 0, 0, 1000 + CLUSTER_AUTO_TAKEOVER_MS));
}

static void test_auto_promote_staggered_by_rank() {
  ClusterFollowerState st = makeGraceSince1000();
  uint32_t primaryFire = 1000 + CLUSTER_AUTO_TAKEOVER_MS;
  // At the primary's fire time the secondary (rank 1) must NOT yet fire.
  TEST_ASSERT_FALSE(clusterFollowerAutoPromoteDue(st, 1, 0, primaryFire));
  // It fires one stagger later.
  TEST_ASSERT_TRUE(clusterFollowerAutoPromoteDue(
      st, 1, 0, primaryFire + CLUSTER_AUTO_TAKEOVER_STAGGER_MS));
}

static void test_auto_promote_suppressed_by_hold() {
  ClusterFollowerState st = makeGraceSince1000();
  uint32_t now = 1000 + CLUSTER_AUTO_TAKEOVER_MS + 10000;  // well past threshold
  uint32_t holdUntil = now + 5000;                          // hold still active
  TEST_ASSERT_FALSE(clusterFollowerAutoPromoteDue(st, 0, holdUntil, now));
  // Once the hold expires it fires.
  TEST_ASSERT_TRUE(clusterFollowerAutoPromoteDue(st, 0, holdUntil, holdUntil));
}

static void test_auto_promote_not_while_leader_fresh() {
  // Still Clustered (leader in contact) — never auto-promote, even if the
  // clock is arbitrarily far ahead.
  ClusterFollowerState st = makeClustered(1000, 7);
  TEST_ASSERT_FALSE(clusterFollowerAutoPromoteDue(st, 0, 0, 1000000));
  // Standalone (no membership) likewise.
  ClusterFollowerState fresh;
  TEST_ASSERT_FALSE(clusterFollowerAutoPromoteDue(fresh, 0, 0, 1000000));
}

static void test_hold_deadline_clamps() {
  TEST_ASSERT_EQUAL_UINT32(6000, clusterAutoTakeoverHoldDeadline(5000, 1000));
  TEST_ASSERT_EQUAL_UINT32(1000 + CLUSTER_AUTO_TAKEOVER_HOLD_MAX_MS,
                           clusterAutoTakeoverHoldDeadline(999999, 1000));
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
  RUN_TEST(test_same_epoch_rejoin_preserves_seq_tracking);
  RUN_TEST(test_new_epoch_join_resets_seq_tracking);
  RUN_TEST(test_leave_returns_to_standalone);
  RUN_TEST(test_producer_gate_holds_in_every_phase_but_standalone);
  RUN_TEST(test_local_clock_forced_only_in_local_fallback);
  RUN_TEST(test_render_delay_unsynced_clock_is_immediate);
  RUN_TEST(test_render_delay_past_commit_is_immediate);
  RUN_TEST(test_render_delay_near_future_waits_exactly);
  RUN_TEST(test_render_delay_far_future_clamps);
  RUN_TEST(test_phase_names_for_health_json);
  RUN_TEST(test_join_conflict_rejects_foreign_leader_while_clustered_fresh);
  RUN_TEST(test_join_conflict_allows_same_leader_always);
  RUN_TEST(test_join_conflict_allows_takeover_once_contact_is_stale);
  RUN_TEST(test_join_conflict_never_fires_outside_clustered);
  RUN_TEST(test_promote_allowed_only_in_local_fallback);
  RUN_TEST(test_successor_rank_parses_position);
  RUN_TEST(test_auto_promote_ignores_non_successor);
  RUN_TEST(test_auto_promote_fires_at_threshold_for_primary);
  RUN_TEST(test_auto_promote_staggered_by_rank);
  RUN_TEST(test_auto_promote_suppressed_by_hold);
  RUN_TEST(test_auto_promote_not_while_leader_fresh);
  RUN_TEST(test_hold_deadline_clamps);
  return UNITY_END();
}
