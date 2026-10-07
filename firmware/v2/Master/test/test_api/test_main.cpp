// Host-side tests for ApiIndex.h (#307): the self-documenting /api index.

#include <unity.h>

#include <cstring>

#include "../../ApiIndex.h"

void setUp() {}
void tearDown() {}

static void test_api_json_wellformed() {
  char buf[API_JSON_CAP];
  size_t n = buildApiJson(buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_TRUE(n < API_JSON_CAP);  // fits, not truncated
  TEST_ASSERT_EQUAL_size_t(n, strlen(buf));
  TEST_ASSERT_TRUE(strncmp(buf, "{\"routes\":[", 11) == 0);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"/api/v2/wall\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"/api\""));
  TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);
  TEST_ASSERT_EQUAL_CHAR(']', buf[n - 2]);
}

// A reply that does not fit reports its would-be length, so the handler
// answers 500 instead of sending a cut document.
static void test_api_json_reports_a_buffer_too_small() {
  char full[API_JSON_CAP];
  size_t n = buildApiJson(full, sizeof(full));
  char small[64];
  TEST_ASSERT_TRUE(buildApiJson(small, sizeof(small)) >= sizeof(small));
  TEST_ASSERT_TRUE(n > sizeof(small));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_api_json_wellformed);
  RUN_TEST(test_api_json_reports_a_buffer_too_small);
  return UNITY_END();
}
