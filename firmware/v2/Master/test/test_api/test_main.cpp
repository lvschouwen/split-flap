// Host-side tests for ApiIndex.h (#307): the self-documenting /api index and
// the legend-completeness guard — every terse key buildUnitHealthJson emits
// MUST have a legend entry, so the headless legend can never silently drift
// from the data it documents.

#include <unity.h>

#include <cstring>

#include "../../ApiIndex.h"
#include "../../UnitHealth.h"
#include "SplitFlapProtocol.h"

void setUp() {}
void tearDown() {}

// --- /api JSON well-formed ---------------------------------------------------

static void test_api_json_wellformed() {
  char buf[API_JSON_CAP];
  size_t n = buildApiJson(buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_TRUE(n < API_JSON_CAP);  // fits, not truncated
  TEST_ASSERT_EQUAL_size_t(n, strlen(buf));
  // Structural anchors.
  TEST_ASSERT_TRUE(strncmp(buf, "{\"routes\":[", 11) == 0);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"legend\":{"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"/units/health\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"/api\""));
  // Ends with the two closing braces.
  TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);
  TEST_ASSERT_EQUAL_CHAR('}', buf[n - 2]);
}

// --- legend-completeness guard ----------------------------------------------

// A unit with EVERY optional health block populated, so buildUnitHealthJson
// emits every key it can. The guard then proves each one is documented.
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
  // Ext-diag (#365) populated so se/sx/sag/he/dw/sb all emit — without this
  // the legend-completeness guard below never sees these keys and can't
  // catch them going undocumented.
  u.extDiag.stepExcessLast = 3;
  u.extDiag.stepExcessMax = 9;
  u.extDiag.vccSagLastMove = 4600;
  u.extDiag.hallEdgesLastRev = 1;
  u.extDiag.dutyWindow = 12;
  u.extDiag.statusBits = 1;
  u.extDiagValid = true;
}

// Extract each JSON object key (a quoted token immediately after '{' or ',')
// and assert the legend documents it. String *values* (after ':') are skipped.
static void assertEveryKeyDocumented(const char* json) {
  size_t len = strlen(json);
  for (size_t i = 0; i < len; i++) {
    if (json[i] != '"') continue;
    // Preceding non-space char decides key vs value.
    char prev = (i == 0) ? '\0' : json[i - 1];
    bool isKey = (prev == '{' || prev == ',');
    // Read the token.
    size_t j = i + 1;
    char key[32];
    size_t k = 0;
    while (j < len && json[j] != '"' && k < sizeof(key) - 1) key[k++] = json[j++];
    key[k] = '\0';
    if (isKey) {
      // The token is a key; the character after the closing quote is ':'.
      if (j + 1 < len && json[j + 1] == ':') {
        char msg[64];
        snprintf(msg, sizeof(msg), "undocumented key: %s", key);
        TEST_ASSERT_TRUE_MESSAGE(legendHasKey(key), msg);
      }
    }
    i = j;  // resume after the closing quote
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

// The version string value "abc12345" must NOT be mistaken for a key by the
// scanner (it follows ':', not '{'/','). Guards the scanner itself.
static void test_scanner_ignores_string_values() {
  TEST_ASSERT_FALSE(legendHasKey("abc12345"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_api_json_wellformed);
  RUN_TEST(test_legend_covers_all_health_keys);
  RUN_TEST(test_scanner_ignores_string_values);
  return UNITY_END();
}
