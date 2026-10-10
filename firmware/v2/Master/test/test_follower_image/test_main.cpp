// Host-side tests for the stored row image's rules: the upload
// filename/prefix guard (mirrors ota-flash.sh #299) and the PSRAM
// accumulator cursor/bounds check.

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

// --- the hold on an image a release stored ------------------------------------------

static void test_an_uploaded_image_is_never_held() {
  TEST_ASSERT_FALSE(followerImageHeld("", "abc1234"));
  TEST_ASSERT_FALSE(followerImageHeld(nullptr, "abc1234"));
}

static void test_a_release_image_is_held_until_the_master_runs_that_release() {
  TEST_ASSERT_TRUE(followerImageHeld("def5678", "abc1234"));
  TEST_ASSERT_FALSE(followerImageHeld("def5678", "def5678"));
  // A master that fell back runs its old rev again: still held.
  TEST_ASSERT_TRUE(followerImageHeld("def5678", "abc1234"));
  // A build with changes on top of the release is not the release.
  TEST_ASSERT_TRUE(followerImageHeld("def5678", "def5678-dirty"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_upload_accepts_follower_bin_and_extracts_rev);
  RUN_TEST(test_upload_accepts_dirty_and_size_suffix);
  RUN_TEST(test_upload_rejects_s3_and_garbage_names);
  RUN_TEST(test_chunk_ok_sequential_appends);
  RUN_TEST(test_chunk_rejects_cursor_gap_and_overflow);
  RUN_TEST(test_an_uploaded_image_is_never_held);
  RUN_TEST(test_a_release_image_is_held_until_the_master_runs_that_release);
  return UNITY_END();
}
