// Host-side tests for the one JSON document the row serves itself: its
// identity (GET /settings, the answer to POST /pair).

#include <ArduinoFake.h>
#include <unity.h>

#include "../../FollowerJson.h"
#include "LanOrigin.h"

void setUp() {}
void tearDown() {}

static FollowerIdentity makeIdentity() {
  FollowerIdentity id;
  id.name = "split-flap-ab12cd";
  id.rev = "abc1234";
  id.width = 5;
  id.masterId = "split-flap-master";
  id.masterHost = "192.168.15.22";
  id.linked = true;
  id.upSeconds = 1200;
  id.heapBytes = 28000;
  id.sketchBytes = 458752;
  id.sketchFreeBytes = 569344;
  id.flashMode = 3;
  id.flashId = 0x1440E0;
  return id;
}

static void test_identity_json_shape() {
  TEST_ASSERT_EQUAL_STRING(
      "{\"name\":\"split-flap-ab12cd\",\"version\":\"abc1234\",\"plat\":\"esp01\","
      "\"width\":5,\"rescue\":false,\"master\":\"split-flap-master\","
      "\"masterHost\":\"192.168.15.22\",\"linked\":true,\"up\":1200,"
      "\"heap\":28000,\"sketch\":458752,\"sketchFree\":569344,\"flashMode\":3,"
      "\"flashId\":\"1440e0\"}",
      followerIdentityJson(makeIdentity()).c_str());
}

static void test_identity_carries_what_ota_flash_reads() {
  // ota-flash.sh keys the platform on `plat` and compares `version` before
  // and after an upload.
  const String out = followerIdentityJson(makeIdentity());
  TEST_ASSERT_TRUE(out.indexOf("\"version\":\"abc1234\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"plat\":\"esp01\"") >= 0);
}

static void test_unpaired_rescue_row() {
  FollowerIdentity id = makeIdentity();
  id.masterId = "";
  id.masterHost = "";
  id.linked = false;
  id.rescue = true;
  const String out = followerIdentityJson(id);
  TEST_ASSERT_TRUE(out.indexOf("\"rescue\":true") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"master\":\"\",\"masterHost\":\"\",\"linked\":false") >= 0);
}

static void test_stored_strings_are_escaped() {
  // The master's id came from another board: it is served back escaped.
  FollowerIdentity id = makeIdentity();
  id.masterId = "a\"b\\c";
  const String out = followerIdentityJson(id);
  TEST_ASSERT_TRUE(out.indexOf("\"master\":\"a\\\"b\\\\c\"") >= 0);
}

static void test_identity_fits_its_reserve() {
  // Longest name, rev, master id and host, widest numbers: one allocation.
  FollowerIdentity id;
  id.name = "split-flap-ffffffff";
  id.rev = "0123456789abcdef";
  id.width = 16;
  id.rescue = true;
  id.masterId = "0123456789abcdef0123456789abcdef";
  id.masterHost = "0123456789abcdef0123456789abcdef01234567";
  id.linked = false;
  id.upSeconds = 0xFFFFFFFFUL;
  id.heapBytes = 0xFFFFFFFFUL;
  id.sketchBytes = 0xFFFFFFFFUL;
  id.sketchFreeBytes = 0xFFFFFFFFUL;
  id.flashMode = 255;
  id.flashId = 0xFFFFFFFFUL;
  TEST_ASSERT_TRUE(followerIdentityJson(id).length() <= 352);
}

static void test_post_from_a_website_is_refused() {
  // The rule POST /pair and the upload apply (shared LanOrigin.h): a POST
  // with a public or https origin is a website driving the owner's browser.
  TEST_ASSERT_TRUE(lanCsrfRejectPost(true, true, "http://evil.example.com"));
  TEST_ASSERT_TRUE(lanCsrfRejectPost(true, true, "https://192.168.15.90"));
  // A master and ota-flash.sh send no Origin.
  TEST_ASSERT_FALSE(lanCsrfRejectPost(true, false, ""));
  TEST_ASSERT_FALSE(lanCsrfRejectPost(true, true, "http://192.168.15.90"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_identity_json_shape);
  RUN_TEST(test_identity_carries_what_ota_flash_reads);
  RUN_TEST(test_unpaired_rescue_row);
  RUN_TEST(test_stored_strings_are_escaped);
  RUN_TEST(test_identity_fits_its_reserve);
  RUN_TEST(test_post_from_a_website_is_refused);
  return UNITY_END();
}
