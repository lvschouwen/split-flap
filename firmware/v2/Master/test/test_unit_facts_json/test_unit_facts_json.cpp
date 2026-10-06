// Native tests for UnitFactsJson.h: a row board's unit facts, read back from
// the JSON the shared serializer wrote into the one UnitFacts struct.
//
// The proof is a round trip: what the reader makes of a document, written out
// again by the same serializer at the same instant, is the document.
#include <ArduinoFake.h>
#include <unity.h>

#include <cstring>

#include "../../UnitFactsJson.h"

void setUp() {}
void tearDown() {}

static char first[UNIT_HEALTH_JSON_CAP];
static char second[UNIT_HEALTH_JSON_CAP];

static void assertRoundTrip(const UnitFacts* units, int width, int faulty, uint32_t nowMs) {
  const size_t n = buildUnitHealthJson(first, sizeof first, units, width, faulty, 1, nowMs);
  char size[32];
  snprintf(size, sizeof size, "document of %u bytes", (unsigned)n);
  TEST_ASSERT_TRUE_MESSAGE(n > 0 && n < sizeof first, size);
  UnitFactsDoc read;
  TEST_ASSERT_TRUE_MESSAGE(unitFactsFromJson(first, n, nowMs, read), first);
  TEST_ASSERT_EQUAL(width, read.width);
  TEST_ASSERT_EQUAL(faulty, read.faulty);
  buildUnitHealthJson(second, sizeof second, read.units, read.width, read.faulty, 1, nowMs);
  TEST_ASSERT_EQUAL_STRING(first, second);
}

static UnitFacts saturated() {
  UnitFacts u;
  u.state = 1;
  u.statusValid = true;
  u.fwStatus = 1;
  strcpy(u.version, "abc12345");
  u.status.flags = 0xFF;
  u.status.mcusrAtBoot = 255;
  u.status.lifetimeBrownoutCount = 255;
  u.status.lifetimeWatchdogCount = 255;
  u.status.uptimeSeconds = 65535;
  u.status.badCommandCount = 255;
  u.status.lastHomingStepCount = 65520;
  u.resetSeen = true;
  u.odometer = 0xFFFFFFFEUL;
  u.odometerValid = true;
  u.offset = -32768;
  u.offsetValid = true;
  u.diagValid = true;
  u.physLetter = 44;
  u.driftFlags = UNIT_DRIFT_FLAG_PENDING | UNIT_DRIFT_FLAG_POSITION_KNOWN;
  u.driftEvents = 255;
  u.lastDriftSteps = -127;
  u.mismatch = true;
  u.vitalsValid = true;
  u.vitals.vccNow_mV = 5021;
  u.vitals.vccMin_mV = 4310;
  u.vitals.cmdPos = 44;
  u.vitals.freeRamMin = 1234;
  u.extDiagValid = true;
  u.extDiag.stepExcessLast = 0xFFFF;
  u.extDiag.stepExcessMax = 0xFFFE;
  u.extDiag.vccSagLastMove = 0xFFFD;
  u.extDiag.hallEdgesLastRev = 0xFF;
  u.extDiag.dutyWindow = 0xFFFC;
  u.extDiag.statusBits = 0xFF;
  u.linkValid = true;
  u.link.uptimeSeconds = 0xFFFFFFFFUL;
  u.link.rxFrames = 0xFFFF;
  u.link.txReplies = 0xFFFE;
  u.link.deafHeals = 0xFF;
  u.protocolKnown = true;
  u.protocolVersion = 200;  // one this build does not speak: the pmm key too
  u.lifetimeValid = true;
  u.lifetime.homeFailedCount = 0xFF;
  u.lifetime.featureGates = 0xFE;
  u.lifetime.stepExcessLifetimeMax = 0xFFFF;
  u.lifetime.selfTestFirstHallWindow = 0xFFFF;
  u.lifetime.selfTestFirstStepsPerRev = 0xFFFE;
  u.lifetime.selfTestLastHallWindow = 0xFFFD;
  u.lifetime.selfTestLastStepsPerRev = 0xFFFC;
  u.lifetime.idleHallFutileRehomes = LIFETIME_FUTILE_REHOME_MAX;
  u.lifetime.idleHallStoodDown = true;
  u.lastSeenMs = 1000;
  u.misses = 255;
  u.stale = true;
  u.i2cErrors = 0xFFFF;
  u.lastErrorMs = 2000;
  u.rescueExits = 0xFFFF;
  u.bootVerdict = BOOT_INTEGRITY_CORRUPT;
  u.bootCrc32 = 0x00C0FFEEUL;  // leading zeros must survive
  return u;
}

static void test_every_key_family_at_its_widest_survives_the_round_trip() {
  // Twelve of them: sixteen units with every family at its widest at once do
  // not fit UNIT_HEALTH_JSON_CAP (#567).
  static UnitFacts units[UNITS_AMOUNT];
  for (auto& u : units) u = saturated();
  assertRoundTrip(units, 12, 12, 0xFFFFFFF0UL);
}

static void test_a_plain_working_unit_and_empty_columns() {
  static UnitFacts units[UNITS_AMOUNT];
  for (auto& u : units) u = UnitFacts{};
  units[0].state = 1;
  units[0].statusValid = true;
  units[0].fwStatus = 0;
  strcpy(units[0].version, "d360e2b");
  units[0].status.flags = UNIT_FLAG_HOMED;
  units[0].status.lastHomingStepCount = 2032;
  units[0].lastSeenMs = 99000;
  units[0].bootVerdict = BOOT_INTEGRITY_OK;
  units[0].offsetValid = true;
  units[0].offset = 0;
  units[0].lifetimeValid = true;  // valid and all zero: no key at all
  assertRoundTrip(units, 5, 0, 100000);
}

// A unit that stopped answering: its last read failed, so the status keys are
// gone, but the heartbeat block still says how far it was at its last one.
static void test_a_lost_unit_keeps_its_freshness_and_its_homing_state() {
  for (uint8_t flags : {(uint8_t)0, (uint8_t)UNIT_FLAG_MOVING, (uint8_t)UNIT_FLAG_HOMED}) {
    static UnitFacts units[UNITS_AMOUNT];
    for (auto& u : units) u = UnitFacts{};
    units[2].state = 1;
    units[2].statusValid = false;
    units[2].status.flags = flags;
    units[2].lastSeenMs = 40000;
    units[2].misses = 3;
    units[2].stale = true;
    assertRoundTrip(units, 4, 1, 100000);
  }
}

static void test_a_unit_sitting_in_its_bootloader() {
  static UnitFacts units[UNITS_AMOUNT];
  for (auto& u : units) u = UnitFacts{};
  units[1].state = 2;
  units[1].bootloader.generation = 3;
  units[1].bootloader.caps = 0x07;
  units[1].bootloader.fusesValid = true;
  units[1].bootloader.lock = 0x0F;
  units[1].bootloader.lfuse = 0xFF;
  units[1].bootloader.hfuse = 0xDA;
  units[1].bootloader.efuse = 0x05;
  units[1].bootloader.crashValid = true;
  units[1].bootloader.crashCount = 3;
  units[3].state = 2;
  units[3].bootloader.generation = TWIBOOT_GEN_NO_IDENTITY;
  assertRoundTrip(units, 4, 1, 5000);
}

static void test_the_calibration_offset_arrives() {
  static UnitFacts units[UNITS_AMOUNT];
  for (auto& u : units) u = UnitFacts{};
  units[0].state = 1;
  units[0].offsetValid = true;
  units[0].offset = -37;
  const size_t n = buildUnitHealthJson(first, sizeof first, units, 1, 0, 1, 0);
  UnitFactsDoc read;
  TEST_ASSERT_TRUE(unitFactsFromJson(first, n, 0, read));
  TEST_ASSERT_TRUE(read.units[0].offsetValid);
  TEST_ASSERT_EQUAL(-37, read.units[0].offset);
  TEST_ASSERT_FALSE(read.units[0].odometerValid);
}

// Ages are counted on the row's clock; the master keeps them against its own,
// from the moment the document arrived.
static void test_ages_are_kept_against_the_moment_of_arrival() {
  static UnitFacts units[UNITS_AMOUNT];
  for (auto& u : units) u = UnitFacts{};
  units[0].state = 1;
  units[0].lastSeenMs = 9000;   // 1 s before the row wrote the document
  units[0].i2cErrors = 2;
  units[0].lastErrorMs = 4000;  // 6 s before
  const size_t n = buildUnitHealthJson(first, sizeof first, units, 1, 0, 1, 10000);
  UnitFactsDoc read;
  TEST_ASSERT_TRUE(unitFactsFromJson(first, n, 500000, read));
  TEST_ASSERT_EQUAL_UINT32(499000, read.units[0].lastSeenMs);
  TEST_ASSERT_EQUAL_UINT32(494000, read.units[0].lastErrorMs);
}

static void test_what_is_not_a_unit_facts_document_is_refused() {
  UnitFactsDoc read;
  const char* bad[] = {
      "",
      "not json",
      "[1,2,3]",
      "{\"width\":2,\"faulty\":0}",                                  // no units
      "{\"width\":2,\"faulty\":0,\"units\":[{\"i\":0,\"a\":1,\"st\":1,\"v\":0}]}",  // one short
      "{\"width\":17,\"faulty\":0,\"units\":[]}",                    // wider than a row
      "{\"width\":1,\"faulty\":0,\"units\":[{\"i\":1,\"a\":2,\"st\":1,\"v\":0}]}",  // wrong column
      "{\"width\":1,\"faulty\":0,\"units\":[{\"i\":0,\"a\":1,\"st\":9,\"v\":0}]}",  // no such state
      "{\"width\":1,\"faulty\":0,\"units\":[{\"i\":0,\"a\":1,\"st\":1,\"v\":1,\"rev\":\"far-too-long-a-rev\"}]}",
      "{\"width\":1,\"faulty\":0,\"units\":[7]}",
  };
  for (const char* doc : bad) {
    TEST_ASSERT_FALSE_MESSAGE(unitFactsFromJson(doc, strlen(doc), 0, read), doc);
  }
}

// A row on a newer build may say more than this master knows.
static void test_keys_this_build_does_not_know_are_ignored() {
  const char* doc =
      "{\"width\":1,\"faulty\":0,\"later\":{\"a\":[1,2]},"
      "\"units\":[{\"i\":0,\"a\":1,\"st\":1,\"v\":0,\"age\":10,\"hs2\":2,\"zz\":\"new\"}]}";
  UnitFactsDoc read;
  TEST_ASSERT_TRUE(unitFactsFromJson(doc, strlen(doc), 1000, read));
  TEST_ASSERT_EQUAL(1, read.units[0].state);
  TEST_ASSERT_EQUAL_UINT32(990, read.units[0].lastSeenMs);
}

static void test_a_value_too_large_for_its_field_refuses_the_document() {
  const char* doc =
      "{\"width\":1,\"faulty\":0,\"units\":[{\"i\":0,\"a\":1,\"st\":1,\"v\":0,\"vcc\":70000,"
      "\"vmin\":1,\"cp\":1,\"ram\":1,\"age\":1,\"hs2\":2}]}";
  UnitFactsDoc read;
  TEST_ASSERT_FALSE(unitFactsFromJson(doc, strlen(doc), 0, read));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_key_family_at_its_widest_survives_the_round_trip);
  RUN_TEST(test_a_plain_working_unit_and_empty_columns);
  RUN_TEST(test_a_lost_unit_keeps_its_freshness_and_its_homing_state);
  RUN_TEST(test_a_unit_sitting_in_its_bootloader);
  RUN_TEST(test_the_calibration_offset_arrives);
  RUN_TEST(test_ages_are_kept_against_the_moment_of_arrival);
  RUN_TEST(test_what_is_not_a_unit_facts_document_is_refused);
  RUN_TEST(test_keys_this_build_does_not_know_are_ignored);
  RUN_TEST(test_a_value_too_large_for_its_field_refuses_the_document);
  return UNITY_END();
}
