// Native tests for FollowerUpdatePolicy.h: when a row takes the image its
// master offers, and what it requires of the download before and after
// anything is written to flash.
#include <unity.h>

#include "FollowerUpdatePolicy.h"

void setUp() {}
void tearDown() {}

static const uint32_t SPACE = 540672;  // free app area on the wall's row today

static wl_Update offer(const char* rev = "abc1234", uint32_t size = 340000) {
  wl_Update u = wl_Update_init_zero;
  strcpy(u.rev, rev);
  u.size = size;
  for (uint8_t i = 0; i < 16; i++) u.md5[i] = (uint8_t)(i + 1);
  u.packed = true;
  return u;
}

static wl_UpdateReason admit(const wl_Update& u, bool rescue = false, bool unitsBusy = false) {
  return followerUpdateAdmit(u, "3f1a516", rescue, unitsBusy, SPACE);
}

static void test_another_rev_is_taken() {
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_REASON_NONE, admit(offer()));
}

static void test_the_running_rev_is_not_installed_again() {
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_CURRENT, admit(offer("3f1a516")));
}

static void test_rescue_mode_takes_the_rev_it_already_runs() {
  // Rescue mode ends only with an installed image; the master may have no other.
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_REASON_NONE, admit(offer("3f1a516"), true));
}

static void test_a_unit_job_is_never_cut_short_by_the_restart() {
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_UNITS_BUSY, admit(offer(), false, true));
}

static void test_an_offer_without_rev_size_or_md5_is_refused() {
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_BAD_OFFER, admit(offer("")));
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_BAD_OFFER, admit(offer("abc1234", 0)));
  wl_Update u = offer();
  memset(u.md5, 0, sizeof(u.md5));
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_BAD_OFFER, admit(u));
}

static void test_an_image_larger_than_the_free_area_is_refused_before_the_download() {
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_REASON_NONE, admit(offer("abc1234", SPACE)));
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_TOO_LARGE, admit(offer("abc1234", SPACE + 1)));
}

static void test_the_storable_size_leaves_the_sector_the_updater_keeps() {
  TEST_ASSERT_EQUAL_UINT32(0, followerUpdateMaxSpace(0));
  TEST_ASSERT_EQUAL_UINT32(0, followerUpdateMaxSpace(0x0FFF));
  TEST_ASSERT_EQUAL_UINT32(0, followerUpdateMaxSpace(0x1000));
  TEST_ASSERT_EQUAL_UINT32(0x84000, followerUpdateMaxSpace(0x85000));
  TEST_ASSERT_EQUAL_UINT32(0x84000, followerUpdateMaxSpace(0x85FFF));
}

static void test_md5_bytes_become_the_hex_the_updater_takes() {
  const uint8_t md5[16] = {0x00, 0x01, 0x9a, 0xbc, 0xde, 0xf0, 0x10, 0x20,
                           0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xff};
  char hex[33];
  followerUpdateMd5Hex(md5, hex);
  TEST_ASSERT_EQUAL_STRING("00019abcdef0102030405060708090ff", hex);
}

static void test_the_port_defaults_to_80() {
  TEST_ASSERT_EQUAL_UINT16(80, followerUpdatePort(0));
  TEST_ASSERT_EQUAL_UINT16(8080, followerUpdatePort(8080));
  TEST_ASSERT_EQUAL_UINT16(80, followerUpdatePort(70000));  // not a port
}

static void test_only_a_200_with_the_offered_length_is_downloaded() {
  uint32_t detail = 0;
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_REASON_NONE,
                    followerUpdateAnswer(200, 340000, 340000, detail));
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_UNREACHABLE,
                    followerUpdateAnswer(-1, 0, 340000, detail));
  TEST_ASSERT_EQUAL_UINT32(1, detail);
  // The client's own error says where it stopped: -11 = no answer in time.
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_UNREACHABLE,
                    followerUpdateAnswer(-11, 0, 340000, detail));
  TEST_ASSERT_EQUAL_UINT32(11, detail);
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_HTTP_STATUS,
                    followerUpdateAnswer(404, 9, 340000, detail));
  TEST_ASSERT_EQUAL_UINT32(404, detail);
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_SIZE_DIFFERS,
                    followerUpdateAnswer(200, 339999, 340000, detail));
  TEST_ASSERT_EQUAL_UINT32(339999, detail);
  // No Content-Length (chunked): the length cannot be checked before flash is erased.
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_SIZE_DIFFERS,
                    followerUpdateAnswer(200, -1, 340000, detail));
  TEST_ASSERT_EQUAL_UINT32(0, detail);
}

// The first bytes of the build's packed image: gzip header, one stored block
// of 16 bytes, then the image header (magic, segments, flash mode, size/freq).
static const uint8_t PACKED[] = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 2, 3,
                                 0x00, 16, 0, 0xEF, 0xFF, 0xE9, 3, 3, 0x20};
static const uint8_t RUNNING[4] = {0xE9, 1, 3, 0x20};

static void test_a_packed_image_must_show_the_flash_config_this_board_runs() {
  uint32_t detail = 0;
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_REASON_NONE,
                    followerUpdateFirstBytes(PACKED, sizeof PACKED, true, RUNNING, detail));
  uint8_t otherMode[sizeof PACKED];
  memcpy(otherMode, PACKED, sizeof PACKED);
  otherMode[17] = 0;  // QIO
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_IMAGE,
                    followerUpdateFirstBytes(otherMode, sizeof otherMode, true, RUNNING, detail));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)OtaImageCheck::FlashMode, detail);
}

static void test_the_file_must_be_what_the_offer_said() {
  uint32_t detail = 0;
  const uint8_t plain[] = {0xE9, 1, 3, 0x20, 0, 0, 0, 0};
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_REASON_NONE,
                    followerUpdateFirstBytes(plain, sizeof plain, false, RUNNING, detail));
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_IMAGE,
                    followerUpdateFirstBytes(plain, sizeof plain, true, RUNNING, detail));
  TEST_ASSERT_EQUAL_UINT32(FOLLOWER_UPDATE_DETAIL_KIND, detail);
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_IMAGE,
                    followerUpdateFirstBytes(PACKED, sizeof PACKED, false, RUNNING, detail));
  TEST_ASSERT_EQUAL_UINT32(FOLLOWER_UPDATE_DETAIL_KIND, detail);
  // A web page answered in place of an image.
  const uint8_t html[] = {'<', 'h', 't', 'm', 'l', '>', ' ', ' '};
  TEST_ASSERT_EQUAL(wl_UpdateReason_UPDATE_IMAGE,
                    followerUpdateFirstBytes(html, sizeof html, false, RUNNING, detail));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)OtaImageCheck::NotAnImage, detail);
}

static void test_how_many_first_bytes_the_checks_need() {
  TEST_ASSERT_EQUAL_UINT32(19, followerUpdateFirstLen(340000));
  TEST_ASSERT_EQUAL_UINT32(7, followerUpdateFirstLen(7));
  TEST_ASSERT_TRUE(followerUpdateFirstLen(340000) <= FOLLOWER_UPDATE_CHUNK);
}

static void test_a_silent_or_endless_download_is_given_up() {
  TEST_ASSERT_FALSE(followerUpdateGiveUp(20000, 15000, 0));
  TEST_ASSERT_TRUE(followerUpdateGiveUp(25001, 15000, 0));  // 10 s without a byte
  TEST_ASSERT_FALSE(followerUpdateGiveUp(FOLLOWER_UPDATE_TOTAL_MS, FOLLOWER_UPDATE_TOTAL_MS - 5, 0));
  TEST_ASSERT_TRUE(
      followerUpdateGiveUp(FOLLOWER_UPDATE_TOTAL_MS + 1, FOLLOWER_UPDATE_TOTAL_MS - 5, 0));
  // Across the wrap of the millisecond counter.
  TEST_ASSERT_FALSE(followerUpdateGiveUp(500, 0xFFFFFF00u, 0xFFFFF000u));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_another_rev_is_taken);
  RUN_TEST(test_the_running_rev_is_not_installed_again);
  RUN_TEST(test_rescue_mode_takes_the_rev_it_already_runs);
  RUN_TEST(test_a_unit_job_is_never_cut_short_by_the_restart);
  RUN_TEST(test_an_offer_without_rev_size_or_md5_is_refused);
  RUN_TEST(test_an_image_larger_than_the_free_area_is_refused_before_the_download);
  RUN_TEST(test_the_storable_size_leaves_the_sector_the_updater_keeps);
  RUN_TEST(test_md5_bytes_become_the_hex_the_updater_takes);
  RUN_TEST(test_the_port_defaults_to_80);
  RUN_TEST(test_only_a_200_with_the_offered_length_is_downloaded);
  RUN_TEST(test_a_packed_image_must_show_the_flash_config_this_board_runs);
  RUN_TEST(test_the_file_must_be_what_the_offer_said);
  RUN_TEST(test_how_many_first_bytes_the_checks_need);
  RUN_TEST(test_a_silent_or_endless_download_is_given_up);
  return UNITY_END();
}
