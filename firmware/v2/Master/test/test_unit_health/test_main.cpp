// Host-side tests for UnitHealth.h (#203) — v2 copy of v1's pure unit-health
// logic (per the copy policy: v2 pure headers are natively-tested copies).
// Covers the faulty predicate, the faulty count over UnitFacts, the health
// JSON builder (same wire shape as v1's /units/health), the CMD_GET_LETTER
// readback validation, and the twiboot chipinfo signature check.

#include <ArduinoFake.h>
#include <unity.h>

#include <cstring>

#include "HeartbeatPolicy.h"
#include "ReflashPlan.h"
#include "TwibootProtocol.h"
#include "UnitHealth.h"
#include "UnitProtocolHelpers.h"
#include "WearPolicy.h"
#include "SplitFlapProtocol.h"

void setUp() {}
void tearDown() {}

// --- unitStatusIsFaulty ------------------------------------------------------

static void test_clean_status_is_not_faulty() {
  UnitStatus s{};
  TEST_ASSERT_FALSE(unitStatusIsFaulty(s));
}

static void test_home_failed_flag_is_faulty() {
  UnitStatus s{};
  s.flags = UNIT_FLAG_LAST_HOME_FAILED;
  TEST_ASSERT_TRUE(unitStatusIsFaulty(s));
}

static void test_hall_never_flag_is_faulty() {
  UnitStatus s{};
  s.flags = UNIT_FLAG_HALL_NEVER;
  TEST_ASSERT_TRUE(unitStatusIsFaulty(s));
}

// The lifetime counters are history: every unit carries the one reset a
// firmware campaign leaves behind, and that must not read as 21 faults.
static void test_lifetime_reset_counters_alone_are_not_faulty() {
  UnitStatus s{};
  s.lifetimeBrownoutCount = 1;
  TEST_ASSERT_FALSE(unitStatusIsFaulty(s));
  UnitStatus w{};
  w.lifetimeWatchdogCount = 3;
  TEST_ASSERT_FALSE(unitStatusIsFaulty(w));
  UnitFacts u;
  u.state = 1;
  u.statusValid = true;
  u.status.lifetimeBrownoutCount = 255;
  u.status.lifetimeWatchdogCount = 255;
  TEST_ASSERT_FALSE(unitIsFaultyOrLost(u));
}

// --- unitResetBaselineFold ---------------------------------------------------

static void test_first_read_sets_the_baseline_and_is_not_a_reset() {
  UnitResetBaseline b;
  TEST_ASSERT_FALSE(unitResetBaselineFold(b, 4, 7));
  TEST_ASSERT_TRUE(b.valid);
  TEST_ASSERT_FALSE(unitResetBaselineFold(b, 4, 7));
}

static void test_a_counter_that_climbs_is_a_reset_seen() {
  UnitResetBaseline b;
  unitResetBaselineFold(b, 0, 1);
  TEST_ASSERT_TRUE(unitResetBaselineFold(b, 0, 2));   // watchdog
  UnitResetBaseline c;
  unitResetBaselineFold(c, 3, 1);
  TEST_ASSERT_TRUE(unitResetBaselineFold(c, 4, 1));   // brownout
}

// The verdict stays for as long as the baseline does: a probe rescan between
// two polls must not make the reset disappear.
static void test_a_reset_seen_stays_seen_on_later_reads() {
  UnitResetBaseline b;
  unitResetBaselineFold(b, 0, 0);
  TEST_ASSERT_TRUE(unitResetBaselineFold(b, 1, 0));
  TEST_ASSERT_TRUE(unitResetBaselineFold(b, 1, 0));
  TEST_ASSERT_TRUE(unitResetBaselineFold(b, 1, 0));
}

static void test_a_cleared_counter_pulls_the_baseline_down() {
  UnitResetBaseline b;
  unitResetBaselineFold(b, 5, 5);
  TEST_ASSERT_FALSE(unitResetBaselineFold(b, 0, 0));  // unit EEPROM re-initialised
  TEST_ASSERT_TRUE(unitResetBaselineFold(b, 0, 1));   // and the next reset counts
}

static void test_a_saturated_counter_cannot_report_a_reset() {
  UnitResetBaseline b;
  unitResetBaselineFold(b, 255, 255);
  TEST_ASSERT_FALSE(unitResetBaselineFold(b, 255, 255));
}

static void test_reset_seen_makes_a_valid_unit_faulty() {
  UnitFacts u;
  u.state = 1;
  u.statusValid = true;
  u.resetSeen = true;
  TEST_ASSERT_TRUE(unitIsFaultyOrLost(u));
  u.statusValid = false;  // never read: cannot be assessed
  TEST_ASSERT_FALSE(unitIsFaultyOrLost(u));
}

static void test_a_corrupt_bootloader_makes_a_unit_faulty() {
  // #520: independent of the status read — the boot report is its own read.
  UnitFacts u;
  u.state = 1;
  u.bootVerdict = BOOT_INTEGRITY_CORRUPT;
  TEST_ASSERT_TRUE(unitIsFaultyOrLost(u));
  u.bootVerdict = BOOT_INTEGRITY_OUTDATED;  // an update is owed, nothing rotted
  TEST_ASSERT_FALSE(unitIsFaultyOrLost(u));
  u.bootVerdict = BOOT_INTEGRITY_OK;
  TEST_ASSERT_FALSE(unitIsFaultyOrLost(u));
  u.bootVerdict = BOOT_INTEGRITY_UNREAD;
  TEST_ASSERT_FALSE(unitIsFaultyOrLost(u));
}

static void test_health_json_boot_verdict_keys() {
  UnitFacts units[1];
  units[0].state = 1;
  char buf[256];
  buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 0);
  TEST_ASSERT_NULL(strstr(buf, "\"bv\""));  // never read: no key at all
  units[0].bootVerdict = BOOT_INTEGRITY_OK;
  units[0].bootCrc32 = BOOT_CURRENT_CRC32;
  buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 0);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"bv\":1"));
  TEST_ASSERT_NULL(strstr(buf, "\"bcrc\""));  // the expected image: no need
  units[0].bootVerdict = BOOT_INTEGRITY_CORRUPT;
  units[0].bootCrc32 = 0x00c0ffeeUL;
  buildUnitHealthJson(buf, sizeof(buf), units, 1, 1, 1, 0);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"bv\":3,\"bcrc\":\"00c0ffee\""));
}

static void test_health_json_bootloader_identity_keys() {
  UnitFacts units[1];
  units[0].state = 2;
  char buf[256];
  buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 0);
  TEST_ASSERT_NULL(strstr(buf, "\"blv\""));  // never read: no key at all
  units[0].bootloader.generation = TWIBOOT_GEN_NO_IDENTITY;
  buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 0);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"blv\":1,\"blc\":0}"));
  units[0].bootloader.generation = 2;
  units[0].bootloader.caps = 0x0B;
  buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 0);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"blv\":2,\"blc\":11}"));  // no fuses served
  units[0].bootloader.fusesValid = true;
  units[0].bootloader.lock = 0xCF;
  units[0].bootloader.lfuse = 0xFF;
  units[0].bootloader.hfuse = 0xDA;
  units[0].bootloader.efuse = 0xFD;
  buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 0);
  TEST_ASSERT_NOT_NULL(
      strstr(buf, "\"blv\":2,\"blc\":11,\"blk\":\"cf\",\"blf\":\"ffdafd\"}"));
  units[0].bootloader.crashValid = true;
  units[0].bootloader.crashCount = 3;
  buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 0);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"blf\":\"ffdafd\",\"blx\":3}"));
  // 1 is the intentional reset of any reflash: not reported.
  units[0].bootloader.crashCount = 1;
  buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 0);
  TEST_ASSERT_NULL(strstr(buf, "\"blx\""));
  units[0].bootloader.crashCount = 3;
  // Held for crashing is a fault; merely sitting in the bootloader is not.
  TEST_ASSERT_TRUE(unitIsFaultyOrLost(units[0]));
  units[0].bootloader.crashCount = 2;
  TEST_ASSERT_FALSE(unitIsFaultyOrLost(units[0]));
  // Back in its application the unit's own boot report is the source again.
  units[0].state = 1;
  buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 0);
  TEST_ASSERT_NULL(strstr(buf, "\"blv\""));
}

static void test_bad_commands_alone_are_not_faulty() {
  // Deliberate (#45/#137): a stray malformed I2C receive is not a hardware
  // problem; badCommandCount is surfaced but never counted.
  UnitStatus s{};
  s.badCommandCount = 9;
  TEST_ASSERT_FALSE(unitStatusIsFaulty(s));
}

static void test_addr_eeprom_flag_alone_is_not_faulty() {
  // Address source (#215) is informational — an EEPROM-provisioned address is
  // a provisioning fact, not a hardware fault.
  UnitStatus s{};
  s.flags = UNIT_FLAG_ADDR_EEPROM;
  TEST_ASSERT_FALSE(unitStatusIsFaulty(s));
}

// --- computeFaultyUnitCount --------------------------------------------------

static void test_faulty_count_only_counts_valid_slots() {
  UnitFacts units[3];
  units[0].statusValid = true;   // valid + faulty -> counts
  units[0].status.flags = UNIT_FLAG_LAST_HOME_FAILED;
  units[1].statusValid = false;  // faulty-looking but never read -> ignored
  units[1].status.flags = UNIT_FLAG_LAST_HOME_FAILED;
  units[2].statusValid = true;   // valid + clean -> not counted
  TEST_ASSERT_EQUAL(1, computeFaultyUnitCount(units, 3));
}

static void test_faulty_count_includes_lost_sketch_units() {
  // #497: a sketch unit that stopped answering (heartbeat stale) is the worst
  // fault there is — it must count even though its status read is invalid.
  UnitFacts units[4];
  units[0].state = 1;            // lost: stale, status unreadable -> counts
  units[0].stale = true;
  units[1].state = 2;            // bootloader slot: stale is meaningless
  units[1].stale = true;
  units[2].state = 0;            // empty column
  units[3].state = 1;            // healthy sketch unit
  units[3].statusValid = true;
  TEST_ASSERT_EQUAL(1, computeFaultyUnitCount(units, 4));
  TEST_ASSERT_EQUAL(1, computeLostUnitCount(units, 4));
}

static void test_lost_and_faulty_unit_counts_once() {
  UnitFacts units[1];
  units[0].state = 1;
  units[0].stale = true;
  units[0].statusValid = true;   // last read before it went quiet was faulty
  units[0].status.flags = UNIT_FLAG_HALL_NEVER;
  TEST_ASSERT_EQUAL(1, computeFaultyUnitCount(units, 1));
}

// --- buildUnitHealthJson -----------------------------------------------------

static void test_health_json_reports_rescue_exits_when_nonzero() {
  UnitFacts units[2];
  units[0].state = 1;
  units[0].statusValid = true;
  units[0].rescueExits = 3;  // #498: twiboot exits by the runtime rescue
  units[1].state = 1;
  units[1].statusValid = true;
  char buf[2048];
  buildUnitHealthJson(buf, sizeof(buf), units, 2, 0, 1, 1000);
  String out(buf);
  TEST_ASSERT_TRUE(out.indexOf("\"rsx\":3") >= 0);
  TEST_ASSERT_EQUAL(out.indexOf("\"rsx\""), out.lastIndexOf("\"rsx\""));
}

static void test_health_json_valid_and_bootloader_slots() {
  UnitFacts units[2];
  units[0].state = 1;
  units[0].statusValid = true;
  units[0].fwStatus = 2;
  strcpy(units[0].version, "abc12345");
  units[0].status.flags = 0;
  units[0].status.mcusrAtBoot = 54;
  units[0].status.uptimeSeconds = 1200;
  units[0].status.badCommandCount = 0;
  units[0].status.lastHomingStepCount = 720;
  units[1].state = 2;  // bootloader: bare slot, no health fields

  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 2, 0,
                                 SFP_I2C_ADDRESS_BASE, 0);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  // nowMs=0 and the default lastSeenMs=0 -> age:0; flags 0 -> hs2:0 (unhomed,
  // not moving). misses/stale omitted (0/false), same emit-when-set contract
  // as the drift block.
  TEST_ASSERT_EQUAL_STRING(
      "{\"width\":2,\"faulty\":0,\"units\":["
      "{\"i\":0,\"a\":1,\"st\":1,\"v\":1,\"fw\":2,\"rev\":\"abc12345\","
      "\"up\":1200,\"br\":0,\"wd\":0,\"bc\":0,\"mc\":54,\"fl\":0,\"hs\":720,\"ae\":0,"
      "\"age\":0,\"hs2\":0},"
      "{\"i\":1,\"a\":2,\"st\":2,\"v\":0}]}",
      buf);
}

static void test_health_json_addr_eeprom_bit_surfaces_as_ae() {
  // #215: flags bit 4 (address source = EEPROM) is derived into "ae" so the
  // UI never has to know the bitfield layout. The raw bit stays in "fl" too.
  UnitFacts units[1];
  units[0].state = 1;
  units[0].statusValid = true;
  units[0].status.flags = UNIT_FLAG_ADDR_EEPROM;
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 0);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"fl\":16"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"ae\":1"));
}

static void test_boot_home_state_decode() {
  // #309: hs2 derives from the status flags — not homed & not moving = unhomed,
  // not homed & moving = homing, homed = homed (even while moving).
  TEST_ASSERT_EQUAL_UINT8(0, unitBootHomeState(0));
  TEST_ASSERT_EQUAL_UINT8(1, unitBootHomeState(UNIT_FLAG_MOVING));
  TEST_ASSERT_EQUAL_UINT8(2, unitBootHomeState(UNIT_FLAG_HOMED));
  TEST_ASSERT_EQUAL_UINT8(
      2, unitBootHomeState(UNIT_FLAG_HOMED | UNIT_FLAG_MOVING));
}

static void test_health_json_heartbeat_freshness() {
  // #310/#309: age = nowMs - lastSeenMs; hs2 from the homed/moving bits;
  // misses/stale emitted only when set (emit-when-nonzero, like the drift keys).
  UnitFacts units[3];
  units[0].state = 1; units[0].statusValid = true;  // unhomed, healthy
  units[0].status.flags = 0; units[0].lastSeenMs = 400;
  units[1].state = 1; units[1].statusValid = true;  // homing (moving, !homed)
  units[1].status.flags = UNIT_FLAG_MOVING; units[1].lastSeenMs = 1000;
  units[2].state = 1; units[2].statusValid = true;  // homed + lost
  units[2].status.flags = UNIT_FLAG_HOMED; units[2].lastSeenMs = 500;
  units[2].misses = 3; units[2].stale = true;
  char buf[1024];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 3, 0, 1, 1400);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"age\":1000,\"hs2\":0"));  // unit0
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"hs2\":1"));               // unit1 homing
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"hs2\":2,\"misses\":3,\"stale\":1"));
  // A healthy unit never emits misses/stale.
  char* second = strstr(buf, "\"i\":1");
  TEST_ASSERT_NOT_NULL(second);
  *second = '\0';  // truncate to unit0's object
  TEST_ASSERT_NULL(strstr(buf, "\"stale\""));
  TEST_ASSERT_NULL(strstr(buf, "\"misses\""));
}

static void test_freshness_survives_a_real_miss_streak() {
  // #310 regression guard: a lost unit's current read
  // FAILS (statusValid=false), which is exactly when misses/stale must show.
  // The freshness keys therefore gate on state==1, NOT statusValid — otherwise
  // heartbeatApply can never coexist with statusValid=true and the keys are
  // dead. Drive a real miss streak end-to-end and prove the JSON surfaces it.
  UnitFacts u;
  u.state = 1;
  // A good read first: stamps lastSeenMs, keeps the last-known homed flag.
  u.statusValid = true;
  u.status.flags = UNIT_FLAG_HOMED;
  heartbeatApply(u, true, 1000, HEARTBEAT_MISS_THRESHOLD);
  TEST_ASSERT_EQUAL_UINT8(0, u.misses);
  TEST_ASSERT_FALSE(u.stale);
  // Then three consecutive failed reads — the unit fell off the bus. A failed
  // read leaves statusValid=false (the condition the buggy gate hid).
  u.statusValid = false;
  heartbeatApply(u, false, 4000, HEARTBEAT_MISS_THRESHOLD);
  heartbeatApply(u, false, 7000, HEARTBEAT_MISS_THRESHOLD);
  heartbeatApply(u, false, 10000, HEARTBEAT_MISS_THRESHOLD);
  TEST_ASSERT_EQUAL_UINT8(3, u.misses);
  TEST_ASSERT_TRUE(u.stale);
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), &u, 1, 0, 1, 13000);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  // The lost unit surfaces stale + misses + last-known hs2 despite the failed
  // current read; age = now(13000) - lastSeenMs(1000) = 12000.
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"stale\":1"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"misses\":3"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"hs2\":2"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"age\":12000"));
  // But the status block (fw/rev/up) stays statusValid-gated — absent here.
  TEST_ASSERT_NULL(strstr(buf, "\"rev\":"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"v\":0"));  // "no current status" marker
}

static void test_freshness_gap_slot_never_stale() {
  // A silent gap (state 0) is not a lost unit — heartbeatApply resets it and
  // buildUnitHealthJson emits no freshness keys for it.
  UnitFacts u;
  u.state = 0;
  u.misses = 9;
  u.stale = true;  // stale bookkeeping from a prior life must be cleared
  heartbeatApply(u, false, 5000, HEARTBEAT_MISS_THRESHOLD);
  TEST_ASSERT_EQUAL_UINT8(0, u.misses);
  TEST_ASSERT_FALSE(u.stale);
  char buf[256];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), &u, 1, 0, 1, 6000);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NULL(strstr(buf, "\"stale\""));
  TEST_ASSERT_NULL(strstr(buf, "\"age\""));
}

static void test_health_json_odometer_emitted_when_valid() {
  // #231: "odo" rides its own valid flag, independent of statusValid —
  // a unit can report status but run pre-odometer firmware.
  UnitFacts units[2];
  units[0].state = 1;
  units[0].statusValid = true;
  units[0].odometer = 123456;
  units[0].odometerValid = true;
  units[1].state = 1;
  units[1].statusValid = true;  // no odometer -> no "odo" key
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 2, 0, 1, 0);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"odo\":123456"));
  char* second = strstr(buf, "\"i\":1");
  TEST_ASSERT_NOT_NULL(second);
  TEST_ASSERT_NULL(strstr(second, "\"odo\""));
}

static void test_health_json_offset_emitted_when_valid() {
  // The calibration offset rides its own valid flag, like "odo": a unit
  // whose offset read failed has no "ofs" key rather than a phantom 0.
  UnitFacts units[2];
  units[0].state = 1;
  units[0].statusValid = true;
  units[0].offset = -12;
  units[0].offsetValid = true;
  units[1].state = 1;
  units[1].statusValid = true;
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 2, 0, 1, 0);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"ofs\":-12"));
  char* second = strstr(buf, "\"i\":1");
  TEST_ASSERT_NOT_NULL(second);
  TEST_ASSERT_NULL(strstr(second, "\"ofs\""));
}

// --- odometer readback (SFP_CMD_GET_ODOMETER, #231) --------------------------

static void test_odometer_readback_valid_roundtrip() {
  uint8_t buf[5] = {0x01, 0x02, 0x03, 0x04,
                    (uint8_t)((0x01 ^ 0x02 ^ 0x03 ^ 0x04) ^ 0xA5)};
  uint32_t out = 0;
  TEST_ASSERT_TRUE(odometerReadbackValid(buf, out));
  TEST_ASSERT_EQUAL_UINT32(0x04030201UL, out);
}

static void test_odometer_readback_rejects_old_firmware_garbage() {
  // Pre-odometer firmware answers the unknown opcode with its 1-byte status
  // reply + bus padding: all-0xFF, all-0x00 and repeated 0x01 must all fail.
  uint32_t out;
  uint8_t ff[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t zero[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
  uint8_t busy[5] = {0x01, 0x01, 0x01, 0x01, 0x01};
  TEST_ASSERT_FALSE(odometerReadbackValid(ff, out));
  TEST_ASSERT_FALSE(odometerReadbackValid(zero, out));
  TEST_ASSERT_FALSE(odometerReadbackValid(busy, out));
}

// --- diag readback (SFP_CMD_GET_DIAG, #263/#264) ------------------------------

static void test_diag_readback_valid_roundtrip() {
  // physLetter 7, flags pending|known, 3 events, -30 steps, reserved 0.
  uint8_t buf[6] = {7, 0x03, 3, (uint8_t)(int8_t)-30, 0, 0};
  buf[5] = (uint8_t)((buf[0] ^ buf[1] ^ buf[2] ^ buf[3] ^ buf[4]) ^ 0xB7);
  UnitDiagReading out;
  TEST_ASSERT_TRUE(diagReadbackValid(buf, 45, out));
  TEST_ASSERT_EQUAL_UINT8(7, out.physicalLetter);
  TEST_ASSERT_EQUAL_UINT8(0x03, out.flags);
  TEST_ASSERT_EQUAL_UINT8(3, out.driftEvents);
  TEST_ASSERT_EQUAL_INT8(-30, out.lastDriftSteps);
}

static void test_diag_readback_accepts_unknown_letter_sentinel() {
  uint8_t buf[6] = {0xFF, 0, 0, 0, 0, 0};
  buf[5] = (uint8_t)((buf[0] ^ buf[1] ^ buf[2] ^ buf[3] ^ buf[4]) ^ 0xB7);
  UnitDiagReading out;
  TEST_ASSERT_TRUE(diagReadbackValid(buf, 45, out));
  TEST_ASSERT_EQUAL_UINT8(0xFF, out.physicalLetter);
}

static void test_diag_readback_rejects_old_firmware_garbage() {
  // Pre-diag firmware answers the unknown opcode with 1-byte status + bus
  // padding: all-0xFF, all-0x00 and repeated 0x01 must all fail (#106 class).
  UnitDiagReading out;
  uint8_t ff[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
  uint8_t busy[6] = {1, 1, 1, 1, 1, 1};
  TEST_ASSERT_FALSE(diagReadbackValid(ff, 45, out));
  TEST_ASSERT_FALSE(diagReadbackValid(zero, 45, out));
  TEST_ASSERT_FALSE(diagReadbackValid(busy, 45, out));
}

static void test_diag_readback_rejects_out_of_range_letter() {
  // Checksum-valid but letter 45 with flapAmount 45 → glitched payload.
  uint8_t buf[6] = {45, 0x02, 0, 0, 0, 0};
  buf[5] = (uint8_t)((buf[0] ^ buf[1] ^ buf[2] ^ buf[3] ^ buf[4]) ^ 0xB7);
  UnitDiagReading out;
  TEST_ASSERT_FALSE(diagReadbackValid(buf, 45, out));
}

// --- self-test readback (SFP_CMD_GET_SELF_TEST, #265) --------------------------

static void test_selftest_readback_valid_roundtrip() {
  uint8_t buf[9] = {2, 0xF9, 0x07, 0x5A, 0x00, 0xE8, 0x17, 0x00, 0};
  uint8_t x = 0;
  for (int i = 0; i < 8; i++) x ^= buf[i];
  buf[8] = (uint8_t)(x ^ 0x5C);
  UnitSelfTestReading out;
  TEST_ASSERT_TRUE(selfTestReadbackValid(buf, out));
  TEST_ASSERT_EQUAL_UINT8(2, out.state);
  TEST_ASSERT_EQUAL_UINT16(2041, out.stepsPerRev);
  TEST_ASSERT_EQUAL_UINT16(90, out.hallWindowSteps);
  TEST_ASSERT_EQUAL_UINT16(6120, out.revTimeMs);
}

static void test_selftest_readback_rejects_garbage_and_bad_state() {
  UnitSelfTestReading out;
  uint8_t ff[9] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t zero[9] = {0};
  zero[8] = 0x01;  // break the checksum too
  TEST_ASSERT_FALSE(selfTestReadbackValid(ff, out));
  TEST_ASSERT_FALSE(selfTestReadbackValid(zero, out));
  // Checksum-valid but state out of the 0..3 vocabulary.
  uint8_t badState[9] = {9, 0, 0, 0, 0, 0, 0, 0, 0};
  uint8_t x = 0;
  for (int i = 0; i < 8; i++) x ^= badState[i];
  badState[8] = (uint8_t)(x ^ 0x5C);
  TEST_ASSERT_FALSE(selfTestReadbackValid(badState, out));
}

// --- health JSON: drift fields + mismatch (#263/#264) --------------------------

static void test_health_json_diag_fields_emitted_when_valid() {
  UnitFacts units[2];
  units[0].state = 1;
  units[0].statusValid = true;
  units[0].diagValid = true;
  units[0].physLetter = 7;
  units[0].driftFlags = 0x03;  // pending | position known
  units[0].driftEvents = 4;
  units[0].lastDriftSteps = -30;
  units[1].state = 1;  // no diag read (old firmware): no drift keys at all
  units[1].statusValid = true;
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 2, 0, 1, 0);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"phys\":7"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"de\":4"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"ds\":-30"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"dp\":1"));
  char* second = strstr(buf, "\"i\":1");
  TEST_ASSERT_NOT_NULL(second);
  TEST_ASSERT_NULL(strstr(second, "\"de\""));
}

static void test_health_json_phys_omitted_when_position_unknown() {
  UnitFacts units[1];
  units[0].state = 1;
  units[0].statusValid = true;
  units[0].diagValid = true;
  units[0].physLetter = 0xFF;  // DRIFT_LETTER_UNKNOWN
  units[0].driftFlags = 0;     // position not known
  units[0].driftEvents = 0;
  char buf[256];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 0);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NULL(strstr(buf, "\"phys\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"de\":0"));
}

static void test_health_json_emits_stamped_mismatch_only() {
  // The verdict is stamped by displayApplyUnitFacts at poll time (#267);
  // this layer only serializes it — and only for a known position.
  UnitFacts units[2];
  for (int i = 0; i < 2; i++) {
    units[i].state = 1;
    units[i].statusValid = true;
    units[i].diagValid = true;
    units[i].driftFlags = 0x02;  // position known
  }
  units[0].physLetter = 7;
  units[0].mismatch = false;
  units[1].physLetter = 12;
  units[1].mismatch = true;
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 2, 0, 1, 0);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  char* first = strstr(buf, "\"i\":0");
  char* second = strstr(buf, "\"i\":1");
  TEST_ASSERT_NOT_NULL(first);
  TEST_ASSERT_NOT_NULL(second);
  TEST_ASSERT_TRUE(strstr(buf, "\"mm\":1") > second);  // only on unit 1
  first[second - first] = '\0';  // terminate the first slot's substring
  TEST_ASSERT_NULL(strstr(first, "\"mm\""));
}

static void test_health_json_no_mismatch_without_position() {
  // A stale mismatch stamp must never surface once the position is unknown.
  UnitFacts units[1];
  units[0].state = 1;
  units[0].statusValid = true;
  units[0].diagValid = true;
  units[0].driftFlags = 0;     // position unknown
  units[0].physLetter = 0xFF;
  units[0].mismatch = true;
  char buf[256];
  buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 0);
  TEST_ASSERT_NULL(strstr(buf, "\"mm\""));
}

// --- lifetime block (#406) ---------------------------------------------------

static void test_health_json_lifetime_emitted_when_valid() {
  UnitFacts units[2];
  units[0].state = 1;
  units[0].statusValid = true;
  units[0].lifetimeValid = true;
  units[0].lifetime.homeFailedCount = 4;
  units[0].lifetime.featureGates = 1;
  units[0].lifetime.stepExcessLifetimeMax = 61;
  units[0].lifetime.selfTestFirstHallWindow = 46;
  units[0].lifetime.selfTestLastHallWindow = 12;
  units[0].lifetime.selfTestFirstStepsPerRev = 2038;
  units[0].lifetime.selfTestLastStepsPerRev = 2044;
  units[1].state = 1;
  units[1].statusValid = true;  // old firmware: no lifetime read at all
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 2, 0, 1, 0);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"hf\":4"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"gates\":1"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"sxl\":61"));
  // The first/last pairs are the diagnosis — 46 when new, 12 now.
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"stw0\":46,\"stw1\":12"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"str0\":2038,\"str1\":2044"));
  char* second = strstr(buf, "\"i\":1");
  TEST_ASSERT_NOT_NULL(second);
  TEST_ASSERT_NULL(strstr(second, "\"hf\""));
}

static void test_health_json_fresh_unit_emits_no_lifetime_keys() {
  // A unit that has never failed a homing and never run a self-test reports a
  // valid all-zero record. Emitting seven zero keys for every healthy unit on
  // the wall is pure payload — the emit-when-nonzero guard keeps it lean.
  UnitFacts units[1];
  units[0].state = 1;
  units[0].statusValid = true;
  units[0].lifetimeValid = true;  // valid, but nothing has happened yet
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 0);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NULL(strstr(buf, "\"hf\""));
  TEST_ASSERT_NULL(strstr(buf, "\"gates\""));
  TEST_ASSERT_NULL(strstr(buf, "\"sxl\""));
  TEST_ASSERT_NULL(strstr(buf, "\"stw0\""));
  TEST_ASSERT_NULL(strstr(buf, "\"str0\""));
  TEST_ASSERT_NULL(strstr(buf, "\"fr\""));
  TEST_ASSERT_NULL(strstr(buf, "\"frd\""));
}

// --- idle hall standdown (#460) ----------------------------------------------

static void test_health_json_futile_rehomes_emitted_when_nonzero() {
  // Non-zero means this unit is arguing with its own window model — a real
  // diagnostic about the magnet and the self-test data behind it.
  UnitFacts units[1];
  units[0].state = 1;
  units[0].statusValid = true;
  units[0].lifetimeValid = true;
  units[0].lifetime.idleHallFutileRehomes = 2;
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 0);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"fr\":2"));
  // Still armed — the disarmed flag must not appear merely because it tried.
  TEST_ASSERT_NULL(strstr(buf, "\"frd\""));
}

static void test_health_json_says_plainly_when_the_check_is_disarmed() {
  // The question this exists to answer: "is the idle hall check actually
  // protecting this wall?" A stood-down unit must say so, not merely carry a
  // count the reader has to compare against a limit they do not have.
  UnitFacts units[1];
  units[0].state = 1;
  units[0].statusValid = true;
  units[0].lifetimeValid = true;
  units[0].lifetime.idleHallFutileRehomes = 3;
  units[0].lifetime.idleHallStoodDown = true;
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 0);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"fr\":3"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"frd\":1"));
}

static void test_health_json_self_test_pair_emits_when_only_one_is_set() {
  // A unit whose only self-test failed to measure steps/rev still has a hall
  // window worth reporting; the pair must not vanish because its twin is 0.
  UnitFacts units[1];
  units[0].state = 1;
  units[0].statusValid = true;
  units[0].lifetimeValid = true;
  units[0].lifetime.selfTestFirstHallWindow = 46;
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 0);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"stw0\":46,\"stw1\":0"));
  TEST_ASSERT_NULL(strstr(buf, "\"str0\""));
}

// --- ext-diag block (#365) ---------------------------------------------------

static void test_health_json_ext_diag_emitted_when_valid() {
  UnitFacts units[2];
  units[0].state = 1;
  units[0].statusValid = true;
  units[0].extDiagValid = true;
  units[0].extDiag.stepExcessLast = 12;
  units[0].extDiag.stepExcessMax = 34;
  units[0].extDiag.vccSagLastMove = 4650;
  units[0].extDiag.hallEdgesLastRev = 1;
  units[0].extDiag.dutyWindow = 8;
  units[0].extDiag.statusBits = 0;
  units[1].state = 1;
  units[1].statusValid = true;  // old firmware: no ext-diag read at all
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 2, 0, 1, 0);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"se\":12,\"sx\":34,\"sag\":4650,\"he\":1,\"dw\":8,\"sb\":0"));
  char* second = strstr(buf, "\"i\":1");
  TEST_ASSERT_NOT_NULL(second);
  TEST_ASSERT_NULL(strstr(second, "\"se\""));
}

// --- link-health extension (#502) ---------------------------------------------

static void fillExtReply(uint8_t* buf, bool baseOk, bool linkOk) {
  UnitExtDiag d;
  d.stepExcessLast = 12;
  d.dutyWindow = 8;
  UnitLinkStats l;
  l.uptimeSeconds = 70000;  // past the u16 saturation of status "up"
  l.rxFrames = 500;
  l.txReplies = 499;
  l.deafHeals = 2;
  extDiagEncodeReply(d, buf);
  extDiagLinkEncode(l, buf + EXT_DIAG_REPLY_LEN);
  if (!baseOk) buf[EXT_DIAG_REPLY_LEN - 1] ^= 0x55;
  if (!linkOk) buf[EXT_DIAG_LINK_REPLY_LEN - 1] ^= 0x55;
}

static void test_fold_ext_diag_both_valid() {
  uint8_t buf[EXT_DIAG_LINK_REPLY_LEN];
  fillExtReply(buf, true, true);
  UnitFacts u;
  unitFactsFoldExtDiag(u, buf);
  TEST_ASSERT_TRUE(u.extDiagValid);
  TEST_ASSERT_TRUE(u.linkValid);
  TEST_ASSERT_EQUAL_UINT16(12, u.extDiag.stepExcessLast);
  TEST_ASSERT_EQUAL_UINT32(70000UL, u.link.uptimeSeconds);
  TEST_ASSERT_EQUAL_UINT16(500, u.link.rxFrames);
  TEST_ASSERT_EQUAL_UINT16(499, u.link.txReplies);
  TEST_ASSERT_EQUAL_UINT8(2, u.link.deafHeals);
}

static void test_fold_ext_diag_base_only_when_extension_absent() {
  // A unit without the extension: padding behind the base packet.
  uint8_t buf[EXT_DIAG_LINK_REPLY_LEN];
  fillExtReply(buf, true, true);
  memset(buf + EXT_DIAG_REPLY_LEN, 0xFF, EXT_DIAG_LINK_EXT_LEN);
  UnitFacts u;
  u.link.rxFrames = 77;  // stale value must not survive as "valid"
  u.linkValid = true;
  unitFactsFoldExtDiag(u, buf);
  TEST_ASSERT_TRUE(u.extDiagValid);
  TEST_ASSERT_FALSE(u.linkValid);
  TEST_ASSERT_EQUAL_UINT16(12, u.extDiag.stepExcessLast);
  TEST_ASSERT_EQUAL_UINT16(8, u.extDiag.dutyWindow);
}

static void test_fold_ext_diag_bad_base_good_extension_independent() {
  uint8_t buf[EXT_DIAG_LINK_REPLY_LEN];
  fillExtReply(buf, false, true);
  UnitFacts u;
  u.extDiagValid = true;
  unitFactsFoldExtDiag(u, buf);
  TEST_ASSERT_FALSE(u.extDiagValid);
  TEST_ASSERT_TRUE(u.linkValid);
}

static void test_health_json_link_keys_only_when_valid() {
  UnitFacts units[2];
  for (int i = 0; i < 2; i++) {
    units[i].state = 1;
    units[i].statusValid = true;
    units[i].extDiagValid = true;
  }
  units[0].linkValid = true;
  units[0].link.uptimeSeconds = 70000;
  units[0].link.rxFrames = 500;
  units[0].link.txReplies = 499;
  units[0].link.deafHeals = 2;
  char buf[768];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 2, 0, 1, 0);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"ut\":70000,\"rx\":500,\"tx\":499,\"dh\":2"));
  char* second = strstr(buf, "\"i\":1");
  TEST_ASSERT_NOT_NULL(second);
  TEST_ASSERT_NULL(strstr(second, "\"ut\""));
  TEST_ASSERT_NOT_NULL(strstr(second, "\"se\""));
}

// --- vitals block (#306) ----------------------------------------------------

static void test_health_json_vitals_emitted_when_valid() {
  UnitFacts units[1];
  units[0].state = 1;
  units[0].statusValid = true;
  strcpy(units[0].version, "abc12345");
  units[0].vitals.vccNow_mV = 4980;
  units[0].vitals.vccMin_mV = 4210;
  units[0].vitals.cmdPos = 17;
  units[0].vitals.freeRamMin = 384;
  units[0].vitalsValid = true;
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 0);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  // Per-unit fields.
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"vcc\":4980"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"vmin\":4210"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"cp\":17"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"ram\":384"));
  // Headline aggregate = the single unit's vmin.
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"vccMin\":4210"));
}

static void test_health_json_headline_vccmin_is_min_across_units() {
  UnitFacts units[3];
  for (int i = 0; i < 3; i++) {
    units[i].state = 1;
    units[i].statusValid = true;
    strcpy(units[i].version, "abc12345");
    units[i].vitalsValid = true;
    units[i].vitals.vccNow_mV = 5000;
  }
  units[0].vitals.vccMin_mV = 4600;
  units[1].vitals.vccMin_mV = 4100;  // the floor
  units[2].vitals.vccMin_mV = 4700;
  char buf[1024];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 3, 0, 1, 0);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"vccMin\":4100"));
}

static void test_health_json_no_vccmin_without_any_vitals() {
  // Pre-vitals firmware: no vitalsValid anywhere -> headline omits vccMin
  // (never a phantom 0) and no per-unit vcc field appears.
  UnitFacts units[2];
  units[0].state = 1;
  units[0].statusValid = true;
  strcpy(units[0].version, "abc12345");
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 2, 0, 1, 0);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NULL(strstr(buf, "\"vccMin\""));
  TEST_ASSERT_NULL(strstr(buf, "\"vcc\""));
}

// --- per-unit I2C error attribution (#367) ----------------------------------

static void test_unit_err_bump_saturates() {
  TEST_ASSERT_EQUAL_UINT16(1, unitErrBump(0));
  TEST_ASSERT_EQUAL_UINT16(100, unitErrBump(99));
  // Pins at 0xFFFF instead of wrapping to 0 (a wrap would read as healthy).
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, unitErrBump(0xFFFE));
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, unitErrBump(0xFFFF));
}

static void test_health_json_err_emitted_when_nonzero() {
  UnitFacts units[1];
  units[0].state = 1;
  units[0].statusValid = true;
  strcpy(units[0].version, "abc12345");
  units[0].i2cErrors = 7;
  units[0].lastErrorMs = 1000;
  char buf[512];
  // nowMs = 4500 -> errAge = 3500.
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 4500);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"err\":7"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"errAge\":3500"));
}

static void test_health_json_err_omitted_when_clean() {
  // A unit that has never errored stays lean — no err/errAge keys.
  UnitFacts units[1];
  units[0].state = 1;
  units[0].statusValid = true;
  strcpy(units[0].version, "abc12345");
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 5000);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NULL(strstr(buf, "\"err\""));
  TEST_ASSERT_NULL(strstr(buf, "\"errAge\""));
}

static void test_fleet_vcc_min_helper() {
  UnitFacts units[3];
  for (int i = 0; i < 3; i++) { units[i].state = 1; units[i].statusValid = true; }
  // No vitals anywhere -> 0 (sentinel).
  TEST_ASSERT_EQUAL_UINT16(0, unitFleetVccMin(units, 3));
  units[0].vitalsValid = true; units[0].vitals.vccMin_mV = 4600;
  units[1].vitalsValid = true; units[1].vitals.vccMin_mV = 4100;
  units[2].vitalsValid = true; units[2].vitals.vccMin_mV = 0;  // sentinel skipped
  TEST_ASSERT_EQUAL_UINT16(4100, unitFleetVccMin(units, 3));
}

static void test_health_json_silent_gap_slot() {
  UnitFacts units[1];  // default: state 0, statusValid false
  char buf[128];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1, 0);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_EQUAL_STRING(
      "{\"width\":1,\"faulty\":0,\"units\":[{\"i\":0,\"a\":1,\"st\":0,\"v\":0}]}",
      buf);
}

static void test_health_json_overflow_reports_full() {
  UnitFacts units[2];
  units[0].state = 1;
  units[0].statusValid = true;
  char buf[24];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 2, 0, 1, 0);
  // Caller contract (v1 parity): n >= cap means "would truncate", the
  // endpoint falls back to a headline-only JSON.
  TEST_ASSERT_TRUE(n >= sizeof(buf));
}

// --- letterReadbackValid -----------------------------------------------------

static void test_letter_readback_valid_pair() {
  TEST_ASSERT_TRUE(letterReadbackValid(7, (uint8_t)~7, 45));
}

static void test_letter_readback_rejects_bad_complement() {
  TEST_ASSERT_FALSE(letterReadbackValid(7, 7, 45));
}

static void test_letter_readback_rejects_out_of_range() {
  TEST_ASSERT_FALSE(letterReadbackValid(45, (uint8_t)~45, 45));
}

// --- isAtmega328pSignature ---------------------------------------------------

static void test_atmega328p_signature() {
  TEST_ASSERT_TRUE(isAtmega328pSignature(0x1E, 0x95, 0x0F));
  TEST_ASSERT_FALSE(isAtmega328pSignature(0x1E, 0x95, 0x14));  // 328PB
  TEST_ASSERT_FALSE(isAtmega328pSignature(0xFF, 0xFF, 0xFF));  // bus noise
}

// --- fw status vs the bundled rev (#205) ---------------------------------------

static void test_fw_status_matches_bundled_rev() {
  TEST_ASSERT_EQUAL_UINT8(0, unitFwStatusFromRev("26518a1", "26518a1"));
}

static void test_fw_status_outdated_on_mismatch() {
  TEST_ASSERT_EQUAL_UINT8(1, unitFwStatusFromRev("0fd341f", "26518a1"));
}

static void test_fw_status_unknown_on_empty_version_or_bundle() {
  TEST_ASSERT_EQUAL_UINT8(2, unitFwStatusFromRev("", "26518a1"));
  TEST_ASSERT_EQUAL_UINT8(2, unitFwStatusFromRev("26518a1", ""));
}

static void test_fw_status_compares_first_eight_chars_like_v1() {
  // v1 stored 8 chars + NUL and compared with strncmp(..., 8); a longer
  // sidecar rev (e.g. "26518a1e-dirty") must still match its 8-char prefix.
  TEST_ASSERT_EQUAL_UINT8(0, unitFwStatusFromRev("26518a1e", "26518a1e-dirty"));
  TEST_ASSERT_EQUAL_UINT8(1, unitFwStatusFromRev("26518a1e", "26518a1f-dirty"));
}

// --- proven content-equivalent legacy revs (#440) ------------------------------

// A unit built before the identity scheme changed reports an older rev while
// running byte-identical machine code. Those revs are proven equivalent at
// stage time and carried in the bundle, so they must grade as current.
static void test_fw_status_accepts_a_proven_equivalent_rev() {
  TEST_ASSERT_EQUAL_UINT8(0, unitFwStatusFromRev("d6e8a8a", "f01d855",
                                                 "d6e8a8a"));
}

static void test_fw_status_accepts_any_of_several_equivalents() {
  const char* equiv = "aaaaaaa,d6e8a8a,bbbbbbb";
  TEST_ASSERT_EQUAL_UINT8(0, unitFwStatusFromRev("aaaaaaa", "f01d855", equiv));
  TEST_ASSERT_EQUAL_UINT8(0, unitFwStatusFromRev("d6e8a8a", "f01d855", equiv));
  TEST_ASSERT_EQUAL_UINT8(0, unitFwStatusFromRev("bbbbbbb", "f01d855", equiv));
}

// The whole point: a rev NOT proven equivalent still reads OUTDATED. The list
// must widen what counts as current, never blanket-suppress the flag.
static void test_fw_status_still_outdated_when_not_in_the_equivalent_list() {
  TEST_ASSERT_EQUAL_UINT8(1, unitFwStatusFromRev("0fd341f", "f01d855",
                                                 "d6e8a8a,aaaaaaa"));
}

// A CSV entry must terminate at the comma — otherwise "d6e8a8a" would be
// compared against "d6e8a8a,aaaaaaa" and fail on the separator.
static void test_fw_status_equivalent_entry_terminates_at_the_comma() {
  TEST_ASSERT_EQUAL_UINT8(0, unitFwStatusFromRev("d6e8a8a", "f01d855",
                                                 "d6e8a8a,aaaaaaa"));
  // ...and a longer version must not match a shorter entry by prefix.
  TEST_ASSERT_EQUAL_UINT8(1, unitFwStatusFromRev("d6e8a8ab", "f01d855",
                                                 "d6e8a8a,aaaaaaa"));
}

static void test_fw_status_tolerates_empty_null_and_padded_equivalents() {
  TEST_ASSERT_EQUAL_UINT8(1, unitFwStatusFromRev("d6e8a8a", "f01d855", nullptr));
  TEST_ASSERT_EQUAL_UINT8(1, unitFwStatusFromRev("d6e8a8a", "f01d855", ""));
  TEST_ASSERT_EQUAL_UINT8(1, unitFwStatusFromRev("d6e8a8a", "f01d855", ",,,"));
  TEST_ASSERT_EQUAL_UINT8(0, unitFwStatusFromRev("d6e8a8a", "f01d855",
                                                 " d6e8a8a , aaaaaaa "));
}

// An unreadable version stays UNKNOWN — the equivalence list must never
// promote "we could not ask" into "current".
static void test_fw_status_equivalents_never_rescue_an_unknown_version() {
  TEST_ASSERT_EQUAL_UINT8(2, unitFwStatusFromRev("", "f01d855", "d6e8a8a"));
  TEST_ASSERT_EQUAL_UINT8(2, unitFwStatusFromRev(nullptr, "f01d855", "d6e8a8a"));
  TEST_ASSERT_EQUAL_UINT8(2, unitFwStatusFromRev("d6e8a8a", "", "d6e8a8a"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_clean_status_is_not_faulty);
  RUN_TEST(test_home_failed_flag_is_faulty);
  RUN_TEST(test_hall_never_flag_is_faulty);
  RUN_TEST(test_lifetime_reset_counters_alone_are_not_faulty);
  RUN_TEST(test_first_read_sets_the_baseline_and_is_not_a_reset);
  RUN_TEST(test_a_counter_that_climbs_is_a_reset_seen);
  RUN_TEST(test_a_reset_seen_stays_seen_on_later_reads);
  RUN_TEST(test_a_cleared_counter_pulls_the_baseline_down);
  RUN_TEST(test_a_saturated_counter_cannot_report_a_reset);
  RUN_TEST(test_reset_seen_makes_a_valid_unit_faulty);
  RUN_TEST(test_bad_commands_alone_are_not_faulty);
  RUN_TEST(test_addr_eeprom_flag_alone_is_not_faulty);
  RUN_TEST(test_faulty_count_only_counts_valid_slots);
  RUN_TEST(test_faulty_count_includes_lost_sketch_units);
  RUN_TEST(test_lost_and_faulty_unit_counts_once);
  RUN_TEST(test_health_json_valid_and_bootloader_slots);
  RUN_TEST(test_health_json_reports_rescue_exits_when_nonzero);
  RUN_TEST(test_health_json_addr_eeprom_bit_surfaces_as_ae);
  RUN_TEST(test_boot_home_state_decode);
  RUN_TEST(test_health_json_heartbeat_freshness);
  RUN_TEST(test_freshness_survives_a_real_miss_streak);
  RUN_TEST(test_freshness_gap_slot_never_stale);
  RUN_TEST(test_health_json_odometer_emitted_when_valid);
  RUN_TEST(test_health_json_offset_emitted_when_valid);
  RUN_TEST(test_odometer_readback_valid_roundtrip);
  RUN_TEST(test_odometer_readback_rejects_old_firmware_garbage);
  RUN_TEST(test_diag_readback_valid_roundtrip);
  RUN_TEST(test_diag_readback_accepts_unknown_letter_sentinel);
  RUN_TEST(test_diag_readback_rejects_old_firmware_garbage);
  RUN_TEST(test_diag_readback_rejects_out_of_range_letter);
  RUN_TEST(test_selftest_readback_valid_roundtrip);
  RUN_TEST(test_selftest_readback_rejects_garbage_and_bad_state);
  RUN_TEST(test_health_json_diag_fields_emitted_when_valid);
  RUN_TEST(test_health_json_phys_omitted_when_position_unknown);
  RUN_TEST(test_health_json_emits_stamped_mismatch_only);
  RUN_TEST(test_health_json_no_mismatch_without_position);
  RUN_TEST(test_a_corrupt_bootloader_makes_a_unit_faulty);
  RUN_TEST(test_health_json_boot_verdict_keys);
  RUN_TEST(test_health_json_bootloader_identity_keys);
  RUN_TEST(test_health_json_ext_diag_emitted_when_valid);
  RUN_TEST(test_fold_ext_diag_both_valid);
  RUN_TEST(test_fold_ext_diag_base_only_when_extension_absent);
  RUN_TEST(test_fold_ext_diag_bad_base_good_extension_independent);
  RUN_TEST(test_health_json_link_keys_only_when_valid);
  RUN_TEST(test_health_json_lifetime_emitted_when_valid);
  RUN_TEST(test_health_json_futile_rehomes_emitted_when_nonzero);
  RUN_TEST(test_health_json_says_plainly_when_the_check_is_disarmed);
  RUN_TEST(test_health_json_fresh_unit_emits_no_lifetime_keys);
  RUN_TEST(test_health_json_self_test_pair_emits_when_only_one_is_set);
  RUN_TEST(test_health_json_vitals_emitted_when_valid);
  RUN_TEST(test_health_json_headline_vccmin_is_min_across_units);
  RUN_TEST(test_health_json_no_vccmin_without_any_vitals);
  RUN_TEST(test_unit_err_bump_saturates);
  RUN_TEST(test_health_json_err_emitted_when_nonzero);
  RUN_TEST(test_health_json_err_omitted_when_clean);
  RUN_TEST(test_fleet_vcc_min_helper);
  RUN_TEST(test_health_json_silent_gap_slot);
  RUN_TEST(test_health_json_overflow_reports_full);
  RUN_TEST(test_letter_readback_valid_pair);
  RUN_TEST(test_letter_readback_rejects_bad_complement);
  RUN_TEST(test_letter_readback_rejects_out_of_range);
  RUN_TEST(test_atmega328p_signature);
  RUN_TEST(test_fw_status_matches_bundled_rev);
  RUN_TEST(test_fw_status_outdated_on_mismatch);
  RUN_TEST(test_fw_status_unknown_on_empty_version_or_bundle);
  RUN_TEST(test_fw_status_compares_first_eight_chars_like_v1);
  RUN_TEST(test_fw_status_accepts_a_proven_equivalent_rev);
  RUN_TEST(test_fw_status_accepts_any_of_several_equivalents);
  RUN_TEST(test_fw_status_still_outdated_when_not_in_the_equivalent_list);
  RUN_TEST(test_fw_status_equivalent_entry_terminates_at_the_comma);
  RUN_TEST(test_fw_status_tolerates_empty_null_and_padded_equivalents);
  RUN_TEST(test_fw_status_equivalents_never_rescue_an_unknown_version);
  return UNITY_END();
}
