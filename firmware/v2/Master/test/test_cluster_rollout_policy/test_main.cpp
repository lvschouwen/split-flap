// Host-side tests for the fleet-firmware rollout policy (#276) — candidate
// selection (rev mismatch, both directions), strictly-sequential phase
// machine, attempt cap → blocked, transient-rejection holdoff, rejoin
// health gate, and the multipart wire bits the streaming upload uses.

#include <unity.h>

#include "../../ClusterRolloutPolicy.h"

void setUp() {}
void tearDown() {}

static const char* LEADER_REV = "1234abc";

static ClusterMemberTable makeTable() {
  ClusterMemberTable t;
  t.count = 3;
  // Row 0 = leader's own row (empty host), rows 1-2 remote.
  t.members[0].row = 0;
  t.members[0].width = 16;
  strcpy(t.members[1].host, "192.168.15.91");
  t.members[1].row = 1;
  t.members[1].width = 16;
  strcpy(t.members[2].host, "192.168.15.92");
  t.members[2].row = 2;
  t.members[2].width = 16;
  return t;
}

static void primeJoined(ClusterMemberRuntime* r, const char* rev1,
                        const char* rev2) {
  r[1].joined = true;
  r[1].rev = rev1;
  r[2].joined = true;
  r[2].rev = rev2;
}

// --- candidate selection -------------------------------------------------------

static void test_no_candidate_when_all_match() {
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  primeJoined(r, LEADER_REV, LEADER_REV);
  ClusterRolloutState st;
  TEST_ASSERT_EQUAL(-1, clusterRolloutNextCandidate(t, r, LEADER_REV, st, 1000));
}

static void test_mismatch_selects_first_candidate() {
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  primeJoined(r, "0000000", "0000000");
  ClusterRolloutState st;
  TEST_ASSERT_EQUAL(1, clusterRolloutNextCandidate(t, r, LEADER_REV, st, 1000));
}

static void test_newer_follower_is_converged_back_too() {
  // BOTH directions: any rev != leader rev is a candidate.
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  primeJoined(r, LEADER_REV, "fffffff");
  ClusterRolloutState st;
  TEST_ASSERT_EQUAL(2, clusterRolloutNextCandidate(t, r, LEADER_REV, st, 1000));
}

static void test_unjoined_and_unknown_rev_are_not_candidates() {
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  r[1].joined = false;  // never converge a member we can't even reach
  r[1].rev = "0000000";
  r[2].joined = true;
  r[2].rev = "";  // pre-#273 join reply: rev unknown — don't guess
  ClusterRolloutState st;
  TEST_ASSERT_EQUAL(-1, clusterRolloutNextCandidate(t, r, LEADER_REV, st, 1000));
}

static void test_no_candidate_while_not_idle() {
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  primeJoined(r, "0000000", "0000000");
  ClusterRolloutState st;
  clusterRolloutStart(st, 1, 1000000);
  TEST_ASSERT_EQUAL(-1, clusterRolloutNextCandidate(t, r, LEADER_REV, st, 1000));
}

static void test_holdoff_gates_candidates() {
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  primeJoined(r, "0000000", LEADER_REV);
  ClusterRolloutState st;
  clusterRolloutStart(st, 1, 1000000);
  clusterRolloutUploadRejected(st, 1000);  // follower busy → holdoff
  TEST_ASSERT_EQUAL(-1, clusterRolloutNextCandidate(
                            t, r, LEADER_REV, st,
                            1000 + CLUSTER_ROLLOUT_RETRY_HOLDOFF_MS - 1));
  TEST_ASSERT_EQUAL(1, clusterRolloutNextCandidate(
                           t, r, LEADER_REV, st,
                           1000 + CLUSTER_ROLLOUT_RETRY_HOLDOFF_MS));
}

// --- phase machine ---------------------------------------------------------------

static void test_start_tracks_progress_target() {
  ClusterRolloutState st;
  clusterRolloutStart(st, 2, 2600000);
  TEST_ASSERT_EQUAL(ClusterRolloutPhase::Uploading, st.phase);
  TEST_ASSERT_EQUAL(2, st.memberIndex);
  TEST_ASSERT_EQUAL_UINT32(2600000, st.bytesTotal);
  TEST_ASSERT_EQUAL_UINT32(0, st.bytesSent);
}

static void test_rejection_burns_no_attempt() {
  ClusterRolloutState st;
  clusterRolloutStart(st, 1, 1000);
  clusterRolloutUploadRejected(st, 5000);
  TEST_ASSERT_EQUAL(ClusterRolloutPhase::Idle, st.phase);
  TEST_ASSERT_EQUAL_UINT8(0, st.attempts[1]);
  TEST_ASSERT_FALSE(st.blocked[1]);
}

static void test_every_idle_transition_clears_progress() {
  // Stale bytes next to phase "idle" would read as a wedged rollout in
  // the Cluster card.
  ClusterRolloutState st;
  clusterRolloutStart(st, 1, 2600000);
  st.bytesSent = 1000000;
  clusterRolloutUploadRejected(st, 5000);
  TEST_ASSERT_EQUAL_UINT32(0, st.bytesSent);
  TEST_ASSERT_EQUAL_UINT32(0, st.bytesTotal);

  clusterRolloutStart(st, 1, 2600000);
  st.bytesSent = 1000000;
  clusterRolloutUploadFailed(st, 90000);
  TEST_ASSERT_EQUAL_UINT32(0, st.bytesSent);
  TEST_ASSERT_EQUAL_UINT32(0, st.bytesTotal);

  clusterRolloutStart(st, 1, 2600000);
  st.bytesSent = 2600000;
  clusterRolloutUploadDone(st, 200000);
  clusterRolloutCheckWait(st, true, String(LEADER_REV), LEADER_REV, 210000);
  TEST_ASSERT_EQUAL_UINT32(0, st.bytesSent);
  TEST_ASSERT_EQUAL_UINT32(0, st.bytesTotal);
}

static void test_failure_cap_blocks_member() {
  ClusterRolloutState st;
  for (int i = 0; i < CLUSTER_ROLLOUT_ATTEMPT_CAP; i++) {
    clusterRolloutStart(st, 1, 1000);
    clusterRolloutUploadFailed(st, 5000);
    TEST_ASSERT_EQUAL(ClusterRolloutPhase::Idle, st.phase);
  }
  TEST_ASSERT_TRUE(st.blocked[1]);
  TEST_ASSERT_EQUAL_UINT8(CLUSTER_ROLLOUT_ATTEMPT_CAP, st.attempts[1]);

  // A blocked member is never re-selected.
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  primeJoined(r, "0000000", LEADER_REV);
  TEST_ASSERT_EQUAL(-1, clusterRolloutNextCandidate(t, r, LEADER_REV, st,
                                                    5000 + 60000000));
}

static void test_upload_done_waits_for_rejoin() {
  ClusterRolloutState st;
  clusterRolloutStart(st, 1, 1000);
  clusterRolloutUploadDone(st, 10000);
  TEST_ASSERT_EQUAL(ClusterRolloutPhase::WaitingRejoin, st.phase);
  TEST_ASSERT_EQUAL_UINT32(10000 + CLUSTER_ROLLOUT_REJOIN_TIMEOUT_MS,
                           st.waitDeadlineMs);
}

// --- rejoin health gate ----------------------------------------------------------

static void test_wait_converged_resets_attempts() {
  ClusterRolloutState st;
  clusterRolloutStart(st, 1, 1000);
  clusterRolloutUploadFailed(st, 100);  // one earlier failed attempt
  clusterRolloutStart(st, 1, 1000);
  clusterRolloutUploadDone(st, 10000);
  TEST_ASSERT_EQUAL(
      (int)ClusterRolloutWait::Converged,
      (int)clusterRolloutCheckWait(st, true, String(LEADER_REV), LEADER_REV,
                                   20000));
  TEST_ASSERT_EQUAL(ClusterRolloutPhase::Idle, st.phase);
  TEST_ASSERT_EQUAL_UINT8(0, st.attempts[1]);
  TEST_ASSERT_FALSE(st.blocked[1]);
}

static void test_wait_still_waiting_while_member_down() {
  ClusterRolloutState st;
  clusterRolloutStart(st, 1, 1000);
  clusterRolloutUploadDone(st, 10000);
  TEST_ASSERT_EQUAL((int)ClusterRolloutWait::Waiting,
                    (int)clusterRolloutCheckWait(st, false, String(""),
                                                 LEADER_REV, 20000));
  TEST_ASSERT_EQUAL(ClusterRolloutPhase::WaitingRejoin, st.phase);
}

static void test_wait_rollback_burns_attempt() {
  // The follower came back on its OLD rev — native rollback rejected the
  // image. Retrying forever would flash-loop the board; count it.
  ClusterRolloutState st;
  clusterRolloutStart(st, 1, 1000);
  clusterRolloutUploadDone(st, 10000);
  TEST_ASSERT_EQUAL(
      (int)ClusterRolloutWait::RolledBack,
      (int)clusterRolloutCheckWait(st, true, String("0000000"), LEADER_REV,
                                   20000));
  TEST_ASSERT_EQUAL(ClusterRolloutPhase::Idle, st.phase);
  TEST_ASSERT_EQUAL_UINT8(1, st.attempts[1]);
}

static void test_wait_timeout_burns_attempt() {
  ClusterRolloutState st;
  clusterRolloutStart(st, 1, 1000);
  clusterRolloutUploadDone(st, 10000);
  uint32_t after = 10000 + CLUSTER_ROLLOUT_REJOIN_TIMEOUT_MS;
  TEST_ASSERT_EQUAL((int)ClusterRolloutWait::TimedOut,
                    (int)clusterRolloutCheckWait(st, false, String(""),
                                                 LEADER_REV, after));
  TEST_ASSERT_EQUAL(ClusterRolloutPhase::Idle, st.phase);
  TEST_ASSERT_EQUAL_UINT8(1, st.attempts[1]);
}

static void test_reset_clears_everything() {
  ClusterRolloutState st;
  clusterRolloutStart(st, 1, 1000);
  clusterRolloutUploadFailed(st, 100);
  clusterRolloutReset(st);
  TEST_ASSERT_EQUAL(ClusterRolloutPhase::Idle, st.phase);
  TEST_ASSERT_EQUAL(-1, st.memberIndex);
  TEST_ASSERT_EQUAL_UINT8(0, st.attempts[1]);
  TEST_ASSERT_FALSE(st.blocked[1]);
}

// --- wire bits -------------------------------------------------------------------

static void test_multipart_frame_matches_upload_contract() {
  // The follower's /firmware/master parses multipart field "firmware" —
  // the exact strings the streaming client wraps the image in.
  String pre = clusterRolloutMultipartPreamble();
  TEST_ASSERT_TRUE(pre.startsWith("--" CLUSTER_ROLLOUT_BOUNDARY "\r\n"));
  TEST_ASSERT_TRUE(pre.indexOf("name=\"firmware\"") >= 0);
  TEST_ASSERT_TRUE(pre.indexOf("filename=") >= 0);
  TEST_ASSERT_TRUE(pre.endsWith("\r\n\r\n"));
  String tail = clusterRolloutMultipartTrailer();
  TEST_ASSERT_EQUAL_STRING("\r\n--" CLUSTER_ROLLOUT_BOUNDARY "--\r\n",
                           tail.c_str());
}

static void test_upload_url_carries_md5_and_intended_rev() {
  TEST_ASSERT_EQUAL_STRING(
      "http://192.168.15.91/firmware/master"
      "?md5=0123456789abcdef0123456789abcdef&v=1234abc",
      clusterRolloutUrl("192.168.15.91",
                        "0123456789abcdef0123456789abcdef", "1234abc")
          .c_str());
}

static void test_phase_names() {
  TEST_ASSERT_EQUAL_STRING("idle",
                           clusterRolloutPhaseName(ClusterRolloutPhase::Idle));
  TEST_ASSERT_EQUAL_STRING(
      "uploading", clusterRolloutPhaseName(ClusterRolloutPhase::Uploading));
  TEST_ASSERT_EQUAL_STRING(
      "waiting", clusterRolloutPhaseName(ClusterRolloutPhase::WaitingRejoin));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_no_candidate_when_all_match);
  RUN_TEST(test_mismatch_selects_first_candidate);
  RUN_TEST(test_newer_follower_is_converged_back_too);
  RUN_TEST(test_unjoined_and_unknown_rev_are_not_candidates);
  RUN_TEST(test_no_candidate_while_not_idle);
  RUN_TEST(test_holdoff_gates_candidates);
  RUN_TEST(test_start_tracks_progress_target);
  RUN_TEST(test_rejection_burns_no_attempt);
  RUN_TEST(test_every_idle_transition_clears_progress);
  RUN_TEST(test_failure_cap_blocks_member);
  RUN_TEST(test_upload_done_waits_for_rejoin);
  RUN_TEST(test_wait_converged_resets_attempts);
  RUN_TEST(test_wait_still_waiting_while_member_down);
  RUN_TEST(test_wait_rollback_burns_attempt);
  RUN_TEST(test_wait_timeout_burns_attempt);
  RUN_TEST(test_reset_clears_everything);
  RUN_TEST(test_multipart_frame_matches_upload_contract);
  RUN_TEST(test_upload_url_carries_md5_and_intended_rev);
  RUN_TEST(test_phase_names);
  return UNITY_END();
}
