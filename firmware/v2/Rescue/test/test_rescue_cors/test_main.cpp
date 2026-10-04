// Host-side tests for RescueCors.h (#349) — the CSRF origin gate on the
// rescue app's mutating POSTs. Vector set mirrors the follower copy's
// (test_follower_json) so the copied origin logic can't drift silently.

#include <ArduinoFake.h>
#include <unity.h>

#include "LanOrigin.h"

void setUp() {}
void tearDown() {}

static void test_lan_origins_allowed() {
  TEST_ASSERT_TRUE(lanOriginAllowed("http://192.168.15.90"));
  TEST_ASSERT_TRUE(lanOriginAllowed("http://192.168.4.1"));  // AP portal
  TEST_ASSERT_TRUE(lanOriginAllowed("http://10.1.2.3:8080"));
  TEST_ASSERT_TRUE(lanOriginAllowed("http://172.16.0.9"));
  TEST_ASSERT_TRUE(lanOriginAllowed("http://split-flap.local"));
  TEST_ASSERT_TRUE(lanOriginAllowed("http://localhost:8000"));
}

static void test_foreign_origins_rejected() {
  TEST_ASSERT_FALSE(lanOriginAllowed("https://192.168.15.90"));
  TEST_ASSERT_FALSE(lanOriginAllowed("http://8.8.8.8"));
  TEST_ASSERT_FALSE(lanOriginAllowed("http://evil.example.com"));
  TEST_ASSERT_FALSE(lanOriginAllowed("http://172.32.0.1"));
  TEST_ASSERT_FALSE(lanOriginAllowed(""));
  TEST_ASSERT_FALSE(lanOriginAllowed("null"));
}

static void test_csrf_reject_post() {
  // No Origin (curl / ota-flash.sh / same-origin fetch) always passes.
  TEST_ASSERT_FALSE(lanCsrfRejectPost(true, false, ""));
  // LAN origin passes; foreign origin on a POST is refused.
  TEST_ASSERT_FALSE(lanCsrfRejectPost(true, true, "http://192.168.4.1"));
  TEST_ASSERT_TRUE(lanCsrfRejectPost(true, true, "https://evil.example.com"));
  // Non-POST is never the CSRF gate's business.
  TEST_ASSERT_FALSE(lanCsrfRejectPost(false, true, "https://evil.example.com"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_lan_origins_allowed);
  RUN_TEST(test_foreign_origins_rejected);
  RUN_TEST(test_csrf_reject_post);
  return UNITY_END();
}
