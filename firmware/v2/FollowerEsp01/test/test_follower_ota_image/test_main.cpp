// Host-side tests for the gzip-upload checks of POST /firmware/master (#540):
// the image header must be visible inside the gzip file and carry the flash
// config the board runs, and the unpacked length must fit the app area.

#include <unity.h>

#include "../../FollowerOtaImage.h"

void setUp() {}
void tearDown() {}

// QIO, 1 MB / 40 MHz — what esp01_1m builds carry.
static const uint8_t RUNNING[4] = {0xE9, 0x02, 0x00, 0x20};

// The first bytes of a file as fwbuild.ota_gzip_image() writes it, holding
// an image header of the given flash mode and size byte.
static void makePacked(uint8_t* out, uint8_t mode, uint8_t sizeFreq) {
  const uint8_t head[] = {0x1F, 0x8B, 0x08, 0x00, 0, 0, 0, 0, 0x02, 0xFF,
                          0x00, 0x10, 0x00, 0xEF, 0xFF,
                          0xE9, 0x02, mode, sizeFreq};
  memcpy(out, head, sizeof(head));
}

static void test_kind_by_magic() {
  const uint8_t gz[] = {0x1F, 0x8B};
  const uint8_t raw[] = {0xE9, 0x02};
  TEST_ASSERT_TRUE(otaIsGzip(gz, 2));
  TEST_ASSERT_FALSE(otaIsGzip(raw, 2));
  TEST_ASSERT_FALSE(otaIsGzip(raw, 1));
  TEST_ASSERT_FALSE(otaIsGzip(gz, 0));
}

static void test_one_byte_first_chunk_still_takes_the_gzip_checks() {
  // A segment boundary right after the first file byte: classified plain,
  // the file would be flashed with every check skipped.
  const uint8_t one[] = {0x1F};
  TEST_ASSERT_TRUE(otaIsGzip(one, 1));
  TEST_ASSERT_TRUE(otaGzipCheck(one, 1, RUNNING) == OtaImageCheck::ShortChunk);
}

static void test_first_byte_alone_is_not_enough() {
  uint8_t f[19];
  makePacked(f, 0x00, 0x20);
  f[1] = 0x00;
  TEST_ASSERT_TRUE(otaGzipCheck(f, sizeof(f), RUNNING) ==
                   OtaImageCheck::NotInspectable);
}

static void test_matching_image_passes() {
  uint8_t f[19];
  makePacked(f, 0x00, 0x20);
  TEST_ASSERT_TRUE(otaGzipCheck(f, sizeof(f), RUNNING) == OtaImageCheck::Ok);
  // The frequency nibble is not part of the flash-size rule.
  makePacked(f, 0x00, 0x2F);
  TEST_ASSERT_TRUE(otaGzipCheck(f, sizeof(f), RUNNING) == OtaImageCheck::Ok);
}

static void test_other_flash_mode_refused() {
  uint8_t f[19];
  makePacked(f, 0x03, 0x20);  // DOUT image onto a QIO board
  TEST_ASSERT_TRUE(otaGzipCheck(f, sizeof(f), RUNNING) ==
                   OtaImageCheck::FlashMode);
  const uint8_t dout[4] = {0xE9, 0x02, 0x03, 0x20};
  makePacked(f, 0x00, 0x20);  // and the other way round
  TEST_ASSERT_TRUE(otaGzipCheck(f, sizeof(f), dout) ==
                   OtaImageCheck::FlashMode);
}

static void test_other_flash_size_refused() {
  uint8_t f[19];
  makePacked(f, 0x00, 0x40);  // 4 MB build
  TEST_ASSERT_TRUE(otaGzipCheck(f, sizeof(f), RUNNING) ==
                   OtaImageCheck::FlashSize);
}

static void test_plain_gzip_refused() {
  // `gzip -9 firmware.bin`: a dynamic-Huffman block follows the header, so
  // the image header is not readable.
  uint8_t f[19] = {0x1F, 0x8B, 0x08, 0x00, 0, 0, 0, 0, 0x02, 0x03,
                   0xEC, 0xBD, 0x0B, 0x7C, 0x54, 0xD5, 0x01, 0x02, 0x03};
  TEST_ASSERT_TRUE(otaGzipCheck(f, sizeof(f), RUNNING) ==
                   OtaImageCheck::NotInspectable);
}

static void test_gzip_with_optional_fields_refused() {
  uint8_t f[19];
  makePacked(f, 0x00, 0x20);
  f[3] = 0x08;  // FNAME: a file name sits where the stored block should be
  TEST_ASSERT_TRUE(otaGzipCheck(f, sizeof(f), RUNNING) ==
                   OtaImageCheck::NotInspectable);
}

static void test_broken_stored_block_refused() {
  uint8_t f[19];
  makePacked(f, 0x00, 0x20);
  f[13] = 0x00;  // ~LEN no longer the complement
  TEST_ASSERT_TRUE(otaGzipCheck(f, sizeof(f), RUNNING) ==
                   OtaImageCheck::NotInspectable);
  makePacked(f, 0x00, 0x20);
  f[11] = 0x02;  // stored block shorter than the header
  f[13] = 0xFD;
  TEST_ASSERT_TRUE(otaGzipCheck(f, sizeof(f), RUNNING) ==
                   OtaImageCheck::NotInspectable);
  makePacked(f, 0x00, 0x20);
  f[10] = 0x01;  // final block: nothing would follow the header
  TEST_ASSERT_TRUE(otaGzipCheck(f, sizeof(f), RUNNING) ==
                   OtaImageCheck::NotInspectable);
}

static void test_not_an_image_refused() {
  uint8_t f[19];
  makePacked(f, 0x00, 0x20);
  f[15] = 0x7F;
  TEST_ASSERT_TRUE(otaGzipCheck(f, sizeof(f), RUNNING) ==
                   OtaImageCheck::NotAnImage);
}

static void test_short_first_chunk_refused() {
  uint8_t f[19];
  makePacked(f, 0x00, 0x20);
  TEST_ASSERT_TRUE(otaGzipCheck(f, 18, RUNNING) == OtaImageCheck::ShortChunk);
}

static void test_every_refusal_has_a_reason() {
  for (uint8_t c = 1; c <= (uint8_t)OtaImageCheck::FlashSize; c++) {
    String why(otaImageCheckReason((OtaImageCheck)c));
    TEST_ASSERT_TRUE(why.length() > 0);
  }
}

static void test_tail_survives_any_chunk_split() {
  // ...payload, then ISIZE 453776 = 0x0006EC90 little-endian.
  const uint8_t file[] = {1, 2, 3, 4, 5, 6, 7, 0x90, 0xEC, 0x06, 0x00};
  for (size_t cut = 0; cut <= sizeof(file); cut++) {
    OtaGzipTail t;
    t.feed(file, cut);
    t.feed(file + cut, sizeof(file) - cut);
    TEST_ASSERT_EQUAL_UINT32(453776, t.unpackedLen());
    TEST_ASSERT_EQUAL_UINT32(sizeof(file), t.seen);
  }
  OtaGzipTail bytewise;
  for (size_t i = 0; i < sizeof(file); i++) bytewise.feed(file + i, 1);
  TEST_ASSERT_EQUAL_UINT32(453776, bytewise.unpackedLen());
}

static const uint32_t AREA = 0xFB000;  // eagle.flash.1m.ld

static OtaGzipTail tailFor(uint32_t n) {
  const uint8_t b[4] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16),
                        (uint8_t)(n >> 24)};
  OtaGzipTail t;
  t.feed(b, 4);
  return t;
}

static void test_gzip_reserves_only_what_the_request_carries() {
  // 317842 B file + multipart framing -> 78 sectors, not the whole 139.
  TEST_ASSERT_EQUAL_UINT32(78 * 4096, otaGzipReserve(318100, 139 * 4096));
  TEST_ASSERT_EQUAL_UINT32(4096, otaGzipReserve(1, 139 * 4096));
  TEST_ASSERT_EQUAL_UINT32(8192, otaGzipReserve(8192, 139 * 4096));
  // Unknown length: everything, as a plain image gets.
  TEST_ASSERT_EQUAL_UINT32(139 * 4096, otaGzipReserve(0, 139 * 4096));
  // Never more than there is.
  TEST_ASSERT_EQUAL_UINT32(139 * 4096, otaGzipReserve(900000, 139 * 4096));
}

static void test_unpacked_image_must_end_below_its_stored_copy() {
  const uint32_t reserved = 78 * 4096;         // stored at 708608..
  const uint32_t below = AREA - reserved;      // 708608
  TEST_ASSERT_TRUE(otaGzipUnpackFits(tailFor(453776), AREA, reserved));
  TEST_ASSERT_TRUE(otaGzipUnpackFits(tailFor(below), AREA, reserved));
  // One byte more needs the sector the packed copy starts in.
  TEST_ASSERT_FALSE(otaGzipUnpackFits(tailFor(below + 1), AREA, reserved));
  // The whole free space reserved (length unknown): the copy sits right
  // behind the running image, so a grown image is refused, not gambled on.
  const uint32_t all = 139 * 4096;             // stored at 458752..
  TEST_ASSERT_TRUE(otaGzipUnpackFits(tailFor(458752), AREA, all));
  TEST_ASSERT_FALSE(otaGzipUnpackFits(tailFor(470000), AREA, all));
}

static void test_unpacked_length_must_be_sane() {
  TEST_ASSERT_FALSE(otaGzipUnpackFits(tailFor(0), AREA, 4096));
  TEST_ASSERT_FALSE(otaGzipUnpackFits(tailFor(AREA + 1), AREA, 0));
  TEST_ASSERT_FALSE(otaGzipUnpackFits(tailFor(0xFFFFFFFFUL), AREA, 4096));
  TEST_ASSERT_FALSE(otaGzipUnpackFits(tailFor(453776), AREA, AREA + 4096));
  OtaGzipTail empty;
  TEST_ASSERT_FALSE(otaGzipUnpackFits(empty, AREA, 4096));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_kind_by_magic);
  RUN_TEST(test_one_byte_first_chunk_still_takes_the_gzip_checks);
  RUN_TEST(test_first_byte_alone_is_not_enough);
  RUN_TEST(test_matching_image_passes);
  RUN_TEST(test_other_flash_mode_refused);
  RUN_TEST(test_other_flash_size_refused);
  RUN_TEST(test_plain_gzip_refused);
  RUN_TEST(test_gzip_with_optional_fields_refused);
  RUN_TEST(test_broken_stored_block_refused);
  RUN_TEST(test_not_an_image_refused);
  RUN_TEST(test_short_first_chunk_refused);
  RUN_TEST(test_every_refusal_has_a_reason);
  RUN_TEST(test_tail_survives_any_chunk_split);
  RUN_TEST(test_gzip_reserves_only_what_the_request_carries);
  RUN_TEST(test_unpacked_image_must_end_below_its_stored_copy);
  RUN_TEST(test_unpacked_length_must_be_sane);
  return UNITY_END();
}
