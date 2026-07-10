// Host-side unit tests for FactoryChunkPlan.h (#195) — the pure split of a
// rescue-upload chunk into the RAM-held header sector vs the flash write
// behind it. FactorySlot holds flash sector 0 back until the MD5 verdict, so
// a torn upload can never leave a bootable-looking image; this arithmetic is
// what routes each byte.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../FactoryChunkPlan.h"

void setUp() {}
void tearDown() {}

static const size_t SECTOR = 4096;

static void test_chunk_entirely_inside_header_sector() {
  FactoryChunkPlan p = planFactoryChunk(0, 1460, SECTOR);
  TEST_ASSERT_EQUAL_UINT32(1460, p.headerBytes);
  TEST_ASSERT_EQUAL_UINT32(0, p.flashBytes);
  TEST_ASSERT_EQUAL_UINT32(0, p.eraseTo);  // nothing to erase for a RAM copy
}

static void test_chunk_filling_header_exactly_writes_no_flash() {
  FactoryChunkPlan p = planFactoryChunk(2048, 2048, SECTOR);
  TEST_ASSERT_EQUAL_UINT32(2048, p.headerBytes);
  TEST_ASSERT_EQUAL_UINT32(0, p.flashBytes);
}

static void test_chunk_straddling_boundary_splits() {
  FactoryChunkPlan p = planFactoryChunk(3000, 2000, SECTOR);
  TEST_ASSERT_EQUAL_UINT32(1096, p.headerBytes);  // fills 3000..4096
  TEST_ASSERT_EQUAL_UINT32(SECTOR, p.flashOffset);
  TEST_ASSERT_EQUAL_UINT32(904, p.flashBytes);  // remainder lands at 4096
  TEST_ASSERT_EQUAL_UINT32(2 * SECTOR, p.eraseTo);
}

static void test_chunk_entirely_beyond_header_goes_to_flash() {
  FactoryChunkPlan p = planFactoryChunk(10000, 1460, SECTOR);
  TEST_ASSERT_EQUAL_UINT32(0, p.headerBytes);
  TEST_ASSERT_EQUAL_UINT32(10000, p.flashOffset);
  TEST_ASSERT_EQUAL_UINT32(1460, p.flashBytes);
  TEST_ASSERT_EQUAL_UINT32(3 * SECTOR, p.eraseTo);  // 10000+1460=11460 < 12288
}

static void test_flash_write_ending_on_sector_boundary_erases_exactly() {
  FactoryChunkPlan p = planFactoryChunk(SECTOR, SECTOR, SECTOR);
  TEST_ASSERT_EQUAL_UINT32(0, p.headerBytes);
  TEST_ASSERT_EQUAL_UINT32(SECTOR, p.flashOffset);
  TEST_ASSERT_EQUAL_UINT32(SECTOR, p.flashBytes);
  TEST_ASSERT_EQUAL_UINT32(2 * SECTOR, p.eraseTo);
}

static void test_offsets_accumulate_across_a_stream() {
  // Simulate three TCP-sized chunks and check the bytes are contiguous.
  size_t offset = 0;
  size_t headerFilled = 0;
  size_t flashWritten = 0;
  const size_t chunks[] = {1460, 1460, 1460, 1460};
  for (size_t len : chunks) {
    FactoryChunkPlan p = planFactoryChunk(offset, len, SECTOR);
    TEST_ASSERT_EQUAL_UINT32(len, p.headerBytes + p.flashBytes);
    if (p.flashBytes > 0) {
      TEST_ASSERT_EQUAL_UINT32(offset + p.headerBytes, p.flashOffset);
    }
    headerFilled += p.headerBytes;
    flashWritten += p.flashBytes;
    offset += len;
  }
  TEST_ASSERT_EQUAL_UINT32(SECTOR, headerFilled);           // header full
  TEST_ASSERT_EQUAL_UINT32(4 * 1460 - SECTOR, flashWritten);  // rest to flash
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_chunk_entirely_inside_header_sector);
  RUN_TEST(test_chunk_filling_header_exactly_writes_no_flash);
  RUN_TEST(test_chunk_straddling_boundary_splits);
  RUN_TEST(test_chunk_entirely_beyond_header_goes_to_flash);
  RUN_TEST(test_flash_write_ending_on_sector_boundary_erases_exactly);
  RUN_TEST(test_offsets_accumulate_across_a_stream);
  return UNITY_END();
}
