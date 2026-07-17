// Native tests for ClusterHmac.h — the cluster-wire authentication layer.
// Known-answer vectors (FIPS 180-4 SHA-256, RFC 4231 HMAC-SHA256) prove the
// portable crypto matches the standard byte-for-byte, then sign/verify/replay
// behavior is exercised on top.

#include <unity.h>

#include "../../ClusterHmac.h"

static String sha256Hex(const String& s) {
  uint8_t out[32];
  clusterSha256((const uint8_t*)s.c_str(), s.length(), out);
  return clusterBytesToHex(out, 32);
}

// Convenience for the tests that exercise mac/window behavior, not the
// monotonic guard: a fresh high-water mark each call (starts at 0).
static bool accept(const uint8_t* key, const String& msg, uint64_t ts,
                   const String& mac, uint64_t now, bool synced) {
  uint64_t last = 0;
  return clusterHmacAccept(key, msg, ts, mac, now, synced, last);
}

static void test_sha256_known_answers() {
  // FIPS 180-4 examples.
  TEST_ASSERT_EQUAL_STRING(
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      sha256Hex("abc").c_str());
  TEST_ASSERT_EQUAL_STRING(
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
      sha256Hex("").c_str());
}

static void test_sha256_multiblock() {
  // 56-byte input forces the two-block padding path.
  String in = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  TEST_ASSERT_EQUAL_STRING(
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
      sha256Hex(in).c_str());
}

static void test_hmac_sha256_rfc4231_case2() {
  // RFC 4231 test case 2: key "Jefe", data "what do ya want for nothing?".
  const char* key = "Jefe";
  const char* data = "what do ya want for nothing?";
  uint8_t mac[32];
  clusterHmacSha256((const uint8_t*)key, 4, (const uint8_t*)data, strlen(data),
                    mac);
  TEST_ASSERT_EQUAL_STRING(
      "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
      clusterBytesToHex(mac, 32).c_str());
}

static void test_hmac_long_key_is_hashed() {
  // RFC 4231 test case 5-ish: a >64-byte key must be pre-hashed. Just check it
  // runs and differs from the truncated-key result.
  uint8_t longKey[100];
  for (int i = 0; i < 100; i++) longKey[i] = (uint8_t)i;
  uint8_t mac[32];
  clusterHmacSha256(longKey, 100, (const uint8_t*)"x", 1, mac);
  uint8_t mac64[32];
  clusterHmacSha256(longKey, 64, (const uint8_t*)"x", 1, mac64);
  TEST_ASSERT_FALSE(clusterBytesToHex(mac, 32) == clusterBytesToHex(mac64, 32));
}

static void test_key_hex_round_trip() {
  uint8_t key[CLUSTER_HMAC_KEY_LEN];
  for (int i = 0; i < CLUSTER_HMAC_KEY_LEN; i++) key[i] = (uint8_t)(i * 7 + 1);
  String hex = clusterKeyToHex(key);
  TEST_ASSERT_EQUAL(64, (int)hex.length());
  uint8_t back[CLUSTER_HMAC_KEY_LEN];
  TEST_ASSERT_TRUE(clusterKeyFromHex(hex, back));
  TEST_ASSERT_EQUAL_MEMORY(key, back, CLUSTER_HMAC_KEY_LEN);
}

static void test_key_from_hex_rejects_bad_input() {
  uint8_t key[CLUSTER_HMAC_KEY_LEN];
  TEST_ASSERT_FALSE(clusterKeyFromHex("abcd", key));          // too short
  String tooLong;
  for (int i = 0; i < 66; i++) tooLong += 'a';
  TEST_ASSERT_FALSE(clusterKeyFromHex(tooLong, key));         // too long
  String withZ;
  for (int i = 0; i < 63; i++) withZ += 'a';
  withZ += 'z';
  TEST_ASSERT_FALSE(clusterKeyFromHex(withZ, key));           // non-hex char
}

static void test_canonical_messages_are_stable() {
  TEST_ASSERT_EQUAL_STRING(
      "render\n1720000000000\n3\n42\nHELLO\n80\n1720000000400",
      clusterHmacRenderMsg(1720000000000ULL, 3, 42, "HELLO", 80,
                           1720000000400ULL)
          .c_str());
  // Ping binds ts + sha256(digest) + you (#313 follow-on HIGH#2). Empty
  // digest ⇒ sha256("") and you=-1, matching the leader's digest-less case.
  TEST_ASSERT_EQUAL_STRING(
      "ping\n1720000000000\n"
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n-1",
      clusterHmacPingMsg(1720000000000ULL, "", -1).c_str());
  TEST_ASSERT_EQUAL_STRING("leave\n1720000000000",
                           clusterHmacLeaveMsg(1720000000000ULL).c_str());
}

static void makeKey(uint8_t key[CLUSTER_HMAC_KEY_LEN], uint8_t seed) {
  for (int i = 0; i < CLUSTER_HMAC_KEY_LEN; i++) key[i] = (uint8_t)(seed + i);
}

static void test_sign_then_accept_round_trip() {
  uint8_t key[CLUSTER_HMAC_KEY_LEN];
  makeKey(key, 5);
  uint64_t ts = 1720000000000ULL;
  String msg = clusterHmacRenderMsg(ts, 1, 7, "ABCD", 80, ts + 400);
  String mac = clusterHmacSign(key, msg);
  // Same-second verify, synced.
  TEST_ASSERT_TRUE(accept(key, msg, ts, mac, ts + 500, true));
}

static void test_accept_rejects_wrong_key() {
  uint8_t key[CLUSTER_HMAC_KEY_LEN], other[CLUSTER_HMAC_KEY_LEN];
  makeKey(key, 5);
  makeKey(other, 99);
  uint64_t ts = 1720000000000ULL;
  String msg = clusterHmacPingMsg(ts, "", -1);
  String mac = clusterHmacSign(key, msg);
  TEST_ASSERT_FALSE(accept(other, msg, ts, mac, ts, true));
}

static void test_accept_rejects_tampered_content() {
  uint8_t key[CLUSTER_HMAC_KEY_LEN];
  makeKey(key, 5);
  uint64_t ts = 1720000000000ULL;
  String signed_msg = clusterHmacRenderMsg(ts, 1, 7, "ABCD", 80, ts + 400);
  String mac = clusterHmacSign(key, signed_msg);
  // Attacker swaps the text under the captured mac.
  String tampered = clusterHmacRenderMsg(ts, 1, 7, "EVIL", 80, ts + 400);
  TEST_ASSERT_FALSE(accept(key, tampered, ts, mac, ts, true));
}

static void test_accept_rejects_replay_outside_window_when_synced() {
  uint8_t key[CLUSTER_HMAC_KEY_LEN];
  makeKey(key, 5);
  uint64_t ts = 1720000000000ULL;
  String msg = clusterHmacPingMsg(ts, "", -1);
  String mac = clusterHmacSign(key, msg);
  // 31 s later, synced → outside the 30 s window.
  TEST_ASSERT_FALSE(accept(key, msg, ts, mac, ts + 31000, true));
  // Within the window → still accepted.
  TEST_ASSERT_TRUE(accept(key, msg, ts, mac, ts + 29000, true));
}

static void test_accept_skips_window_when_unsynced() {
  uint8_t key[CLUSTER_HMAC_KEY_LEN];
  makeKey(key, 5);
  uint64_t ts = 1720000000000ULL;
  String msg = clusterHmacPingMsg(ts, "", -1);
  String mac = clusterHmacSign(key, msg);
  // Way outside the window, but unsynced → authenticity alone suffices.
  TEST_ASSERT_TRUE(accept(key, msg, ts, mac, ts + 999999ULL, false));
}

static void test_accept_rejects_malformed_mac() {
  uint8_t key[CLUSTER_HMAC_KEY_LEN];
  makeKey(key, 5);
  uint64_t ts = 1720000000000ULL;
  String msg = clusterHmacLeaveMsg(ts);
  TEST_ASSERT_FALSE(accept(key, msg, ts, "", ts, true));
  TEST_ASSERT_FALSE(accept(key, msg, ts, "notlongenough", ts, true));
}

// HIGH#1: the monotonic high-water mark stops replay / out-of-order requests
// even for a valid mac, and even while the clock is unsynced (where the ±30 s
// window is skipped) — closing the ping/leave replay DoS.
static void test_monotonic_guard_rejects_replay_and_out_of_order() {
  uint8_t key[CLUSTER_HMAC_KEY_LEN];
  makeKey(key, 5);
  uint64_t last = 0;
  uint64_t ts1 = 1720000000000ULL;
  String m1 = clusterHmacPingMsg(ts1, "", -1);
  String mac1 = clusterHmacSign(key, m1);
  // First accept advances the mark.
  TEST_ASSERT_TRUE(clusterHmacAccept(key, m1, ts1, mac1, ts1, true, last));
  // Exact replay (valid mac) is now rejected.
  TEST_ASSERT_FALSE(clusterHmacAccept(key, m1, ts1, mac1, ts1, true, last));
  // An older ts with its own valid mac is rejected too.
  uint64_t ts0 = ts1 - 5000;
  String m0 = clusterHmacPingMsg(ts0, "", -1);
  String mac0 = clusterHmacSign(key, m0);
  TEST_ASSERT_FALSE(clusterHmacAccept(key, m0, ts0, mac0, ts1, true, last));
  // A newer ts advances the mark again.
  uint64_t ts2 = ts1 + 5000;
  String m2 = clusterHmacPingMsg(ts2, "", -1);
  String mac2 = clusterHmacSign(key, m2);
  TEST_ASSERT_TRUE(clusterHmacAccept(key, m2, ts2, mac2, ts2, true, last));
}

static void test_monotonic_guard_blocks_replay_when_unsynced() {
  uint8_t key[CLUSTER_HMAC_KEY_LEN];
  makeKey(key, 5);
  uint64_t last = 0;
  uint64_t ts = 1720000000000ULL;
  String m = clusterHmacLeaveMsg(ts);
  String mac = clusterHmacSign(key, m);
  // Unsynced skips the window, but the monotonic guard still stops the
  // captured-leave replay a passive LAN observer could otherwise use.
  TEST_ASSERT_TRUE(clusterHmacAccept(key, m, ts, mac, 0, false, last));
  TEST_ASSERT_FALSE(clusterHmacAccept(key, m, ts, mac, 0, false, last));
}

// A rejected (bad-mac) request must NOT advance the mark — else an attacker
// without the key could wedge the follower by pushing ts forward.
static void test_bad_mac_does_not_advance_mark() {
  uint8_t key[CLUSTER_HMAC_KEY_LEN];
  makeKey(key, 5);
  uint64_t last = 0;
  uint64_t big = 1720000099999ULL;
  String forged = clusterHmacPingMsg(big, "", -1);
  // No valid mac for `big`.
  TEST_ASSERT_FALSE(clusterHmacAccept(key, forged, big, "deadbeef", big, true,
                                      last));
  TEST_ASSERT_EQUAL_UINT64(0ULL, last);
  // A genuine lower ts still gets through afterwards.
  uint64_t ts = 1720000000000ULL;
  String m = clusterHmacPingMsg(ts, "", -1);
  String mac = clusterHmacSign(key, m);
  TEST_ASSERT_TRUE(clusterHmacAccept(key, m, ts, mac, ts, true, last));
}

// HIGH#2: the ping mac binds the digest + you-index, so an on-path attacker
// cannot swap the promote-critical member table under a valid (ts,mac).
static void test_ping_mac_binds_digest_and_you() {
  uint8_t key[CLUSTER_HMAC_KEY_LEN];
  makeKey(key, 5);
  uint64_t ts = 1720000000000ULL;
  String digest = "{\"gen\":1,\"table\":\"a|0|0|4\"}";
  String m = clusterHmacPingMsg(ts, digest, 2);
  String mac = clusterHmacSign(key, m);
  // Correct digest/you verifies.
  TEST_ASSERT_TRUE(accept(key, m, ts, mac, ts, true));
  // Swapped table under the same mac is rejected (canonical differs).
  String swappedTable =
      clusterHmacPingMsg(ts, "{\"gen\":1,\"table\":\"evil|0|0|4\"}", 2);
  TEST_ASSERT_FALSE(accept(key, swappedTable, ts, mac, ts, true));
  // Swapped you-index under the same mac is rejected too.
  String swappedYou = clusterHmacPingMsg(ts, digest, 3);
  TEST_ASSERT_FALSE(accept(key, swappedYou, ts, mac, ts, true));
}

static void test_u64_str_round_trip() {
  const uint64_t vals[] = {0ULL, 1ULL, 42ULL, 1720000123456ULL,
                           18446744073709551615ULL};
  for (unsigned i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
    TEST_ASSERT_EQUAL_UINT64(vals[i], clusterU64FromStr(clusterU64ToStr(vals[i])));
  }
  // Trailing junk stops the parse (NVS/EEPROM never writes it, but be safe).
  TEST_ASSERT_EQUAL_UINT64(123ULL, clusterU64FromStr("123x9"));
}

// The coarse-persist gate: the mark is only written to flash once it has moved
// a full delta past the last persisted value, so a reboot reloads a mark at
// most one delta stale (bounding post-reboot replay) without flooding flash.
static void test_mark_persist_threshold() {
  TEST_ASSERT_FALSE(clusterHmacMarkNeedsPersist(0, 0));
  TEST_ASSERT_FALSE(
      clusterHmacMarkNeedsPersist(CLUSTER_HMAC_PERSIST_DELTA_MS - 1, 0));
  TEST_ASSERT_TRUE(
      clusterHmacMarkNeedsPersist(CLUSTER_HMAC_PERSIST_DELTA_MS, 0));
  uint64_t base = 1720000000000ULL;
  TEST_ASSERT_FALSE(clusterHmacMarkNeedsPersist(base + 1000, base));
  TEST_ASSERT_TRUE(
      clusterHmacMarkNeedsPersist(base + CLUSTER_HMAC_PERSIST_DELTA_MS, base));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_sha256_known_answers);
  RUN_TEST(test_sha256_multiblock);
  RUN_TEST(test_hmac_sha256_rfc4231_case2);
  RUN_TEST(test_hmac_long_key_is_hashed);
  RUN_TEST(test_key_hex_round_trip);
  RUN_TEST(test_key_from_hex_rejects_bad_input);
  RUN_TEST(test_canonical_messages_are_stable);
  RUN_TEST(test_sign_then_accept_round_trip);
  RUN_TEST(test_accept_rejects_wrong_key);
  RUN_TEST(test_accept_rejects_tampered_content);
  RUN_TEST(test_accept_rejects_replay_outside_window_when_synced);
  RUN_TEST(test_accept_skips_window_when_unsynced);
  RUN_TEST(test_accept_rejects_malformed_mac);
  RUN_TEST(test_monotonic_guard_rejects_replay_and_out_of_order);
  RUN_TEST(test_monotonic_guard_blocks_replay_when_unsynced);
  RUN_TEST(test_bad_mac_does_not_advance_mark);
  RUN_TEST(test_ping_mac_binds_digest_and_you);
  RUN_TEST(test_u64_str_round_trip);
  RUN_TEST(test_mark_persist_threshold);
  return UNITY_END();
}
