// Host-side test for the unit facts buffer (FollowerOps.h, #519): a
// saturated row — every key family buildUnitHealthJson can emit, every value
// at its widest — plus the wear and unit-update splices must fit the buffer
// this board allocates for that width.

#include <unity.h>

#include <cstring>

#include "../../FollowerOps.h"
#include "SplitFlapProtocol.h"
#include "UnitHealth.h"
#include "WearPolicy.h"

void setUp() {}
void tearDown() {}

// One saturated row of `width` units against the buffer allocated for that
// width. Returns the document length.
static size_t worstCaseFitsFor(int width) {
  const size_t FOLLOWER_HEALTH_BUF = followerHealthBufCap(width, 16);
  UnitFacts units[16];
  for (int i = 0; i < width; i++) {
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
    units[i].offset = -32768;  // widest "ofs" field
    units[i].offsetValid = true;
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
    units[i].rescueExits = 65535;  // #498: set by the follower's lost-unit rescue
    units[i].extDiag.stepExcessLast = 0xFFFF;
    units[i].extDiag.stepExcessMax = 0xFFFF;
    units[i].extDiag.vccSagLastMove = 0xFFFF;
    units[i].extDiag.hallEdgesLastRev = 0xFF;
    units[i].extDiag.dutyWindow = 0xFFFF;
    units[i].extDiag.statusBits = 0xFF;
    // #502 link-health keys at their widest (10-digit uptime).
    units[i].linkValid = true;
    units[i].resetSeen = true;  // #502: the rs key
    units[i].link.uptimeSeconds = 0xFFFFFFFFUL;
    units[i].link.rxFrames = 0xFFFF;
    units[i].link.txReplies = 0xFFFF;
    units[i].link.deafHeals = 0xFF;
    units[i].bootVerdict = BOOT_INTEGRITY_CORRUPT;  // #520: bv + bcrc
    units[i].bootCrc32 = 0xFFFFFFFFUL;
    // #411: the #405 protocol keys and #406 lifetime keys — this fixture
    // omitting them is exactly how the 6144 buffer went stale unnoticed.
    units[i].protocolKnown = true;
    units[i].protocolVersion = 255;  // unsupported → emits pv AND pmm
    units[i].lifetimeValid = true;
    units[i].lifetime.homeFailedCount = 255;
    units[i].lifetime.featureGates = 255;
    units[i].lifetime.stepExcessLifetimeMax = 0xFFFF;
    units[i].lifetime.selfTestFirstHallWindow = 0xFFFF;
    units[i].lifetime.selfTestFirstStepsPerRev = 0xFFFF;
    units[i].lifetime.selfTestLastHallWindow = 0xFFFF;
    units[i].lifetime.selfTestLastStepsPerRev = 0xFFFF;
  }
  static char buf[16384];
  TEST_ASSERT_TRUE(FOLLOWER_HEALTH_BUF <= sizeof(buf));
  size_t n = buildUnitHealthJson(buf, FOLLOWER_HEALTH_BUF, units, width, width,
                                 SFP_I2C_ADDRESS_BASE, 0xFFFFFFFFUL);
  TEST_ASSERT_TRUE(n > 0 && n < FOLLOWER_HEALTH_BUF);
  // The ext-diag block must actually be present at this saturation, or the
  // headroom assertions below are vacuous.
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"se\":65535"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"sb\":255"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"ut\":4294967295,\"rx\":65535,\"tx\":65535,\"dh\":255"));

  // Worst wear fragment (hand-filled — assessWear can't actually flag all 16,
  // but the buffer must survive it), same splice arithmetic as FollowerUnitJobs.cpp.
  WearAssessment w;
  w.median = 0xFFFFFFFFUL;
  for (int i = 0; i < width; i++) w.flagged[i] = true;
  w.flaggedCount = width;
  char wearJson[96];
  size_t wearLen = buildWearJson(w, wearJson, sizeof(wearJson));
  TEST_ASSERT_TRUE(wearLen > 0 && wearLen < sizeof(wearJson));
  TEST_ASSERT_TRUE(n + wearLen + 2 < FOLLOWER_HEALTH_BUF);
  n += (size_t)snprintf(buf + n - 1, FOLLOWER_HEALTH_BUF - n + 1, ",%s}",
                        wearJson) - 1;

  // Worst reflash fragment: saturated counts + the longest state name.
  ReflashProgress rp;
  rp.state = ReflashState::BootUpdate;
  rp.total = 255;
  rp.done = 255;
  rp.failed = 255;
  rp.currentAddr = 255;
  rp.bootDone = 255;
  rp.bootFailed = 255;
  char reflashJson[REFLASH_JSON_CAP];
  buildReflashJson(reflashJson, sizeof(reflashJson), rp);
  TEST_ASSERT_TRUE(n + strlen(reflashJson) + 13 < FOLLOWER_HEALTH_BUF);
  snprintf(buf + n - 1, FOLLOWER_HEALTH_BUF - n + 1, ",\"reflash\":%s}",
           reflashJson);

  TEST_ASSERT_NOT_NULL(strstr(buf, "\"wear\":{"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"reflash\":{"));
  TEST_ASSERT_EQUAL_CHAR('}', buf[strlen(buf) - 1]);
  TEST_ASSERT_TRUE(strlen(buf) < FOLLOWER_HEALTH_BUF);
  return strlen(buf);
}

static void test_health_json_follower_worst_case_fits_local_buf() {
  size_t widest = 0;
  for (int width = 1; width <= 16; width++) widest = worstCaseFitsFor(width);
  // The sizing must be tight enough to be worth having: a saturated 16-unit
  // row uses most of its buffer, and a 5-unit row gets well under half of
  // the 8 KB a 16-unit worst case would take.
  TEST_ASSERT_TRUE(widest * 10 > followerHealthBufCap(16, 16) * 8);
  TEST_ASSERT_TRUE(followerHealthBufCap(5, 16) < 4096);
  // Out-of-range widths clamp instead of under- or over-allocating.
  TEST_ASSERT_EQUAL_size_t(followerHealthBufCap(0, 16), followerHealthBufCap(-3, 16));
  TEST_ASSERT_EQUAL_size_t(followerHealthBufCap(16, 16), followerHealthBufCap(99, 16));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_health_json_follower_worst_case_fits_local_buf);
  return UNITY_END();
}
