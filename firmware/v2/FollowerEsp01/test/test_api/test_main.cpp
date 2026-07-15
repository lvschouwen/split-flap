// Host-side tests for the follower's ApiIndex.h (#308): the trimmed /api
// index and the legend-completeness guard — every terse key
// buildUnitHealthJson emits MUST be documented, same discipline as the
// master's test_api (copy policy keeps the overlapping meanings identical).

#include <unity.h>

#include <cstring>

#include "../../ApiIndex.h"
#include "../../UnitHealth.h"
#include "SplitFlapProtocol.h"

void setUp() {}
void tearDown() {}

static void test_api_json_wellformed() {
  char buf[API_JSON_CAP];
  size_t n = buildApiJson(buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_TRUE(n < API_JSON_CAP);
  TEST_ASSERT_EQUAL_size_t(n, strlen(buf));
  TEST_ASSERT_TRUE(strncmp(buf, "{\"routes\":[", 11) == 0);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"legend\":{"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"/cluster/health\""));
  TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);
  TEST_ASSERT_EQUAL_CHAR('}', buf[n - 2]);
}

static void fullyPopulated(UnitFacts& u) {
  u.state = 1;
  u.fwStatus = 1;
  strcpy(u.version, "abc12345");
  u.statusValid = true;
  u.status.flags = UNIT_FLAG_LAST_HOME_FAILED | UNIT_FLAG_ADDR_EEPROM;
  u.status.mcusrAtBoot = 0x54;
  u.status.lifetimeBrownoutCount = 2;
  u.status.lifetimeWatchdogCount = 1;
  u.status.uptimeSeconds = 1200;
  u.status.badCommandCount = 3;
  u.status.lastHomingStepCount = 720;
  u.odometer = 123456;
  u.odometerValid = true;
  u.physLetter = 12;
  u.driftFlags = UNIT_DRIFT_FLAG_PENDING | UNIT_DRIFT_FLAG_POSITION_KNOWN;
  u.driftEvents = 4;
  u.lastDriftSteps = -7;
  u.diagValid = true;
  u.mismatch = true;
  u.vitals.vccNow_mV = 4980;
  u.vitals.vccMin_mV = 4210;
  u.vitals.cmdPos = 12;
  u.vitals.freeRamMin = 384;
  u.vitalsValid = true;
  // Heartbeat freshness (#310) populated so age/hs2/misses/stale all emit.
  u.lastSeenMs = 1000;
  u.misses = 4;
  u.stale = true;
}

static void assertEveryKeyDocumented(const char* json) {
  size_t len = strlen(json);
  for (size_t i = 0; i < len; i++) {
    if (json[i] != '"') continue;
    char prev = (i == 0) ? '\0' : json[i - 1];
    bool isKey = (prev == '{' || prev == ',');
    size_t j = i + 1;
    char key[32];
    size_t k = 0;
    while (j < len && json[j] != '"' && k < sizeof(key) - 1) key[k++] = json[j++];
    key[k] = '\0';
    if (isKey && j + 1 < len && json[j + 1] == ':') {
      char msg[64];
      snprintf(msg, sizeof(msg), "undocumented key: %s", key);
      TEST_ASSERT_TRUE_MESSAGE(legendHasKey(key), msg);
    }
    i = j;
  }
}

static void test_legend_covers_all_health_keys() {
  UnitFacts units[2];
  fullyPopulated(units[0]);
  fullyPopulated(units[1]);
  char buf[UNIT_HEALTH_JSON_CAP];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 2, 1,
                                 SFP_I2C_ADDRESS_BASE, 5000);
  TEST_ASSERT_TRUE(n > 0 && n < UNIT_HEALTH_JSON_CAP);
  assertEveryKeyDocumented(buf);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_api_json_wellformed);
  RUN_TEST(test_legend_covers_all_health_keys);
  return UNITY_END();
}
