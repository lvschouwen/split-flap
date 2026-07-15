// Host-side tests for the ESP-01 follower-image relay policy (#304 Part B):
// the upload filename/prefix guard (mirrors ota-flash.sh #299), the
// on-demand push eligibility matrix (esp01 target + a stored image), the
// PSRAM accumulator cursor/bounds check, and the one-shot push phase machine.

#include <unity.h>

#include "../../FollowerImagePolicy.h"

void setUp() {}
void tearDown() {}

// --- upload filename guard ----------------------------------------------------------

static void test_upload_accepts_follower_bin_and_extracts_rev() {
  String rev;
  TEST_ASSERT_TRUE(followerImageUploadAccepts("follower-abc1234.bin", rev));
  TEST_ASSERT_EQUAL_STRING("abc1234", rev.c_str());
}

static void test_upload_accepts_dirty_and_size_suffix() {
  String rev;
  TEST_ASSERT_TRUE(followerImageUploadAccepts("follower-abc1234-dirty.bin", rev));
  TEST_ASSERT_EQUAL_STRING("abc1234-dirty", rev.c_str());
  TEST_ASSERT_TRUE(followerImageUploadAccepts("follower-deadbee-1m.bin", rev));
  TEST_ASSERT_EQUAL_STRING("deadbee", rev.c_str());
}

static void test_upload_rejects_s3_and_garbage_names() {
  String rev;
  // An S3 image would brick the ESP-01 — the whole point of the guard.
  TEST_ASSERT_FALSE(followerImageUploadAccepts("firmware-abc1234-master.bin", rev));
  TEST_ASSERT_FALSE(followerImageUploadAccepts("evil.bin", rev));
  TEST_ASSERT_FALSE(followerImageUploadAccepts("follower-.bin", rev));
  TEST_ASSERT_FALSE(followerImageUploadAccepts("follower-abc1234.hex", rev));
  TEST_ASSERT_FALSE(followerImageUploadAccepts("follower-ABC.bin", rev));  // non-hex rev
}

// --- push eligibility ---------------------------------------------------------------

static void test_eligibility_requires_esp01_target() {
  TEST_ASSERT_EQUAL(FollowerPushEligibility::Eligible,
                    followerPushEligibility("esp01", true));
  // An S3 member converges via #276; never stream the follower image at it.
  TEST_ASSERT_EQUAL(FollowerPushEligibility::NotEsp01,
                    followerPushEligibility("esp32s3", true));
  // Absent plat = same platform as the leader (an S3) — not a follower.
  TEST_ASSERT_EQUAL(FollowerPushEligibility::NotEsp01,
                    followerPushEligibility("", true));
}

static void test_eligibility_requires_stored_image() {
  TEST_ASSERT_EQUAL(FollowerPushEligibility::NoStoredImage,
                    followerPushEligibility("esp01", false));
}

// --- PSRAM accumulator cursor / bounds ----------------------------------------------

static void test_chunk_ok_sequential_appends() {
  TEST_ASSERT_TRUE(followerImageChunkOk(0, 0, 100, 1000));
  TEST_ASSERT_TRUE(followerImageChunkOk(100, 100, 100, 1000));
  TEST_ASSERT_TRUE(followerImageChunkOk(900, 900, 100, 1000));  // fills exactly
}

static void test_chunk_rejects_cursor_gap_and_overflow() {
  // index must equal bytes already accumulated (no gaps/rewinds — v1 #191
  // anti-corruption, mirrors FactoryChunkPlan).
  TEST_ASSERT_FALSE(followerImageChunkOk(50, 100, 10, 1000));
  TEST_ASSERT_FALSE(followerImageChunkOk(200, 100, 10, 1000));
  // would exceed the PSRAM buffer.
  TEST_ASSERT_FALSE(followerImageChunkOk(0, 0, 2000, 1000));
  TEST_ASSERT_FALSE(followerImageChunkOk(950, 950, 100, 1000));
}

// --- one-shot push phase machine ----------------------------------------------------

static void test_push_lifecycle_idle_uploading_done() {
  FollowerPushState st;
  TEST_ASSERT_EQUAL(FollowerPushPhase::Idle, st.phase);
  TEST_ASSERT_FALSE(followerPushActive(st));

  followerPushStart(st, 2, 384000);
  TEST_ASSERT_EQUAL(FollowerPushPhase::Uploading, st.phase);
  TEST_ASSERT_EQUAL_INT(2, st.memberIndex);
  TEST_ASSERT_EQUAL_UINT32(384000, st.bytesTotal);
  TEST_ASSERT_TRUE(followerPushActive(st));

  followerPushProgress(st, 49152);
  TEST_ASSERT_EQUAL_UINT32(49152, st.bytesSent);

  followerPushFinish(st, FollowerPushResult::Done);
  TEST_ASSERT_EQUAL(FollowerPushPhase::Idle, st.phase);
  TEST_ASSERT_EQUAL(FollowerPushResult::Done, st.lastResult);
  TEST_ASSERT_EQUAL_INT(-1, st.memberIndex);
  TEST_ASSERT_EQUAL_UINT32(0, st.bytesSent);  // progress cleared at idle
  TEST_ASSERT_EQUAL_UINT32(0, st.bytesTotal);
  TEST_ASSERT_FALSE(followerPushActive(st));
}

static void test_push_finish_failed_records_result() {
  FollowerPushState st;
  followerPushStart(st, 0, 100);
  followerPushFinish(st, FollowerPushResult::Failed);
  TEST_ASSERT_EQUAL(FollowerPushPhase::Idle, st.phase);
  TEST_ASSERT_EQUAL(FollowerPushResult::Failed, st.lastResult);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_upload_accepts_follower_bin_and_extracts_rev);
  RUN_TEST(test_upload_accepts_dirty_and_size_suffix);
  RUN_TEST(test_upload_rejects_s3_and_garbage_names);
  RUN_TEST(test_eligibility_requires_esp01_target);
  RUN_TEST(test_eligibility_requires_stored_image);
  RUN_TEST(test_chunk_ok_sequential_appends);
  RUN_TEST(test_chunk_rejects_cursor_gap_and_overflow);
  RUN_TEST(test_push_lifecycle_idle_uploading_done);
  RUN_TEST(test_push_finish_failed_records_result);
  return UNITY_END();
}
