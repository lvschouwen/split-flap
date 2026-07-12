// Host-side tests for SystemStatsPolicy.h (#245) — the System tab's sample
// ring, CPU-load math and /system/stats JSON. Target glue (the netTask
// sampler in SystemStats.cpp) is bench tier.

#include <ArduinoFake.h>
#include <unity.h>

#include <cstring>

#include "../../SystemStatsPolicy.h"

void setUp() {}
void tearDown() {}

static SystemSample mkSample(int16_t rssi, uint32_t heap) {
  SystemSample s;
  s.rssi = rssi;
  s.freeHeap = heap;
  return s;
}

// --- ring -------------------------------------------------------------------

static void test_ring_push_and_oldest_first_order() {
  SystemStatsRing r;
  systemStatsPush(r, mkSample(-40, 1000));
  systemStatsPush(r, mkSample(-50, 2000));
  TEST_ASSERT_EQUAL_UINT16(2, r.count);
  TEST_ASSERT_EQUAL_INT16(-40, systemStatsAt(r, 0)->rssi);
  TEST_ASSERT_EQUAL_INT16(-50, systemStatsAt(r, 1)->rssi);
}

static void test_ring_wraps_dropping_oldest() {
  SystemStatsRing r;
  for (int i = 0; i < SYSTEM_STATS_RING + 3; i++) {
    systemStatsPush(r, mkSample((int16_t)-i, 0));
  }
  TEST_ASSERT_EQUAL_UINT16(SYSTEM_STATS_RING, r.count);
  // Oldest surviving sample is i=3; newest is i=RING+2.
  TEST_ASSERT_EQUAL_INT16(-3, systemStatsAt(r, 0)->rssi);
  TEST_ASSERT_EQUAL_INT16((int16_t)-(SYSTEM_STATS_RING + 2),
                          systemStatsAt(r, SYSTEM_STATS_RING - 1)->rssi);
}

// --- CPU load ----------------------------------------------------------------

static void test_cpu_load_from_idle_delta() {
  // Idle ran 25% of the window -> 75% load.
  TEST_ASSERT_EQUAL_UINT8(75, cpuLoadPercent(250, 1000));
}

static void test_cpu_load_clamps_and_handles_zero_window() {
  TEST_ASSERT_EQUAL_UINT8(0, cpuLoadPercent(0, 0));      // no window yet
  TEST_ASSERT_EQUAL_UINT8(0, cpuLoadPercent(2000, 1000)); // idle > total: clamp
  TEST_ASSERT_EQUAL_UINT8(100, cpuLoadPercent(0, 1000));
}

// --- JSON ---------------------------------------------------------------------

static void test_json_now_and_history_shape() {
  SystemStatsRing r;
  SystemSample a;
  a.rssi = -55; a.freeHeap = 200000; a.maxAlloc = 100000;
  a.psramFree = 8000000; a.cpu0 = 12; a.cpu1 = 99; a.tempC10 = 435;
  systemStatsPush(r, a);
  SystemSample b = a;
  b.rssi = -60; b.cpu0 = 15;
  systemStatsPush(r, b);

  SystemNow now;
  now.uptimeS = 3600; now.minFreeHeap = 150000;
  now.i2cTx = 12345; now.i2cErr = 2; now.mqttDrops = 1;
  now.ntpAgeS = 42;
  strcpy(now.resetReason, "POWERON");

  char buf[2048];
  size_t n = buildSystemStatsJson(buf, sizeof(buf), r, now);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"rssi\":-60"));         // now = newest
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"heap\":200000"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"maxAlloc\":100000"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"psram\":8000000"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"temp\":435"));         // x10, JS divides
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"uptime\":3600"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"minHeap\":150000"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"i2cTx\":12345"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"i2cErr\":2"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"mqttDrops\":1"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"ntpAge\":42"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"reset\":\"POWERON\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"rssi\":[-55,-60]"));   // hist oldest-first
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"cpu0\":[12,15]"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"cpu1\":[99,99]"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"interval\":5"));
  TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);
}

static void test_json_never_synced_ntp_is_minus_one() {
  SystemStatsRing r;
  systemStatsPush(r, mkSample(-50, 1000));
  SystemNow now;
  now.ntpAgeS = -1;
  strcpy(now.resetReason, "SW");
  char buf[1024];
  size_t n = buildSystemStatsJson(buf, sizeof(buf), r, now);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"ntpAge\":-1"));
}

static void test_json_full_ring_fits_cap() {
  // Worst case: every field at its widest, full 120-sample ring.
  SystemStatsRing r;
  SystemSample s;
  s.rssi = -127; s.freeHeap = 4294967295UL; s.maxAlloc = 4294967295UL;
  s.psramFree = 4294967295UL; s.cpu0 = 100; s.cpu1 = 100; s.tempC10 = -999;
  for (int i = 0; i < SYSTEM_STATS_RING; i++) systemStatsPush(r, s);
  SystemNow now;
  now.uptimeS = 4294967295UL; now.minFreeHeap = 4294967295UL;
  now.i2cTx = 4294967295UL; now.i2cErr = 4294967295UL;
  now.mqttDrops = 4294967295UL; now.ntpAgeS = -1;
  strcpy(now.resetReason, "TASK_WDT_CPU1");
  char buf[SYSTEM_STATS_JSON_CAP];
  size_t n = buildSystemStatsJson(buf, sizeof(buf), r, now);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_TRUE(n < SYSTEM_STATS_JSON_CAP);
}

static void test_json_overflow_reports_full() {
  SystemStatsRing r;
  for (int i = 0; i < 50; i++) systemStatsPush(r, mkSample(-50, 123456));
  SystemNow now;
  now.ntpAgeS = 0;
  strcpy(now.resetReason, "SW");
  char buf[64];
  size_t n = buildSystemStatsJson(buf, sizeof(buf), r, now);
  TEST_ASSERT_TRUE(n >= sizeof(buf));  // caller falls back / rejects
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ring_push_and_oldest_first_order);
  RUN_TEST(test_ring_wraps_dropping_oldest);
  RUN_TEST(test_cpu_load_from_idle_delta);
  RUN_TEST(test_cpu_load_clamps_and_handles_zero_window);
  RUN_TEST(test_json_now_and_history_shape);
  RUN_TEST(test_json_never_synced_ntp_is_minus_one);
  RUN_TEST(test_json_full_ring_fits_cap);
  RUN_TEST(test_json_overflow_reports_full);
  UNITY_END();
  return 0;
}
