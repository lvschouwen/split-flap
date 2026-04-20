// Host-side end-to-end tests for HexFlashParser.
//
// Exercises the same parser that drives production unit OTA, but with an
// in-memory capture callback instead of the real Wire-backed twiboot
// client. Lets us simulate an entire firmware flash byte-for-byte
// without any hardware.

#include <ArduinoFake.h>
#include <unity.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "../../HexFlashParser.h"

using namespace fakeit;

struct CapturedPage {
  uint16_t addr;
  uint8_t  data[HEX_PAGE_SIZE];
};

static std::vector<CapturedPage> g_captured;
static bool                      g_forceFlushFail = false;

static bool capturePage(const uint8_t* page, uint16_t addr, void* /*ctx*/) {
  if (g_forceFlushFail) return false;
  CapturedPage c;
  c.addr = addr;
  memcpy(c.data, page, HEX_PAGE_SIZE);
  g_captured.push_back(c);
  return true;
}

void setUp() {
  ArduinoFakeReset();
  g_captured.clear();
  g_forceFlushFail = false;
}
void tearDown() {}

static bool feedHexString(HexFlashState& s, const char* hex) {
  return hexFeedChunk(s, (const uint8_t*)hex, strlen(hex), capturePage, nullptr);
}

// --- Happy path ----------------------------------------------------------

static void test_empty_input_produces_no_pages() {
  HexFlashState s;
  hexInitState(s);
  TEST_ASSERT_TRUE(feedHexString(s, ""));
  TEST_ASSERT_TRUE(hexFlushFinal(s, capturePage, nullptr));
  TEST_ASSERT_EQUAL_size_t(0, g_captured.size());
}

static void test_single_record_buffers_into_single_page() {
  HexFlashState s;
  hexInitState(s);
  // :02 0000 00 DEAD 73   (02 bytes at 0x0000 = DE AD)
  TEST_ASSERT_TRUE(feedHexString(s,
      ":02000000DEAD73\n"
      ":00000001FF\n"));
  TEST_ASSERT_TRUE(hexFlushFinal(s, capturePage, nullptr));

  TEST_ASSERT_EQUAL_size_t(1, g_captured.size());
  TEST_ASSERT_EQUAL_UINT16(0x0000, g_captured[0].addr);
  TEST_ASSERT_EQUAL_UINT8(0xDE, g_captured[0].data[0]);
  TEST_ASSERT_EQUAL_UINT8(0xAD, g_captured[0].data[1]);
  // Remainder of the page is padded with 0xFF.
  TEST_ASSERT_EQUAL_UINT8(0xFF, g_captured[0].data[2]);
  TEST_ASSERT_EQUAL_UINT8(0xFF, g_captured[0].data[127]);
}

static void test_record_crossing_page_boundary_emits_two_pages() {
  // Four bytes starting at 0x007F: the first byte belongs to page 0x0000,
  // the next three to page 0x0080. Parser must flush the first page when
  // it sees the boundary crossing.
  HexFlashState s;
  hexInitState(s);
  TEST_ASSERT_TRUE(feedHexString(s,
      ":04007F00AABBCCDD5F\n"
      ":00000001FF\n"));
  TEST_ASSERT_TRUE(hexFlushFinal(s, capturePage, nullptr));

  TEST_ASSERT_EQUAL_size_t(2, g_captured.size());
  TEST_ASSERT_EQUAL_UINT16(0x0000, g_captured[0].addr);
  TEST_ASSERT_EQUAL_UINT8(0xAA, g_captured[0].data[127]);
  TEST_ASSERT_EQUAL_UINT16(0x0080, g_captured[1].addr);
  TEST_ASSERT_EQUAL_UINT8(0xBB, g_captured[1].data[0]);
  TEST_ASSERT_EQUAL_UINT8(0xCC, g_captured[1].data[1]);
  TEST_ASSERT_EQUAL_UINT8(0xDD, g_captured[1].data[2]);
}

static void test_contiguous_records_accumulate_in_same_page() {
  // Real avr-objcopy output uses 16-byte records, which stay under the
  // 64-char line buffer (43 chars each). Two contiguous records at 0x0000
  // and 0x0010 must land in a single page with correct byte order.
  HexFlashState s;
  hexInitState(s);
  TEST_ASSERT_TRUE_MESSAGE(feedHexString(s,
      ":100000000102030405060708090A0B0C0D0E0F1068\n"
      ":100010001112131415161718191A1B1C1D1E1F2058\n"
      ":00000001FF\n"), s.errorMsg.c_str());
  TEST_ASSERT_TRUE(hexFlushFinal(s, capturePage, nullptr));

  TEST_ASSERT_EQUAL_size_t(1, g_captured.size());
  TEST_ASSERT_EQUAL_UINT16(0x0000, g_captured[0].addr);
  for (int i = 0; i < 32; i++) {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(i + 1), g_captured[0].data[i]);
  }
  // Bytes past the written region are the 0xFF init pattern.
  TEST_ASSERT_EQUAL_UINT8(0xFF, g_captured[0].data[32]);
  TEST_ASSERT_EQUAL_UINT8(0xFF, g_captured[0].data[127]);
}

static void test_chunk_split_mid_line_resumes_cleanly() {
  // Feed the same record but split the chunk at byte 5 — the line
  // accumulator has to survive across feed calls.
  HexFlashState s;
  hexInitState(s);
  const char* line = ":02000000DEAD73\n";
  TEST_ASSERT_TRUE(hexFeedChunk(s, (const uint8_t*)line, 5, capturePage, nullptr));
  TEST_ASSERT_TRUE(hexFeedChunk(s, (const uint8_t*)line + 5, strlen(line) - 5, capturePage, nullptr));
  TEST_ASSERT_TRUE(feedHexString(s, ":00000001FF\n"));
  TEST_ASSERT_TRUE(hexFlushFinal(s, capturePage, nullptr));

  TEST_ASSERT_EQUAL_size_t(1, g_captured.size());
  TEST_ASSERT_EQUAL_UINT8(0xDE, g_captured[0].data[0]);
  TEST_ASSERT_EQUAL_UINT8(0xAD, g_captured[0].data[1]);
}

static void test_eof_record_alone_produces_nothing() {
  HexFlashState s;
  hexInitState(s);
  TEST_ASSERT_TRUE(feedHexString(s, ":00000001FF\n"));
  TEST_ASSERT_TRUE(hexFlushFinal(s, capturePage, nullptr));
  TEST_ASSERT_EQUAL_size_t(0, g_captured.size());
}

static void test_totalBytesWritten_increments_per_page() {
  HexFlashState s;
  hexInitState(s);
  TEST_ASSERT_TRUE(feedHexString(s, ":02000000DEAD73\n"));
  TEST_ASSERT_TRUE(hexFlushFinal(s, capturePage, nullptr));
  TEST_ASSERT_EQUAL_UINT32(HEX_PAGE_SIZE, s.totalBytesWritten);
}

// --- Error paths ---------------------------------------------------------

static void test_line_without_colon_is_error() {
  HexFlashState s;
  hexInitState(s);
  TEST_ASSERT_FALSE(feedHexString(s, "not a hex line at all, just garbage\n"));
  TEST_ASSERT_TRUE(s.errorMsg.length() > 0);
}

static void test_malformed_header_is_error() {
  HexFlashState s;
  hexInitState(s);
  // Replaces one of the header hex digits with a non-hex character.
  TEST_ASSERT_FALSE(feedHexString(s, ":0X000000DEAD73\n"));
  TEST_ASSERT_TRUE(s.errorMsg.indexOf("Malformed HEX header") >= 0 ||
                   s.errorMsg.indexOf("Invalid HEX line") >= 0);
}

static void test_bad_data_byte_is_error() {
  HexFlashState s;
  hexInitState(s);
  // DE is valid, XX is not.
  TEST_ASSERT_FALSE(feedHexString(s, ":02000000DEXX73\n"));
  TEST_ASSERT_TRUE(s.errorMsg.indexOf("Bad HEX data byte") >= 0);
}

static void test_address_past_appMax_is_error() {
  HexFlashState s;
  // Make the limit tight so we can trip it easily.
  hexInitState(s, /*appMax=*/0x0100);
  // Record at 0x0100 — just past the limit.
  TEST_ASSERT_FALSE(feedHexString(s, ":02010000DEAD72\n"));
  TEST_ASSERT_TRUE(s.errorMsg.indexOf("past app section") >= 0);
}

static void test_exceeding_maxHexBytes_is_error() {
  HexFlashState s;
  hexInitState(s, HEX_APP_MAX_DEFAULT, /*maxHexBytes=*/10);
  // 16 bytes of garbage — doesn't need to be valid HEX to trip the counter.
  TEST_ASSERT_FALSE(feedHexString(s, "xxxxxxxxxxxxxxxx"));
  TEST_ASSERT_TRUE(s.errorMsg.indexOf("size limit") >= 0);
}

static void test_short_line_is_error() {
  HexFlashState s;
  hexInitState(s);
  TEST_ASSERT_FALSE(feedHexString(s, ":02000\n"));
  TEST_ASSERT_TRUE(s.errorMsg.indexOf("Invalid HEX line") >= 0);
}

static void test_line_buffer_overflow_is_error() {
  // Feed a long run of non-newline characters; the internal line buffer
  // is HEX_LINE_BUF_SIZE bytes.
  HexFlashState s;
  hexInitState(s);
  std::vector<uint8_t> big(HEX_LINE_BUF_SIZE + 16, (uint8_t)'A');
  TEST_ASSERT_FALSE(hexFeedChunk(s, big.data(), big.size(), capturePage, nullptr));
  TEST_ASSERT_TRUE(s.errorMsg.indexOf("line exceeds internal buffer") >= 0);
}

static void test_flush_callback_failure_propagates() {
  HexFlashState s;
  hexInitState(s);
  g_forceFlushFail = true;
  // Record crosses the page boundary at byte 128, so the parser must
  // try to flush the first page mid-feed.
  TEST_ASSERT_FALSE(feedHexString(s,
      ":04007F00AABBCCDD5F\n"));
  TEST_ASSERT_TRUE(s.errorMsg.length() > 0);
}

// --- Integration: real unit-firmware.hex --------------------------------

// Read the shipped unit firmware from disk. `pio test -e native` runs
// binaries with CWD at the project directory, so this relative path
// lands inside ESPMaster/data/.
static std::vector<uint8_t> loadUnitFirmwareHex() {
  std::vector<uint8_t> out;
  FILE* f = fopen("data/unit-firmware.hex", "rb");
  if (!f) return out;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  out.resize((size_t)size);
  if (fread(out.data(), 1, (size_t)size, f) != (size_t)size) out.clear();
  fclose(f);
  return out;
}

static void test_real_unit_firmware_parses_to_nonzero_page_count() {
  auto hexBytes = loadUnitFirmwareHex();
  if (hexBytes.empty()) {
    TEST_IGNORE_MESSAGE("data/unit-firmware.hex not readable from CWD; skipping");
    return;
  }

  HexFlashState s;
  hexInitState(s);
  TEST_ASSERT_TRUE_MESSAGE(
      hexFeedChunk(s, hexBytes.data(), hexBytes.size(), capturePage, nullptr),
      s.errorMsg.c_str());
  TEST_ASSERT_TRUE(hexFlushFinal(s, capturePage, nullptr));

  TEST_ASSERT_GREATER_THAN_size_t(0, g_captured.size());
  // Each page should be 128 bytes and page-aligned.
  for (const auto& c : g_captured) {
    TEST_ASSERT_EQUAL_UINT16(0, c.addr & (HEX_PAGE_SIZE - 1));
  }
  // Pages must be strictly increasing in address (parser emits each page
  // exactly once, in encounter order — for a well-formed HEX that means
  // ascending flash addresses).
  for (size_t i = 1; i < g_captured.size(); i++) {
    TEST_ASSERT_TRUE(g_captured[i].addr > g_captured[i - 1].addr);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_empty_input_produces_no_pages);
  RUN_TEST(test_single_record_buffers_into_single_page);
  RUN_TEST(test_record_crossing_page_boundary_emits_two_pages);
  RUN_TEST(test_contiguous_records_accumulate_in_same_page);
  RUN_TEST(test_chunk_split_mid_line_resumes_cleanly);
  RUN_TEST(test_eof_record_alone_produces_nothing);
  RUN_TEST(test_totalBytesWritten_increments_per_page);
  RUN_TEST(test_line_without_colon_is_error);
  RUN_TEST(test_malformed_header_is_error);
  RUN_TEST(test_bad_data_byte_is_error);
  RUN_TEST(test_address_past_appMax_is_error);
  RUN_TEST(test_exceeding_maxHexBytes_is_error);
  RUN_TEST(test_short_line_is_error);
  RUN_TEST(test_line_buffer_overflow_is_error);
  RUN_TEST(test_flush_callback_failure_propagates);
  RUN_TEST(test_real_unit_firmware_parses_to_nonzero_page_count);
  return UNITY_END();
}
