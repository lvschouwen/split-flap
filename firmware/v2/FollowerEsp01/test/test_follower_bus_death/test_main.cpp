// Host-side tests for FollowerBusDeath.h: which units count as there, and how
// the record of a bus death is packed for the master.
#include <unity.h>

#include <string.h>

#include "../../FollowerBusDeath.h"
#include "../../FollowerBusRecovery.h"

void setUp() {}
void tearDown() {}

static void test_an_address_that_only_acknowledges_is_no_unit() {
  UnitFacts facts[16] = {};
  TEST_ASSERT_EQUAL(0, followerUnitsAnswering(facts, 16));
  // The phantom: found "running" at the scan, nothing readable behind it.
  facts[8].state = 1;
  TEST_ASSERT_EQUAL(0, followerUnitsAnswering(facts, 16));
  // A row with only that is an empty row: it is probed again.
  BusRecoveryState s;
  TEST_ASSERT_TRUE(busRecoveryReprobeDue(s, followerUnitsAnswering(facts, 16)));
}

static void test_a_reply_that_checked_out_makes_a_unit() {
  UnitFacts facts[16] = {};
  facts[0].state = 2;  // in its bootloader, which identified itself
  facts[1].state = 1;
  facts[1].protocolKnown = true;  // its version was read
  facts[2].state = 1;
  facts[2].statusValid = true;  // firmware whose version does not read, but a status does
  facts[3].state = 1;           // acknowledged only
  facts[4].protocolKnown = true;  // a slot the scan emptied keeps nothing
  facts[4].state = 0;
  TEST_ASSERT_EQUAL(3, followerUnitsAnswering(facts, 16));
  BusRecoveryState s;
  TEST_ASSERT_FALSE(busRecoveryReprobeDue(s, followerUnitsAnswering(facts, 16)));
}

static void test_units_that_were_known_and_went_silent_are_still_the_rows_units() {
  // The bus dies under a working row: the facts keep what was read, so the
  // row is not empty and its units are not forgotten by a re-probe.
  UnitFacts facts[16] = {};
  for (int i = 0; i < 5; i++) {
    facts[i].state = 1;
    facts[i].protocolKnown = true;
    facts[i].statusValid = false;  // the reads fail now
    facts[i].stale = true;
  }
  TEST_ASSERT_EQUAL(5, followerUnitsAnswering(facts, 16));
}

static void test_the_death_is_packed_field_by_field() {
  FollowerBusDeath d;
  d.lineState = 3;
  d.acked = 1;
  d.notAcked = 15;
  d.refused = 0;
  d.hadAnswered = 5;
  d.sinceMoveS = 24;
  const uint32_t a = followerBusDeathA(d);
  TEST_ASSERT_EQUAL_UINT32(3, a & 0x0F);
  TEST_ASSERT_EQUAL_UINT32(1, (a >> 4) & 0x1F);
  TEST_ASSERT_EQUAL_UINT32(15, (a >> 9) & 0x1F);
  TEST_ASSERT_EQUAL_UINT32(0, (a >> 14) & 0x1F);
  TEST_ASSERT_EQUAL_UINT32(5, (a >> 19) & 0x1F);
  TEST_ASSERT_EQUAL_UINT32(24, followerBusDeathB(d));
  // A count past its field stays in it.
  d.refused = 200;
  TEST_ASSERT_EQUAL_UINT32(31, (followerBusDeathA(d) >> 14) & 0x1F);
  TEST_ASSERT_EQUAL_UINT32(15, (followerBusDeathA(d) >> 9) & 0x1F);
}

static void test_a_row_that_never_moved_says_so() {
  FollowerBusDeath d;
  TEST_ASSERT_EQUAL_UINT32(FOLLOWER_BUS_DEATH_NO_MOVE, followerBusDeathB(d));
  d.sinceMoveS = 500000;  // days ago: past the field
  TEST_ASSERT_EQUAL_UINT32(FOLLOWER_BUS_DEATH_NO_MOVE, followerBusDeathB(d));
  d.sinceMoveS = 0;
  TEST_ASSERT_EQUAL_UINT32(0, followerBusDeathB(d));
}

static void test_a_rise_is_given_in_tenths_of_a_microsecond() {
  TEST_ASSERT_EQUAL_UINT16(10, followerLineTenths(80, 80));     // 1 us at 80 MHz
  TEST_ASSERT_EQUAL_UINT16(12, followerLineTenths(200, 160));
  TEST_ASSERT_EQUAL_UINT16(1, followerLineTenths(2, 80));       // never 0: that is "not measured"
  // The longest wait still reads as a rise; "no rise" is the caller's verdict.
  TEST_ASSERT_EQUAL_UINT16(FOLLOWER_LINE_NO_RISE - 1, followerLineTenths(80UL * 1000000UL, 80));
  TEST_ASSERT_EQUAL_UINT16(FOLLOWER_LINE_NOT_MEASURED, followerLineTenths(80, 0));
  TEST_ASSERT_EQUAL_UINT32(0x000C000AUL, followerLinesPack(10, 12));
}

static void test_a_rise_reads_as_text() {
  char text[16];
  followerLineText(text, sizeof(text), 12);
  TEST_ASSERT_EQUAL_STRING("1.2 us", text);
  followerLineText(text, sizeof(text), 9999);
  TEST_ASSERT_EQUAL_STRING("999.9 us", text);
  followerLineText(text, sizeof(text), FOLLOWER_LINE_NO_RISE);
  TEST_ASSERT_EQUAL_STRING("no rise", text);
  followerLineText(text, sizeof(text), FOLLOWER_LINE_NOT_MEASURED);
  TEST_ASSERT_EQUAL_STRING("not measured", text);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_an_address_that_only_acknowledges_is_no_unit);
  RUN_TEST(test_a_reply_that_checked_out_makes_a_unit);
  RUN_TEST(test_units_that_were_known_and_went_silent_are_still_the_rows_units);
  RUN_TEST(test_the_death_is_packed_field_by_field);
  RUN_TEST(test_a_row_that_never_moved_says_so);
  RUN_TEST(test_a_rise_is_given_in_tenths_of_a_microsecond);
  RUN_TEST(test_a_rise_reads_as_text);
  return UNITY_END();
}
