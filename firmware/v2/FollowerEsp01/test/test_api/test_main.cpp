// Host-side tests for the follower's ApiIndex.h (#308): the trimmed /api
// index and the legend-completeness guard — every terse key
// buildUnitHealthJson emits MUST be documented, same discipline as the
// master's test_api (copy policy keeps the overlapping meanings identical).

#include <unity.h>

#include <cstring>

#include "../../ApiIndex.h"
#include "../../FollowerOps.h"  // ReflashProgress/buildReflashJson (#365 capacity check)
#include "UnitHealth.h"
#include "WearPolicy.h"   // WearAssessment/buildWearJson (#365 capacity check)
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

// #365: the follower reads GET_EXT_DIAG on its own bus now (FollowerBus.cpp),
// so extDiagValid can be true here too — unlike err/errAge (#367), which stay
// inert (per-unit I2C attribution is master-only). Mirrors the master's
// combined-splice worst case (test_unit_health.cpp
// test_health_json_combined_splices_fit_cap) against THIS board's local
// /units/health buffer (FollowerWeb.cpp's FOLLOWER_HEALTH_BUF), which is
// deliberately smaller than the master's shared UNIT_HEALTH_JSON_CAP.
static void test_health_json_follower_worst_case_fits_local_buf() {
  constexpr size_t FOLLOWER_HEALTH_BUF = 6144;  // keep in sync w/ FollowerWeb.cpp
  UnitFacts units[16];
  for (int i = 0; i < 16; i++) {
    units[i].state = 1;
    units[i].statusValid = true;
    units[i].fwStatus = 2;
    strcpy(units[i].version, "abc12345");
    units[i].status.flags = 0xFF;
    units[i].status.mcusrAtBoot = 255;
    units[i].status.lifetimeBrownoutCount = 255;
    units[i].status.lifetimeWatchdogCount = 255;
    units[i].status.uptimeSeconds = 65535;
    units[i].status.badCommandCount = 255;
    units[i].status.lastHomingStepCount = 65520;
    units[i].odometer = 0xFFFFFFFEUL;
    units[i].odometerValid = true;
    units[i].diagValid = true;
    units[i].physLetter = 44;
    units[i].driftFlags = 0x03;
    units[i].driftEvents = 255;
    units[i].lastDriftSteps = -127;
    units[i].mismatch = true;
    units[i].vitals.vccNow_mV = 65535;
    units[i].vitals.vccMin_mV = 65535;
    units[i].vitals.cmdPos = 44;
    units[i].vitals.freeRamMin = 65535;
    units[i].vitalsValid = true;
    units[i].misses = 255;
    units[i].stale = true;
    units[i].lastSeenMs = 0;
    // i2cErrors/lastErrorMs stay 0 — FollowerBus.cpp never populates them.
    units[i].extDiagValid = true;
    units[i].extDiag.stepExcessLast = 0xFFFF;
    units[i].extDiag.stepExcessMax = 0xFFFF;
    units[i].extDiag.vccSagLastMove = 0xFFFF;
    units[i].extDiag.hallEdgesLastRev = 0xFF;
    units[i].extDiag.dutyWindow = 0xFFFF;
    units[i].extDiag.statusBits = 0xFF;
  }
  char buf[FOLLOWER_HEALTH_BUF];
  size_t n = buildUnitHealthJson(buf, FOLLOWER_HEALTH_BUF, units, 16, 16,
                                 SFP_I2C_ADDRESS_BASE, 0xFFFFFFFFUL);
  TEST_ASSERT_TRUE(n > 0 && n < FOLLOWER_HEALTH_BUF);
  // The ext-diag block must actually be present at this saturation, or the
  // headroom assertions below are vacuous.
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"se\":65535"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"sb\":255"));

  // Worst wear fragment (hand-filled — assessWear can't actually flag all 16,
  // but the buffer must survive it), same splice arithmetic as FollowerWeb.cpp.
  WearAssessment w;
  w.median = 0xFFFFFFFFUL;
  for (int i = 0; i < 16; i++) w.flagged[i] = true;
  w.flaggedCount = 16;
  char wearJson[96];
  size_t wearLen = buildWearJson(w, wearJson, sizeof(wearJson));
  TEST_ASSERT_TRUE(wearLen > 0 && wearLen < sizeof(wearJson));
  TEST_ASSERT_TRUE(n + wearLen + 2 < FOLLOWER_HEALTH_BUF);
  n += (size_t)snprintf(buf + n - 1, FOLLOWER_HEALTH_BUF - n + 1, ",%s}",
                        wearJson) - 1;

  // Worst reflash fragment: saturated counts + the longest state name.
  ReflashProgress rp;
  rp.state = ReflashState::Cancelled;
  rp.total = 255;
  rp.done = 255;
  rp.failed = 255;
  rp.currentAddr = 255;
  char reflashJson[80];
  buildReflashJson(reflashJson, sizeof(reflashJson), rp);
  TEST_ASSERT_TRUE(n + strlen(reflashJson) + 13 < FOLLOWER_HEALTH_BUF);
  snprintf(buf + n - 1, FOLLOWER_HEALTH_BUF - n + 1, ",\"reflash\":%s}",
           reflashJson);

  TEST_ASSERT_NOT_NULL(strstr(buf, "\"wear\":{"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"reflash\":{"));
  TEST_ASSERT_EQUAL_CHAR('}', buf[strlen(buf) - 1]);
  TEST_ASSERT_TRUE(strlen(buf) < FOLLOWER_HEALTH_BUF);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_api_json_wellformed);
  RUN_TEST(test_legend_covers_all_health_keys);
  RUN_TEST(test_health_json_follower_worst_case_fits_local_buf);
  return UNITY_END();
}
