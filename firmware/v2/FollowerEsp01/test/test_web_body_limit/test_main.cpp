// Host-side tests for WebBodyLimit.h (#347) — the pure pre-auth body-size
// decision behind the heap-exhaustion DoS guard. No hardware, no async
// server: the whole policy is three facts in, one bool out.

#include <ArduinoFake.h>
#include <unity.h>

#include "WebBodyLimit.h"

void setUp() {}
void tearDown() {}

// --- small bodies always pass ----------------------------------------------

static void test_small_body_passes_on_any_route() {
  TEST_ASSERT_FALSE(bodyLimitExceeded("/", false, 200));
  TEST_ASSERT_FALSE(bodyLimitExceeded("/cluster/render", false, 64));
  TEST_ASSERT_FALSE(bodyLimitExceeded("/cluster/config", false, 400));
  // Exactly at the ceiling is still allowed.
  TEST_ASSERT_FALSE(bodyLimitExceeded("/", false, kMaxNonUploadBodyBytes));
  // A GET / bodyless POST (length 0) never trips.
  TEST_ASSERT_FALSE(bodyLimitExceeded("/settings", false, 0));
}

// --- oversized non-upload bodies are rejected ------------------------------

static void test_oversized_body_rejected_on_normal_routes() {
  TEST_ASSERT_TRUE(bodyLimitExceeded("/", false, kMaxNonUploadBodyBytes + 1));
  TEST_ASSERT_TRUE(bodyLimitExceeded("/cluster/render", false, 500000));
  TEST_ASSERT_TRUE(bodyLimitExceeded("/cluster/ping", false, 100000));
  TEST_ASSERT_TRUE(bodyLimitExceeded("/wifi/config", false, 8192));
  // Even if a normal route is (spuriously) multipart, it is not an upload
  // route, so a huge body is still rejected.
  TEST_ASSERT_TRUE(bodyLimitExceeded("/cluster/render", true, 500000));
}

// --- genuine multipart uploads pass ----------------------------------------

static void test_large_multipart_upload_passes_on_upload_routes() {
  TEST_ASSERT_FALSE(bodyLimitExceeded("/firmware/master", true, 2000000));
  TEST_ASSERT_FALSE(bodyLimitExceeded("/firmware/rescue", true, 2000000));
  TEST_ASSERT_FALSE(
      bodyLimitExceeded("/cluster/follower-firmware", true, 800000));
}

// --- the bypass: non-multipart body to an upload route is still capped ------

static void test_non_multipart_body_to_upload_route_is_capped() {
  // An attacker POSTs a huge urlencoded body to /firmware/master (which the
  // real handler only reads as multipart) — must NOT slip past the cap.
  TEST_ASSERT_TRUE(bodyLimitExceeded("/firmware/master", false, 500000));
  TEST_ASSERT_TRUE(bodyLimitExceeded("/cluster/follower-firmware", false,
                                     500000));
  // ...but a small non-multipart body to the same route is fine (bodyless
  // race POSTs the real handlers already tolerate).
  TEST_ASSERT_FALSE(bodyLimitExceeded("/firmware/master", false, 0));
}

// --- #386: /cluster/ping carries the digest piggyback and needs headroom -----

static void test_ping_route_has_its_own_higher_ceiling() {
  // The #294 digest piggyback pushed a 3-member ping body to ~2137 B, which
  // the flat 2048 B ceiling 413'd before the handler ran — every ping failed,
  // so contact aged to the degrade bar and the whole wall cycled
  // joined -> DEGRADED -> re-join every ~46 s.
  TEST_ASSERT_FALSE(bodyLimitExceeded("/cluster/ping", false, 2137));
  TEST_ASSERT_FALSE(
      bodyLimitExceeded("/cluster/ping", false, kMaxPingBodyBytes));
  // ...but the ping stays bounded — this is still a pre-auth DoS guard.
  TEST_ASSERT_TRUE(
      bodyLimitExceeded("/cluster/ping", false, kMaxPingBodyBytes + 1));
  // Every other cluster route keeps the tight ceiling.
  TEST_ASSERT_TRUE(bodyLimitExceeded("/cluster/render", false, 2137));
  TEST_ASSERT_TRUE(bodyLimitExceeded("/cluster/join", false, 2137));
  TEST_ASSERT_TRUE(bodyLimitExceeded("/cluster/config", false, 2137));
  // The relaxation is real but modest: the ESP-01 follower must still be able
  // to buffer it (~18 KB free heap in practice).
  TEST_ASSERT_TRUE(kMaxPingBodyBytes > kMaxNonUploadBodyBytes);
  TEST_ASSERT_TRUE(kMaxPingBodyBytes <= 8192);
}

// --- #386: leader-side digest budget ----------------------------------------

static void test_digest_budget_drops_oversized_digest() {
  // The digest is an OPTIONAL piggyback (#294 — absent ⇒ ""); the liveness
  // ping is not. A digest that would not fit is dropped, never allowed to
  // 413 the ping that carries it.
  TEST_ASSERT_TRUE(pingBodyDigestFits(1200, 128));
  TEST_ASSERT_TRUE(pingBodyDigestFits(kMaxPingBodyBytes - 128, 128));
  TEST_ASSERT_FALSE(pingBodyDigestFits(kMaxPingBodyBytes - 128 + 1, 128));
  // An 8-member wall's encoded digest degrades gracefully instead of wedging
  // liveness for the entire cluster.
  TEST_ASSERT_FALSE(pingBodyDigestFits(7000, 128));
  // Overhead is counted, not ignored: a digest that only fits when the
  // signature is forgotten must NOT be considered fitting.
  TEST_ASSERT_FALSE(pingBodyDigestFits(kMaxPingBodyBytes, 1));
}

// --- allowlist membership ---------------------------------------------------

static void test_upload_allowlist_membership() {
  TEST_ASSERT_TRUE(bodyLimitUrlIsUpload("/firmware/master"));
  TEST_ASSERT_TRUE(bodyLimitUrlIsUpload("/firmware/rescue"));
  TEST_ASSERT_TRUE(bodyLimitUrlIsUpload("/cluster/follower-firmware"));
  TEST_ASSERT_FALSE(bodyLimitUrlIsUpload("/firmware/master/extra"));
  TEST_ASSERT_FALSE(bodyLimitUrlIsUpload("/"));
  TEST_ASSERT_FALSE(bodyLimitUrlIsUpload("/cluster/render"));
  TEST_ASSERT_FALSE(bodyLimitUrlIsUpload(nullptr));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_small_body_passes_on_any_route);
  RUN_TEST(test_oversized_body_rejected_on_normal_routes);
  RUN_TEST(test_large_multipart_upload_passes_on_upload_routes);
  RUN_TEST(test_non_multipart_body_to_upload_route_is_capped);
  RUN_TEST(test_ping_route_has_its_own_higher_ceiling);
  RUN_TEST(test_digest_budget_drops_oversized_digest);
  RUN_TEST(test_upload_allowlist_membership);
  return UNITY_END();
}
