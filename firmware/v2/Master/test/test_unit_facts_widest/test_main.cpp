// The buffer a facts document gets (UNIT_FACTS_DOC_CAP) against the widest
// document the serializer can write (#567).

#include <unity.h>

#include <cstdio>
#include <cstring>

#include "UnitFactsWidest.h"
#include "UnitHealth.h"

void setUp() {}
void tearDown() {}

static char doc[32768];

static size_t widestDoc(int width, bool inBootloader) {
  const size_t n = unitFactsWidestDoc(doc, sizeof doc, width, inBootloader);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_size_t(n, strlen(doc));
  return n;
}

static bool docHasKey(const char* key) {
  char quoted[24];
  snprintf(quoted, sizeof quoted, "\"%s\":", key);
  return strstr(doc, quoted) != nullptr;
}

static void test_every_key_is_in_one_of_the_two_widest_documents() {
  bool seen[UNIT_FACTS_KEY_COUNT] = {};
  for (int pass = 0; pass < 2; pass++) {
    widestDoc(16, pass == 1);
    for (size_t k = 0; k < UNIT_FACTS_KEY_COUNT; k++) {
      if (docHasKey(UNIT_FACTS_KEYS[k])) seen[k] = true;
    }
  }
  for (size_t k = 0; k < UNIT_FACTS_KEY_COUNT; k++) {
    TEST_ASSERT_TRUE_MESSAGE(seen[k], UNIT_FACTS_KEYS[k]);
  }
}

static void test_a_unit_in_its_firmware_is_the_wider_of_the_two() {
  const size_t firmware = widestDoc(16, false);
  const size_t bootloader = widestDoc(16, true);
  TEST_ASSERT_TRUE(firmware > bootloader);
}

static void test_the_widest_document_fits_at_every_width_and_leaves_little_over() {
  for (int width = 1; width <= 16; width++) {
    const size_t cap = UNIT_FACTS_DOC_CAP(width);
    // Built into exactly the buffer a board has, with the boards' own checks
    // before each splice.
    const size_t n = unitFactsWidestDoc(doc, cap, width, false);
    char what[64];
    snprintf(what, sizeof what, "width %d: %u of %u bytes", width, (unsigned)n, (unsigned)cap);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, what);
    TEST_ASSERT_TRUE_MESSAGE(cap - n <= 16, what);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_key_is_in_one_of_the_two_widest_documents);
  RUN_TEST(test_a_unit_in_its_firmware_is_the_wider_of_the_two);
  RUN_TEST(test_the_widest_document_fits_at_every_width_and_leaves_little_over);
  return UNITY_END();
}
