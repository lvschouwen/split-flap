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

static void test_same_origin() {
  TEST_ASSERT_TRUE(lanSameOrigin("http://192.168.15.90", "192.168.15.90"));
  TEST_ASSERT_TRUE(lanSameOrigin("http://split-flap.local", "Split-Flap.LOCAL"));
  TEST_ASSERT_TRUE(lanSameOrigin("http://10.1.2.3:8080", "10.1.2.3:8080"));
  // The default port may be written out on either side.
  TEST_ASSERT_TRUE(lanSameOrigin("http://192.168.4.1:80", "192.168.4.1"));
  TEST_ASSERT_TRUE(lanSameOrigin("http://192.168.4.1", "192.168.4.1:80"));
  // Another board, another port, a longer or shorter name: not this page.
  TEST_ASSERT_FALSE(lanSameOrigin("http://192.168.15.91", "192.168.15.90"));
  TEST_ASSERT_FALSE(lanSameOrigin("http://10.1.2.3:8080", "10.1.2.3"));
  TEST_ASSERT_FALSE(lanSameOrigin("http://192.168.15.9", "192.168.15.90"));
  TEST_ASSERT_FALSE(lanSameOrigin("http://192.168.15.90", ""));
  TEST_ASSERT_FALSE(lanSameOrigin("null", "null"));
  TEST_ASSERT_FALSE(lanSameOrigin("https://192.168.15.90", "192.168.15.90"));
}

static void test_csrf_reject() {
  // No Origin (curl / ota-flash.sh / the master) always passes.
  TEST_ASSERT_FALSE(lanCsrfReject(true, false, "", "192.168.4.1"));
  // The board's own page passes.
  TEST_ASSERT_FALSE(lanCsrfReject(true, true, "http://192.168.4.1", "192.168.4.1"));
  // A website is refused, and so is a page served by another LAN host.
  TEST_ASSERT_TRUE(lanCsrfReject(true, true, "https://evil.example.com", "192.168.4.1"));
  TEST_ASSERT_TRUE(lanCsrfReject(true, true, "http://192.168.15.7", "192.168.4.1"));
  TEST_ASSERT_TRUE(lanCsrfReject(true, true, "http://192.168.4.1", ""));
  // A public name pointed at the board's address (DNS rebinding) is its own
  // origin, but not a LAN one.
  TEST_ASSERT_TRUE(lanCsrfReject(true, true, "http://evil.example.com", "evil.example.com"));
  // A request that changes nothing is never the gate's business.
  TEST_ASSERT_FALSE(lanCsrfReject(false, true, "https://evil.example.com", "192.168.4.1"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_lan_origins_allowed);
  RUN_TEST(test_foreign_origins_rejected);
  RUN_TEST(test_same_origin);
  RUN_TEST(test_csrf_reject);
  return UNITY_END();
}
