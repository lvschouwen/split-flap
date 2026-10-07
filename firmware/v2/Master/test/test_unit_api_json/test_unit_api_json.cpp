// Native tests for UnitApiJson.h: a unit's facts as the operator API gives them.
#include <unity.h>

#include <string>

#include "../../UnitApiJson.h"

void setUp() {}
void tearDown() {}

static UnitFacts healthy() {
  UnitFacts u;
  u.state = 1;
  u.fwStatus = 0;
  u.statusValid = true;
  strcpy(u.version, "d360e2b");
  u.status.flags = UNIT_FLAG_HOMED | UNIT_FLAG_ADDR_EEPROM;
  u.status.uptimeSeconds = 600;
  u.status.lastHomingStepCount = 1984;
  u.bootVerdict = BOOT_INTEGRITY_OK;
  u.vitalsValid = true;
  u.vitals.vccNow_mV = 5001;
  u.vitals.vccMin_mV = 4871;
  u.vitals.cmdPos = 1;
  u.diagValid = true;
  u.driftFlags = UNIT_DRIFT_FLAG_POSITION_KNOWN;
  u.physLetter = 1;
  u.odometerValid = true;
  u.odometer = 107;
  u.offsetValid = true;
  u.offset = -12;
  u.lastSeenMs = 9000;
  return u;
}

static std::string text(JsonDocument& doc) {
  std::string out;
  serializeJson(doc, out);
  return out;
}

static void test_a_table_row_has_a_value_for_every_field_in_order() {
  JsonDocument doc;
  const UnitFacts u = healthy();
  const UnitVerdict v = unitVerdict(u, UnitVerdictContext());
  unitApiTableRow(doc.to<JsonArray>(), u, 3, &v);
  TEST_ASSERT_EQUAL(sizeof(UNIT_TABLE_FIELDS) / sizeof(UNIT_TABLE_FIELDS[0]), doc.size());
  TEST_ASSERT_EQUAL_STRING(
      "[3,\"working\",\"working\",600,0,\"running\",\"d360e2b\",\"current\",\"ok\",5001,4871,"
      "1,107,-12]",
      text(doc).c_str());
}

static void test_what_was_not_read_is_null_in_the_table_and_the_row_stays_as_long() {
  JsonDocument doc;
  UnitFacts silent;  // nothing answers
  unitApiTableRow(doc.to<JsonArray>(), silent, 9, nullptr);
  TEST_ASSERT_EQUAL(sizeof(UNIT_TABLE_FIELDS) / sizeof(UNIT_TABLE_FIELDS[0]), doc.size());
  TEST_ASSERT_EQUAL_STRING(
      "[9,null,null,null,null,\"silent\",null,null,null,null,null,null,null,null]",
      text(doc).c_str());
}

static void test_a_position_the_drum_does_not_have_is_no_letter() {
  TEST_ASSERT_TRUE(unitApiFlap(0));
  TEST_ASSERT_TRUE(unitApiFlap(SFP_FLAP_AMOUNT - 1));
  TEST_ASSERT_FALSE(unitApiFlap(SFP_FLAP_AMOUNT));
  TEST_ASSERT_FALSE(unitApiFlap(0xFF));
  UnitFacts u = healthy();
  u.physLetter = 0xFF;  // never synced to a hall edge
  JsonDocument doc;
  unitApiTableRow(doc.to<JsonArray>(), u, 1, nullptr);
  TEST_ASSERT_TRUE(doc[11].isNull());
}

static void test_the_detail_groups_the_facts_and_names_the_verdict() {
  JsonDocument doc;
  UnitFacts u = healthy();
  u.lifetimeValid = true;
  u.lifetime.homeFailedCount = 4;
  u.i2cErrors = 2;
  u.lastErrorMs = 8000;
  const UnitVerdict v = unitVerdict(u, UnitVerdictContext());
  unitApiDetail(doc.to<JsonObject>(), u, 3, 10000, &v);
  TEST_ASSERT_EQUAL(3, doc["address"].as<int>());
  TEST_ASSERT_EQUAL_STRING("running", doc["state"]);
  TEST_ASSERT_EQUAL_STRING("note", doc["verdict"]["level"]);
  TEST_ASSERT_EQUAL_STRING("home-failed-before", doc["verdict"]["reason"]);
  TEST_ASSERT_EQUAL(4, doc["verdict"]["a"].as<int>());
  TEST_ASSERT_EQUAL(0, doc["verdict"]["also"].size());  // the leading reason is not repeated
  TEST_ASSERT_EQUAL_STRING("d360e2b", doc["firmware"]["rev"]);
  TEST_ASSERT_EQUAL(600, doc["firmware"]["uptimeS"].as<int>());
  TEST_ASSERT_EQUAL(4871, doc["power"]["supplyMinMv"].as<int>());
  TEST_ASSERT_EQUAL(1000, doc["link"]["heardMsAgo"].as<int>());
  TEST_ASSERT_EQUAL(2, doc["link"]["failed"].as<int>());
  TEST_ASSERT_EQUAL(2000, doc["link"]["failedMsAgo"].as<int>());
  TEST_ASSERT_EQUAL_STRING("homed", doc["drum"]["home"]);
  TEST_ASSERT_EQUAL(1, doc["drum"]["shows"].as<int>());
  TEST_ASSERT_EQUAL(1, doc["drum"]["commanded"].as<int>());
  TEST_ASSERT_FALSE(doc["drum"]["wrongLetter"].as<bool>());
  TEST_ASSERT_EQUAL(-12, doc["drum"]["offset"].as<int>());
  TEST_ASSERT_EQUAL(4, doc["drum"]["homeFailures"].as<int>());
  TEST_ASSERT_TRUE(doc["addressStored"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("ok", doc["bootloader"]["verdict"]);
  TEST_ASSERT_TRUE(doc["bootloader"]["crc32"].isNull());  // only when it is not the expected one
}

static void test_a_unit_in_its_bootloader_says_what_the_bootloader_said() {
  JsonDocument doc;
  UnitFacts u;
  u.state = 2;
  u.bootloader.generation = 3;
  u.bootloader.caps = 5;
  u.bootloader.fusesValid = true;
  u.bootloader.lock = 0xCF;
  u.bootloader.lfuse = 0xFF;
  u.bootloader.hfuse = 0xDA;
  u.bootloader.efuse = 0xFD;
  u.bootloader.crashValid = true;
  u.bootloader.crashCount = 3;
  unitApiDetail(doc.to<JsonObject>(), u, 6, 0, nullptr);
  TEST_ASSERT_EQUAL_STRING("bootloader", doc["state"]);
  TEST_ASSERT_TRUE(doc["verdict"].isNull());
  TEST_ASSERT_EQUAL(3, doc["bootloader"]["generation"].as<int>());
  TEST_ASSERT_EQUAL_STRING("cf", doc["bootloader"]["lock"]);
  TEST_ASSERT_EQUAL_STRING("ffdafd", doc["bootloader"]["fuses"]);
  TEST_ASSERT_EQUAL(3, doc["bootloader"]["crashes"].as<int>());
  // Nothing it was not read for is made up.
  TEST_ASSERT_EQUAL(0, doc["firmware"].size());
  TEST_ASSERT_EQUAL(0, doc["power"].size());
  TEST_ASSERT_EQUAL(0, doc["link"].size());
  TEST_ASSERT_EQUAL(0, doc["drum"].size());
}

static void test_a_damaged_bootloader_carries_its_checksum() {
  JsonDocument doc;
  UnitFacts u = healthy();
  u.bootVerdict = BOOT_INTEGRITY_CORRUPT;
  u.bootCrc32 = 0x0BADF00D;
  unitApiDetail(doc.to<JsonObject>(), u, 1, 0, nullptr);
  TEST_ASSERT_EQUAL_STRING("corrupt", doc["bootloader"]["verdict"]);
  TEST_ASSERT_EQUAL_STRING("0badf00d", doc["bootloader"]["crc32"]);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_table_row_has_a_value_for_every_field_in_order);
  RUN_TEST(test_what_was_not_read_is_null_in_the_table_and_the_row_stays_as_long);
  RUN_TEST(test_a_position_the_drum_does_not_have_is_no_letter);
  RUN_TEST(test_the_detail_groups_the_facts_and_names_the_verdict);
  RUN_TEST(test_a_unit_in_its_bootloader_says_what_the_bootloader_said);
  RUN_TEST(test_a_damaged_bootloader_carries_its_checksum);
  return UNITY_END();
}
