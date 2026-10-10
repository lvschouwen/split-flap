// Host-side unit tests for ImprovPolicy.h (#601) — WiFi credentials over USB
// by the Improv serial protocol. Requests are built here byte for byte as the
// installer sends them; the answers are checked as the bytes it reads.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../ImprovPolicy.h"

void setUp() {}
void tearDown() {}

// A request frame as the installer writes it: no newline before it.
static std::vector<uint8_t> request(uint8_t type, std::vector<uint8_t> data) {
  std::vector<uint8_t> out = {'I', 'M', 'P', 'R', 'O', 'V', 1, type,
                              (uint8_t)data.size()};
  out.insert(out.end(), data.begin(), data.end());
  uint8_t sum = 0;
  for (uint8_t b : out) sum += b;
  out.push_back(sum);
  return out;
}

static std::vector<uint8_t> rpc(uint8_t command, std::vector<uint8_t> payload = {}) {
  std::vector<uint8_t> data = {command, (uint8_t)payload.size()};
  data.insert(data.end(), payload.begin(), payload.end());
  return request(improv::TYPE_RPC, data);
}

static std::vector<uint8_t> wifiSettings(const std::string& ssid,
                                         const std::string& password) {
  std::vector<uint8_t> payload = {(uint8_t)ssid.size()};
  payload.insert(payload.end(), ssid.begin(), ssid.end());
  payload.push_back((uint8_t)password.size());
  payload.insert(payload.end(), password.begin(), password.end());
  return rpc(improv::WIFI_SETTINGS, payload);
}

static ImprovOut feed(ImprovRx& rx, const std::vector<uint8_t>& bytes,
                      const ImprovFacts& facts) {
  ImprovOut out;
  for (uint8_t b : bytes) improvFeed(rx, b, facts, out);
  return out;
}

static ImprovFacts factsOf(ImprovLink link) {
  ImprovFacts f;
  f.link = link;
  f.canScan = link != ImprovLink::Joining;
  f.url = link == ImprovLink::Online ? "http://192.168.1.50/" : "";
  f.name = "split-flap-ee7da4";
  f.rev = "abc1234";
  return f;
}

// What a packet says, once its frame is checked: newline, header, version,
// the length that matches, the checksum over all of it, newline.
struct Read {
  uint8_t type;
  std::vector<uint8_t> data;
};

static Read readPacket(const ImprovPacket& p) {
  TEST_ASSERT_TRUE(p.size() >= 12);
  TEST_ASSERT_EQUAL_UINT8('\n', p.front());
  TEST_ASSERT_EQUAL_UINT8('\n', p.back());
  TEST_ASSERT_EQUAL_MEMORY("IMPROV", &p[1], 6);
  TEST_ASSERT_EQUAL_UINT8(1, p[7]);
  size_t length = p[9];
  TEST_ASSERT_EQUAL_UINT32(1 + 9 + length + 2, p.size());
  uint8_t sum = 0;
  for (size_t i = 1; i < 10 + length; i++) sum += p[i];
  TEST_ASSERT_EQUAL_UINT8(sum, p[10 + length]);
  return {p[8], std::vector<uint8_t>(p.begin() + 10, p.begin() + 10 + length)};
}

// The strings of an RPC result, as the installer splits them.
static std::vector<std::string> strings(const Read& r, uint8_t command) {
  TEST_ASSERT_EQUAL_UINT8(improv::TYPE_RPC_RESPONSE, r.type);
  TEST_ASSERT_EQUAL_UINT8(command, r.data[0]);
  size_t end = 2 + r.data[1];
  TEST_ASSERT_TRUE(end <= r.data.size());
  std::vector<std::string> out;
  for (size_t at = 2; at < end;) {
    size_t n = r.data[at++];
    TEST_ASSERT_TRUE(at + n <= end);
    out.emplace_back(r.data.begin() + at, r.data.begin() + at + n);
    at += n;
  }
  return out;
}

static void assertState(const ImprovPacket& p, improv::State state) {
  Read r = readPacket(p);
  TEST_ASSERT_EQUAL_UINT8(improv::TYPE_CURRENT_STATE, r.type);
  TEST_ASSERT_EQUAL_UINT32(1, r.data.size());
  TEST_ASSERT_EQUAL_UINT8(state, r.data[0]);
}

static void assertError(const ImprovPacket& p, improv::Error error) {
  Read r = readPacket(p);
  TEST_ASSERT_EQUAL_UINT8(improv::TYPE_ERROR_STATE, r.type);
  TEST_ASSERT_EQUAL_UINT8(error, r.data[0]);
}

// ---------------------------------------------------------------------------

static void test_a_new_board_says_it_takes_credentials() {
  ImprovRx rx;
  ImprovOut out = feed(rx, rpc(improv::GET_CURRENT_STATE), factsOf(ImprovLink::Ready));
  TEST_ASSERT_EQUAL_UINT32(1, out.packets.size());
  assertState(out.packets[0], improv::STATE_AUTHORIZED);
  TEST_ASSERT_FALSE(out.credentials);
  TEST_ASSERT_FALSE(out.scan);
}

static void test_a_joining_board_says_so() {
  ImprovRx rx;
  ImprovOut out = feed(rx, rpc(improv::GET_CURRENT_STATE), factsOf(ImprovLink::Joining));
  TEST_ASSERT_EQUAL_UINT32(1, out.packets.size());
  assertState(out.packets[0], improv::STATE_PROVISIONING);
}

static void test_an_online_board_gives_its_address_with_its_state() {
  ImprovRx rx;
  ImprovOut out = feed(rx, rpc(improv::GET_CURRENT_STATE), factsOf(ImprovLink::Online));
  TEST_ASSERT_EQUAL_UINT32(2, out.packets.size());
  assertState(out.packets[0], improv::STATE_PROVISIONED);
  std::vector<std::string> s =
      strings(readPacket(out.packets[1]), improv::GET_CURRENT_STATE);
  TEST_ASSERT_EQUAL_UINT32(1, s.size());
  TEST_ASSERT_EQUAL_STRING("http://192.168.1.50/", s[0].c_str());
}

static void test_device_info_names_firmware_rev_chip_and_board() {
  ImprovRx rx;
  ImprovOut out = feed(rx, rpc(improv::GET_DEVICE_INFO), factsOf(ImprovLink::Ready));
  TEST_ASSERT_EQUAL_UINT32(1, out.packets.size());
  std::vector<std::string> s =
      strings(readPacket(out.packets[0]), improv::GET_DEVICE_INFO);
  TEST_ASSERT_EQUAL_UINT32(4, s.size());
  TEST_ASSERT_EQUAL_STRING("Split-Flap", s[0].c_str());
  TEST_ASSERT_EQUAL_STRING("abc1234", s[1].c_str());
  TEST_ASSERT_EQUAL_STRING("ESP32-S3", s[2].c_str());
  TEST_ASSERT_EQUAL_STRING("split-flap-ee7da4", s[3].c_str());
}

static void test_credentials_are_handed_over_and_answered_as_provisioning() {
  ImprovRx rx;
  ImprovOut out = feed(rx, wifiSettings("Home Net", "correct horse"),
                       factsOf(ImprovLink::Ready));
  TEST_ASSERT_TRUE(out.credentials);
  TEST_ASSERT_EQUAL_STRING("Home Net", out.ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("correct horse", out.password.c_str());
  TEST_ASSERT_EQUAL_UINT32(1, out.packets.size());
  assertState(out.packets[0], improv::STATE_PROVISIONING);
}

static void test_an_open_network_needs_no_password() {
  ImprovRx rx;
  ImprovOut out = feed(rx, wifiSettings("cafe", ""), factsOf(ImprovLink::Ready));
  TEST_ASSERT_TRUE(out.credentials);
  TEST_ASSERT_EQUAL_STRING("", out.password.c_str());
}

// The same rules as the setup page: what could never join is refused here,
// not saved and found out by three restarts.
static void test_credentials_the_setup_page_would_refuse_are_refused() {
  ImprovRx rx;
  ImprovOut shortPass = feed(rx, wifiSettings("Home", "1234567"), factsOf(ImprovLink::Ready));
  TEST_ASSERT_FALSE(shortPass.credentials);
  TEST_ASSERT_EQUAL_UINT32(1, shortPass.packets.size());
  assertError(shortPass.packets[0], improv::ERROR_UNABLE_TO_CONNECT);

  ImprovRx rx2;
  ImprovOut noName = feed(rx2, wifiSettings("", "12345678"), factsOf(ImprovLink::Ready));
  TEST_ASSERT_FALSE(noName.credentials);

  ImprovRx rx3;
  ImprovOut longName = feed(rx3, wifiSettings(std::string(33, 'a'), "12345678"),
                            factsOf(ImprovLink::Ready));
  TEST_ASSERT_FALSE(longName.credentials);

  ImprovRx rx4;
  ImprovOut control = feed(rx4, wifiSettings("Ho\x01me", "12345678"), factsOf(ImprovLink::Ready));
  TEST_ASSERT_FALSE(control.credentials);
}

static void test_the_next_request_clears_the_error_before_its_answer() {
  ImprovRx rx;
  feed(rx, wifiSettings("Home", "short"), factsOf(ImprovLink::Ready));
  ImprovOut out = feed(rx, wifiSettings("Home", "long enough"), factsOf(ImprovLink::Ready));
  TEST_ASSERT_TRUE(out.credentials);
  TEST_ASSERT_EQUAL_UINT32(2, out.packets.size());
  assertError(out.packets[0], improv::ERROR_NONE);
  assertState(out.packets[1], improv::STATE_PROVISIONING);
  // And only once.
  ImprovOut again = feed(rx, rpc(improv::GET_CURRENT_STATE), factsOf(ImprovLink::Ready));
  TEST_ASSERT_EQUAL_UINT32(1, again.packets.size());
}

static void test_a_scan_is_asked_for_when_it_disturbs_nothing() {
  ImprovRx rx;
  ImprovOut out = feed(rx, rpc(improv::GET_WIFI_NETWORKS), factsOf(ImprovLink::Ready));
  TEST_ASSERT_TRUE(out.scan);
  TEST_ASSERT_EQUAL_UINT32(0, out.packets.size());
}

static void test_no_scan_under_a_join_and_the_list_ends_at_once() {
  ImprovRx rx;
  ImprovOut out = feed(rx, rpc(improv::GET_WIFI_NETWORKS), factsOf(ImprovLink::Joining));
  TEST_ASSERT_FALSE(out.scan);
  TEST_ASSERT_EQUAL_UINT32(1, out.packets.size());
  TEST_ASSERT_EQUAL_UINT32(
      0, strings(readPacket(out.packets[0]), improv::GET_WIFI_NETWORKS).size());
}

static void test_scan_reply_is_a_result_per_network_and_an_empty_one() {
  ImprovOut out;
  improvScanReply({{"Home Net", -48, true}, {"cafe", -80, false}}, out);
  TEST_ASSERT_EQUAL_UINT32(3, out.packets.size());
  std::vector<std::string> a =
      strings(readPacket(out.packets[0]), improv::GET_WIFI_NETWORKS);
  TEST_ASSERT_EQUAL_UINT32(3, a.size());
  TEST_ASSERT_EQUAL_STRING("Home Net", a[0].c_str());
  TEST_ASSERT_EQUAL_STRING("-48", a[1].c_str());
  TEST_ASSERT_EQUAL_STRING("YES", a[2].c_str());
  std::vector<std::string> b =
      strings(readPacket(out.packets[1]), improv::GET_WIFI_NETWORKS);
  TEST_ASSERT_EQUAL_STRING("NO", b[2].c_str());
  TEST_ASSERT_EQUAL_UINT32(
      0, strings(readPacket(out.packets[2]), improv::GET_WIFI_NETWORKS).size());
}

static void test_a_command_this_board_does_not_have_is_an_unknown_one() {
  ImprovRx rx;
  ImprovOut out = feed(rx, rpc(improv::GET_NETWORK_STATE), factsOf(ImprovLink::Online));
  TEST_ASSERT_EQUAL_UINT32(1, out.packets.size());
  assertError(out.packets[0], improv::ERROR_UNKNOWN_RPC);
}

static void test_a_frame_with_a_wrong_checksum_is_an_invalid_one() {
  ImprovRx rx;
  std::vector<uint8_t> bytes = rpc(improv::GET_CURRENT_STATE);
  bytes.back() ^= 0x55;
  ImprovOut out = feed(rx, bytes, factsOf(ImprovLink::Ready));
  TEST_ASSERT_EQUAL_UINT32(1, out.packets.size());
  assertError(out.packets[0], improv::ERROR_INVALID_RPC);
}

static void test_other_text_on_the_port_is_ignored_and_a_request_after_it_is_read() {
  ImprovRx rx;
  std::string noise = "hello\r\nIMPRESSIVE IMP\nIM";
  std::vector<uint8_t> bytes(noise.begin(), noise.end());
  std::vector<uint8_t> req = rpc(improv::GET_CURRENT_STATE);
  bytes.insert(bytes.end(), req.begin(), req.end());
  ImprovOut out = feed(rx, bytes, factsOf(ImprovLink::Ready));
  TEST_ASSERT_EQUAL_UINT32(1, out.packets.size());
  assertState(out.packets[0], improv::STATE_AUTHORIZED);
}

static void test_two_requests_back_to_back_are_both_answered() {
  ImprovRx rx;
  std::vector<uint8_t> bytes = rpc(improv::GET_CURRENT_STATE);
  std::vector<uint8_t> second = rpc(improv::GET_DEVICE_INFO);
  bytes.insert(bytes.end(), second.begin(), second.end());
  ImprovOut out = feed(rx, bytes, factsOf(ImprovLink::Ready));
  TEST_ASSERT_EQUAL_UINT32(2, out.packets.size());
}

static void test_the_longest_frame_fits_and_garbage_never_overruns() {
  ImprovRx rx;
  ImprovFacts facts = factsOf(ImprovLink::Ready);
  // A frame that claims 255 bytes of data and then never ends.
  std::vector<uint8_t> bytes = {'I', 'M', 'P', 'R', 'O', 'V', 1, improv::TYPE_RPC, 255};
  bytes.resize(2000, 0xAA);
  ImprovOut out = feed(rx, bytes, facts);
  TEST_ASSERT_TRUE(rx.length <= IMPROV_FRAME_MAX);
  ImprovOut after = feed(rx, rpc(improv::GET_CURRENT_STATE), facts);
  TEST_ASSERT_TRUE(after.packets.size() >= 1);
  assertState(after.packets.back(), improv::STATE_AUTHORIZED);
}

// --- the answer that crosses the restart -------------------------------------

static void test_pending_record_round_trips_a_port() {
  ImprovPendingRecord r;
  improvPendingEncode(r, 1);
  TEST_ASSERT_EQUAL_INT(1, improvPendingDecode(r));
  improvPendingEncode(r, 0);
  TEST_ASSERT_EQUAL_INT(0, improvPendingDecode(r));
  improvPendingClear(r);
  TEST_ASSERT_EQUAL_INT(-1, improvPendingDecode(r));
}

static void test_power_on_garbage_is_no_pending_answer() {
  ImprovPendingRecord r;
  memset(&r, 0xA5, sizeof(r));
  TEST_ASSERT_EQUAL_INT(-1, improvPendingDecode(r));
  memset(&r, 0x00, sizeof(r));
  TEST_ASSERT_EQUAL_INT(-1, improvPendingDecode(r));
  improvPendingEncode(r, 1);
  r.check ^= 1;
  TEST_ASSERT_EQUAL_INT(-1, improvPendingDecode(r));
  improvPendingEncode(r, 0);
  r.port = IMPROV_PORTS;
  r.check = (uint8_t)~r.port;
  TEST_ASSERT_EQUAL_INT(-1, improvPendingDecode(r));
}

static void test_the_boot_that_joined_gives_the_result_with_the_address() {
  ImprovOut out;
  TEST_ASSERT_TRUE(improvOutcome(factsOf(ImprovLink::Online), false, out));
  TEST_ASSERT_EQUAL_UINT32(2, out.packets.size());
  assertState(out.packets[0], improv::STATE_PROVISIONED);
  std::vector<std::string> s =
      strings(readPacket(out.packets[1]), improv::WIFI_SETTINGS);
  TEST_ASSERT_EQUAL_UINT32(1, s.size());
  TEST_ASSERT_EQUAL_STRING("http://192.168.1.50/", s[0].c_str());
}

static void test_nothing_is_said_while_the_join_is_still_open() {
  ImprovOut out;
  TEST_ASSERT_FALSE(improvOutcome(factsOf(ImprovLink::Joining), false, out));
  TEST_ASSERT_EQUAL_UINT32(0, out.packets.size());
}

static void test_back_in_the_setup_network_means_unable_to_connect() {
  ImprovOut out;
  TEST_ASSERT_TRUE(improvOutcome(factsOf(ImprovLink::Ready), true, out));
  TEST_ASSERT_EQUAL_UINT32(2, out.packets.size());
  assertError(out.packets[0], improv::ERROR_UNABLE_TO_CONNECT);
  assertState(out.packets[1], improv::STATE_AUTHORIZED);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_new_board_says_it_takes_credentials);
  RUN_TEST(test_a_joining_board_says_so);
  RUN_TEST(test_an_online_board_gives_its_address_with_its_state);
  RUN_TEST(test_device_info_names_firmware_rev_chip_and_board);
  RUN_TEST(test_credentials_are_handed_over_and_answered_as_provisioning);
  RUN_TEST(test_an_open_network_needs_no_password);
  RUN_TEST(test_credentials_the_setup_page_would_refuse_are_refused);
  RUN_TEST(test_the_next_request_clears_the_error_before_its_answer);
  RUN_TEST(test_a_scan_is_asked_for_when_it_disturbs_nothing);
  RUN_TEST(test_no_scan_under_a_join_and_the_list_ends_at_once);
  RUN_TEST(test_scan_reply_is_a_result_per_network_and_an_empty_one);
  RUN_TEST(test_a_command_this_board_does_not_have_is_an_unknown_one);
  RUN_TEST(test_a_frame_with_a_wrong_checksum_is_an_invalid_one);
  RUN_TEST(test_other_text_on_the_port_is_ignored_and_a_request_after_it_is_read);
  RUN_TEST(test_two_requests_back_to_back_are_both_answered);
  RUN_TEST(test_the_longest_frame_fits_and_garbage_never_overruns);
  RUN_TEST(test_pending_record_round_trips_a_port);
  RUN_TEST(test_power_on_garbage_is_no_pending_answer);
  RUN_TEST(test_the_boot_that_joined_gives_the_result_with_the_address);
  RUN_TEST(test_nothing_is_said_while_the_join_is_still_open);
  RUN_TEST(test_back_in_the_setup_network_means_unable_to_connect);
  return UNITY_END();
}
