// Native tests for SntpReply.h: the answer the master gives a row board that
// asks it for the time.
#include <unity.h>

#include <string.h>

#include "../../SntpReply.h"

void setUp() {}
void tearDown() {}

static uint8_t request[SNTP_PACKET_LEN];
static uint8_t reply[SNTP_PACKET_LEN];

static void clientRequest(uint8_t version = 4) {
  memset(request, 0, sizeof request);
  request[0] = (uint8_t)((version << 3) | 3);  // no warning, version, client
  request[2] = 6;                              // poll
  const uint8_t stamp[8] = {0xE9, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
  memcpy(request + 40, stamp, 8);              // the client's transmit time
}

static uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// 2026-10-06 12:00:00 UTC
static const uint64_t NOON_US = 1791288000ULL * 1000000ULL;

static void test_a_client_request_is_answered_as_a_server() {
  clientRequest();
  TEST_ASSERT_TRUE(sntpBuildReply(request, sizeof request, NOON_US, NOON_US + 250, reply));
  TEST_ASSERT_EQUAL_HEX8((4 << 3) | 4, reply[0]);  // no warning, version 4, server
  TEST_ASSERT_EQUAL(SNTP_STRATUM, reply[1]);
  TEST_ASSERT_EQUAL(6, reply[2]);                  // the client's poll, echoed
}

static void test_the_clients_transmit_time_comes_back_as_originate() {
  clientRequest();
  sntpBuildReply(request, sizeof request, NOON_US, NOON_US, reply);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(request + 40, reply + 24, 8);
}

static void test_times_are_seconds_since_1900_with_a_binary_fraction() {
  clientRequest();
  sntpBuildReply(request, sizeof request, NOON_US + 500000, NOON_US + 750000, reply);
  TEST_ASSERT_EQUAL_UINT32(1791288000UL + 2208988800UL, be32(reply + 32));  // receive, seconds
  TEST_ASSERT_EQUAL_HEX32(0x80000000UL, be32(reply + 36));                  // .5 s
  TEST_ASSERT_EQUAL_UINT32(1791288000UL + 2208988800UL, be32(reply + 40));  // transmit
  TEST_ASSERT_EQUAL_HEX32(0xC0000000UL, be32(reply + 44));                  // .75 s
  TEST_ASSERT_EQUAL_UINT32(be32(reply + 32), be32(reply + 16));             // reference = receive
}

static void test_a_microsecond_is_about_4295_fraction_units() {
  clientRequest();
  sntpBuildReply(request, sizeof request, NOON_US + 1, NOON_US + 999999, reply);
  TEST_ASSERT_UINT32_WITHIN(1, 4295, be32(reply + 36));
  TEST_ASSERT_UINT32_WITHIN(4295, 0xFFFFFFFFUL - 4295, be32(reply + 44));
}

static void test_the_version_of_the_request_is_answered_in_kind() {
  clientRequest(3);
  TEST_ASSERT_TRUE(sntpBuildReply(request, sizeof request, NOON_US, NOON_US, reply));
  TEST_ASSERT_EQUAL_HEX8((3 << 3) | 4, reply[0]);
}

static void test_what_is_not_a_client_request_gets_no_answer() {
  clientRequest();
  TEST_ASSERT_FALSE(sntpBuildReply(request, SNTP_PACKET_LEN - 1, NOON_US, NOON_US, reply));  // short
  request[0] = (4 << 3) | 4;  // a server's packet: answering it would be a loop
  TEST_ASSERT_FALSE(sntpBuildReply(request, sizeof request, NOON_US, NOON_US, reply));
  request[0] = (4 << 3) | 1;  // symmetric active
  TEST_ASSERT_FALSE(sntpBuildReply(request, sizeof request, NOON_US, NOON_US, reply));
  clientRequest(0);
  TEST_ASSERT_FALSE(sntpBuildReply(request, sizeof request, NOON_US, NOON_US, reply));
  clientRequest(5);
  TEST_ASSERT_FALSE(sntpBuildReply(request, sizeof request, NOON_US, NOON_US, reply));
}

// A longer packet (extension fields, a MAC) is answered from its first 48 bytes.
static void test_a_longer_request_is_answered_from_its_header() {
  uint8_t longer[SNTP_PACKET_LEN + 20];
  clientRequest();
  memcpy(longer, request, sizeof request);
  memset(longer + SNTP_PACKET_LEN, 0xAA, 20);
  TEST_ASSERT_TRUE(sntpBuildReply(longer, sizeof longer, NOON_US, NOON_US, reply));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_client_request_is_answered_as_a_server);
  RUN_TEST(test_the_clients_transmit_time_comes_back_as_originate);
  RUN_TEST(test_times_are_seconds_since_1900_with_a_binary_fraction);
  RUN_TEST(test_a_microsecond_is_about_4295_fraction_units);
  RUN_TEST(test_the_version_of_the_request_is_answered_in_kind);
  RUN_TEST(test_what_is_not_a_client_request_gets_no_answer);
  RUN_TEST(test_a_longer_request_is_answered_from_its_header);
  return UNITY_END();
}
