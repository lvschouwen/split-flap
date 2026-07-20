// Host-side tests for the fleet-firmware rollout policy (#276) — candidate
// selection (rev mismatch, both directions), strictly-sequential phase
// machine, attempt cap → blocked, transient-rejection holdoff, rejoin
// health gate, and the multipart wire bits the streaming upload uses.

#include <unity.h>

#include "../../ClusterRolloutPolicy.h"

void setUp() {}
void tearDown() {}

static const char* LEADER_REV = "1234abc";
static const char* LEADER_PLAT = "esp32s3";

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
  TEST_ASSERT_EQUAL(-1, clusterRolloutNextCandidate(t, r, LEADER_REV, LEADER_PLAT, st, 1000));
}

static void test_mismatch_selects_first_candidate() {
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  primeJoined(r, "0000000", "0000000");
  ClusterRolloutState st;
  TEST_ASSERT_EQUAL(1, clusterRolloutNextCandidate(t, r, LEADER_REV, LEADER_PLAT, st, 1000));
}

static void test_newer_follower_is_converged_back_too() {
  // BOTH directions: any rev != leader rev is a candidate.
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  primeJoined(r, LEADER_REV, "fffffff");
  ClusterRolloutState st;
  TEST_ASSERT_EQUAL(2, clusterRolloutNextCandidate(t, r, LEADER_REV, LEADER_PLAT, st, 1000));
}

static void test_unjoined_and_unknown_rev_are_not_candidates() {
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  r[1].joined = false;  // never converge a member we can't even reach
  r[1].rev = "0000000";
  r[2].joined = true;
  r[2].rev = "";  // pre-#273 join reply: rev unknown — don't guess
  ClusterRolloutState st;
  TEST_ASSERT_EQUAL(-1, clusterRolloutNextCandidate(t, r, LEADER_REV, LEADER_PLAT, st, 1000));
}

static void test_no_candidate_while_not_idle() {
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  primeJoined(r, "0000000", "0000000");
  ClusterRolloutState st;
  clusterRolloutStart(st, 1, 1000000);
  TEST_ASSERT_EQUAL(-1, clusterRolloutNextCandidate(t, r, LEADER_REV, LEADER_PLAT, st, 1000));
}

static void test_holdoff_gates_candidates() {
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  primeJoined(r, "0000000", LEADER_REV);
  ClusterRolloutState st;
  clusterRolloutStart(st, 1, 1000000);
  clusterRolloutUploadRejected(st, 1000);  // follower busy → holdoff
  TEST_ASSERT_EQUAL(-1, clusterRolloutNextCandidate(
                            t, r, LEADER_REV, LEADER_PLAT, st,
                            1000 + CLUSTER_ROLLOUT_RETRY_HOLDOFF_MS - 1));
  TEST_ASSERT_EQUAL(1, clusterRolloutNextCandidate(
                           t, r, LEADER_REV, LEADER_PLAT, st,
                           1000 + CLUSTER_ROLLOUT_RETRY_HOLDOFF_MS));
}

static void test_foreign_plat_is_never_a_candidate() {
  // #297: without this the leader streams its S3 image into an ESP-01's
  // /firmware/master. A member whose reported plat differs from the
  // leader's own is excluded from convergence entirely — rev mismatch or
  // not.
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  primeJoined(r, "0000000", "0000000");
  r[1].plat = "esp01";
  ClusterRolloutState st;
  TEST_ASSERT_EQUAL(2, clusterRolloutNextCandidate(t, r, LEADER_REV,
                                                   LEADER_PLAT, st, 1000));
  r[2].plat = "esp01";
  TEST_ASSERT_EQUAL(-1, clusterRolloutNextCandidate(t, r, LEADER_REV,
                                                    LEADER_PLAT, st, 1000));
}

static void test_matching_or_absent_plat_is_a_candidate() {
  // Absent plat = same platform as the leader (pre-#297 S3 fleet, exercised
  // by every other candidate test); an explicit matching plat converges too.
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  primeJoined(r, "0000000", LEADER_REV);
  r[1].plat = "esp32s3";
  ClusterRolloutState st;
  TEST_ASSERT_EQUAL(1, clusterRolloutNextCandidate(t, r, LEADER_REV,
                                                   LEADER_PLAT, st, 1000));
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
  clusterRolloutCheckWait(st, true, String(LEADER_REV), false, LEADER_REV, 210000);
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
  TEST_ASSERT_EQUAL(-1, clusterRolloutNextCandidate(t, r, LEADER_REV, LEADER_PLAT, st,
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
      (int)clusterRolloutCheckWait(st, true, String(LEADER_REV), false, LEADER_REV,
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
                    (int)clusterRolloutCheckWait(st, false, String(""), false,
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
      (int)clusterRolloutCheckWait(st, true, String("0000000"), false, LEADER_REV,
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
                    (int)clusterRolloutCheckWait(st, false, String(""), false,
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

static void test_boundary_is_derived_from_the_image_md5() {
  // #292: a compile-time boundary constant is embedded in the leader's own
  // image (.rodata string literal) — and the image IS the payload, so the
  // follower's parser ends the file field at the first in-body hit. An
  // image cannot contain its own md5, so the boundary derives from it.
  String b = clusterRolloutBoundary("195d92d2328ace62f57bcf5edb0c518d");
  TEST_ASSERT_EQUAL_STRING("sfr-195d92d2328ace62f57bcf5edb0c518d",
                           b.c_str());
  TEST_ASSERT_TRUE(b.length() <= 70);  // RFC 2046 §5.1 boundary cap
}

static void test_multipart_frame_matches_upload_contract() {
  // The follower's /firmware/master parses multipart field "firmware" —
  // the exact strings the streaming client wraps the image in.
  String b = clusterRolloutBoundary("0123456789abcdef0123456789abcdef");
  String pre = clusterRolloutMultipartPreamble(b);
  TEST_ASSERT_TRUE(pre.startsWith("--" + b + "\r\n"));
  TEST_ASSERT_TRUE(pre.indexOf("name=\"firmware\"") >= 0);
  TEST_ASSERT_TRUE(pre.indexOf("filename=") >= 0);
  TEST_ASSERT_TRUE(pre.endsWith("\r\n\r\n"));
  TEST_ASSERT_EQUAL_STRING(("\r\n--" + b + "--\r\n").c_str(),
                           clusterRolloutMultipartTrailer(b).c_str());
  TEST_ASSERT_EQUAL_STRING(("multipart/form-data; boundary=" + b).c_str(),
                           clusterRolloutContentType(b).c_str());
}

static void test_upload_url_carries_md5_and_intended_rev() {
  TEST_ASSERT_EQUAL_STRING(
      "http://192.168.15.91/firmware/master"
      "?md5=0123456789abcdef0123456789abcdef&v=1234abc",
      clusterRolloutUrl("192.168.15.91",
                        "0123456789abcdef0123456789abcdef", "1234abc")
          .c_str());
}

// --- #340 stream hardening -------------------------------------------------------

static void test_rev_change_forgives_blocked_member() {
  ClusterRolloutState st;
  for (int i = 0; i < CLUSTER_ROLLOUT_ATTEMPT_CAP; i++) {
    clusterRolloutStart(st, 1, 1000);
    clusterRolloutUploadFailed(st, 5000);
  }
  TEST_ASSERT_TRUE(st.blocked[1]);
  // The member converged by other means (manual OTA, bench flash): its
  // reported rev moved, so blocked is latched against facts that no longer
  // exist — forgive and let the candidate scan re-decide.
  clusterRolloutNoteMemberRev(st, 1, String("0000000"), String(LEADER_REV));
  TEST_ASSERT_FALSE(st.blocked[1]);
  TEST_ASSERT_EQUAL_UINT8(0, st.attempts[1]);
}

static void test_unknown_to_known_rev_is_not_a_change() {
  // A rejoin after OUR upload goes "" -> rev; forgiving that would defeat
  // the attempt cap (a poisoned image could flash-loop the same board).
  ClusterRolloutState st;
  clusterRolloutStart(st, 1, 1000);
  clusterRolloutUploadFailed(st, 100);
  clusterRolloutNoteMemberRev(st, 1, String(""), String("0000000"));
  TEST_ASSERT_EQUAL_UINT8(1, st.attempts[1]);
}

static void test_same_rev_refresh_changes_nothing() {
  ClusterRolloutState st;
  clusterRolloutStart(st, 1, 1000);
  clusterRolloutUploadFailed(st, 100);
  clusterRolloutNoteMemberRev(st, 1, String("0000000"), String("0000000"));
  TEST_ASSERT_EQUAL_UINT8(1, st.attempts[1]);
}

static void test_note_rev_ignores_out_of_range_index() {
  ClusterRolloutState st;
  clusterRolloutNoteMemberRev(st, -1, String("a"), String("b"));
  clusterRolloutNoteMemberRev(st, CLUSTER_MAX_MEMBERS, String("a"), String("b"));
  for (int i = 0; i < CLUSTER_MAX_MEMBERS; i++) {
    TEST_ASSERT_FALSE(st.blocked[i]);
  }
}

static void test_stream_timeout_sits_between_lan_and_finalize() {
  // Chunk writes must outlast a follower's flash-sector-erase stall (the
  // 1.5 s ping/render bound is too tight — #340) but never exceed the
  // finalize budget that is the design's accepted worst-case stall.
  TEST_ASSERT_GREATER_THAN(1500, CLUSTER_ROLLOUT_STREAM_TIMEOUT_MS);
  TEST_ASSERT_LESS_OR_EQUAL(CLUSTER_ROLLOUT_FINALIZE_TIMEOUT_MS,
                            CLUSTER_ROLLOUT_STREAM_TIMEOUT_MS);
}

// --- #344 esp01 auto-convergence candidate ---------------------------------------

static void test_follower_image_candidate_selects_mismatched_esp01() {
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  primeJoined(r, LEADER_REV, "0000000");
  r[2].plat = "esp01";
  ClusterRolloutState st;
  TEST_ASSERT_EQUAL(2, clusterFollowerImageNextCandidate(t, r, "1111111",
                                                         "esp01", st, 1000));
  // Both directions, mirroring #276: stored == reported → nothing to do.
  TEST_ASSERT_EQUAL(-1, clusterFollowerImageNextCandidate(t, r, "0000000",
                                                          "esp01", st, 1000));
}

static void test_follower_image_candidate_never_picks_non_esp01() {
  // A mismatched S3 member belongs to the #276 rollout — the stored esp01
  // image must never be aimed at it (inverse of the #297 guard).
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  primeJoined(r, "0000000", "0000000");  // no plat = same as leader (S3)
  ClusterRolloutState st;
  TEST_ASSERT_EQUAL(-1, clusterFollowerImageNextCandidate(t, r, "1111111",
                                                          "esp01", st, 1000));
}

static void test_follower_image_candidate_needs_stored_rev() {
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  primeJoined(r, LEADER_REV, "0000000");
  r[2].plat = "esp01";
  ClusterRolloutState st;
  TEST_ASSERT_EQUAL(-1, clusterFollowerImageNextCandidate(t, r, "", "esp01",
                                                          st, 1000));
  TEST_ASSERT_EQUAL(-1, clusterFollowerImageNextCandidate(t, r, nullptr,
                                                          "esp01", st, 1000));
}

static void test_follower_image_candidate_respects_shared_gates() {
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  primeJoined(r, LEADER_REV, "0000000");
  r[2].plat = "esp01";
  ClusterRolloutState st;
  // Shared machine: busy phase, holdoff, blocked and unjoined all gate the
  // esp01 scan exactly like the S3 one.
  st.phase = ClusterRolloutPhase::Uploading;
  TEST_ASSERT_EQUAL(-1, clusterFollowerImageNextCandidate(t, r, "1111111",
                                                          "esp01", st, 1000));
  st.phase = ClusterRolloutPhase::Idle;
  st.holdoffUntilMs = 5000;
  TEST_ASSERT_EQUAL(-1, clusterFollowerImageNextCandidate(t, r, "1111111",
                                                          "esp01", st, 1000));
  st.holdoffUntilMs = 0;
  st.blocked[2] = true;
  TEST_ASSERT_EQUAL(-1, clusterFollowerImageNextCandidate(t, r, "1111111",
                                                          "esp01", st, 1000));
  st.blocked[2] = false;
  r[2].joined = false;
  TEST_ASSERT_EQUAL(-1, clusterFollowerImageNextCandidate(t, r, "1111111",
                                                          "esp01", st, 1000));
}

// --- #343 rescue-beacon convergence ----------------------------------------------

static void test_rescue_member_is_candidate_despite_matching_rev() {
  // A boot-looping esp01 comes up as a rescue beacon and advertises
  // rescue:1 — its flash needs the stored image again EVEN at the same rev.
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  primeJoined(r, LEADER_REV, "1111111");
  r[2].plat = "esp01";
  r[2].rescue = true;
  ClusterRolloutState st;
  TEST_ASSERT_EQUAL(2, clusterFollowerImageNextCandidate(t, r, "1111111",
                                                         "esp01", st, 1000));
  // The flash-loop cap still rules: a blocked member stays blocked even
  // while beaconing (the stored image itself may be the crasher).
  st.blocked[2] = true;
  TEST_ASSERT_EQUAL(-1, clusterFollowerImageNextCandidate(t, r, "1111111",
                                                          "esp01", st, 1000));
}

static void test_rescue_never_unlocks_the_s3_rollout() {
  // rescue is an esp01-image fact — the S3 scan must not adopt it.
  ClusterMemberTable t = makeTable();
  ClusterMemberRuntime r[CLUSTER_MAX_MEMBERS];
  primeJoined(r, LEADER_REV, LEADER_REV);
  r[2].rescue = true;
  ClusterRolloutState st;
  TEST_ASSERT_EQUAL(-1, clusterRolloutNextCandidate(t, r, LEADER_REV,
                                                    LEADER_PLAT, st, 1000));
}

static void test_rescue_rejoin_burns_attempt() {
  // The pushed image booted... straight back into the beacon: the member
  // is still crash-looping, so the attempt burns (cap → blocked protects
  // against an endlessly re-pushed poisoned store image).
  ClusterRolloutState st;
  clusterRolloutStart(st, 1, 1000);
  clusterRolloutUploadDone(st, 10000);
  TEST_ASSERT_EQUAL((int)ClusterRolloutWait::RescueLooping,
                    (int)clusterRolloutCheckWait(st, true, String("1111111"),
                                                 true, "1111111", 20000));
  TEST_ASSERT_EQUAL(ClusterRolloutPhase::Idle, st.phase);
  TEST_ASSERT_EQUAL_UINT8(1, st.attempts[1]);
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
  RUN_TEST(test_foreign_plat_is_never_a_candidate);
  RUN_TEST(test_matching_or_absent_plat_is_a_candidate);
  RUN_TEST(test_start_tracks_progress_target);
  RUN_TEST(test_rejection_burns_no_attempt);
  RUN_TEST(test_every_idle_transition_clears_progress);
  RUN_TEST(test_failure_cap_blocks_member);
  RUN_TEST(test_upload_done_waits_for_rejoin);
  RUN_TEST(test_wait_converged_resets_attempts);
  RUN_TEST(test_wait_still_waiting_while_member_down);
  RUN_TEST(test_wait_rollback_burns_attempt);
  RUN_TEST(test_wait_timeout_burns_attempt);
  RUN_TEST(test_boundary_is_derived_from_the_image_md5);
  RUN_TEST(test_reset_clears_everything);
  RUN_TEST(test_multipart_frame_matches_upload_contract);
  RUN_TEST(test_upload_url_carries_md5_and_intended_rev);
  RUN_TEST(test_rev_change_forgives_blocked_member);
  RUN_TEST(test_unknown_to_known_rev_is_not_a_change);
  RUN_TEST(test_same_rev_refresh_changes_nothing);
  RUN_TEST(test_note_rev_ignores_out_of_range_index);
  RUN_TEST(test_stream_timeout_sits_between_lan_and_finalize);
  RUN_TEST(test_follower_image_candidate_selects_mismatched_esp01);
  RUN_TEST(test_follower_image_candidate_never_picks_non_esp01);
  RUN_TEST(test_follower_image_candidate_needs_stored_rev);
  RUN_TEST(test_follower_image_candidate_respects_shared_gates);
  RUN_TEST(test_rescue_member_is_candidate_despite_matching_rev);
  RUN_TEST(test_rescue_never_unlocks_the_s3_rollout);
  RUN_TEST(test_rescue_rejoin_burns_attempt);
  RUN_TEST(test_phase_names);
  return UNITY_END();
}
