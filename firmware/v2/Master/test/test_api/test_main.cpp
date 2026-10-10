// Host-side tests for ApiIndex.h (#307): the self-documenting /api index.

#include <unity.h>

#include <cstring>

#include <ArduinoJson.h>

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

// What a reader gets back is what the table says, whatever a description
// holds: a quote or a backslash in one must not end the document early.
static void test_api_json_parses_and_gives_every_description_back() {
  char buf[API_JSON_CAP];
  TEST_ASSERT_TRUE(buildApiJson(buf, sizeof(buf)) < sizeof(buf));
  JsonDocument doc;
  TEST_ASSERT_TRUE(deserializeJson(doc, (const char*)buf) == DeserializationError::Ok);
  JsonArray routes = doc["routes"];
  TEST_ASSERT_EQUAL_INT(API_ROUTES_COUNT, (int)routes.size());
  for (int i = 0; i < API_ROUTES_COUNT; i++) {
    TEST_ASSERT_EQUAL_STRING(API_ROUTES[i].m, routes[i]["m"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING(API_ROUTES[i].p, routes[i]["p"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING(API_ROUTES[i].d, routes[i]["d"].as<const char*>());
  }
}

static void test_a_quote_and_a_backslash_are_escaped() {
  char out[32];
  size_t n = apiJsonEscape(out, sizeof(out), 0, "a \"b\" \\c");
  TEST_ASSERT_EQUAL_STRING("a \\\"b\\\" \\\\c", out);
  TEST_ASSERT_EQUAL_size_t(strlen(out), n);
  // Past the end it still counts, and writes nothing out of bounds.
  char tiny[4] = {'x', 'x', 'x', 'x'};
  TEST_ASSERT_EQUAL_size_t(6, apiJsonEscape(tiny, 3, 0, "ab\"cd"));
  TEST_ASSERT_EQUAL_CHAR('x', tiny[3]);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_api_json_wellformed);
  RUN_TEST(test_api_json_reports_a_buffer_too_small);
  RUN_TEST(test_api_json_parses_and_gives_every_description_back);
  RUN_TEST(test_a_quote_and_a_backslash_are_escaped);
  return UNITY_END();
}
