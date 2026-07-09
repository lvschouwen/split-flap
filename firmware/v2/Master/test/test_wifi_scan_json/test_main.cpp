// Host-side unit tests for WifiScanJson.h (#188) — the pure JSON builder
// behind GET /wifi/scan (the portal's scan-and-pick list). Raw scan results
// go in; a deduped, strongest-first, escaped, capped networks array comes
// out.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../WifiScanJson.h"

void setUp() {}
void tearDown() {}

static void test_empty_scan_yields_empty_array() {
  TEST_ASSERT_EQUAL_STRING("{\"networks\":[]}",
                           buildWifiScanJson(nullptr, 0).c_str());
}

static void test_single_network_renders_all_fields() {
  WifiScanEntry e[] = {{String("HomeNet"), -42, true}};
  TEST_ASSERT_EQUAL_STRING(
      "{\"networks\":[{\"ssid\":\"HomeNet\",\"rssi\":-42,\"secure\":true}]}",
      buildWifiScanJson(e, 1).c_str());
}

static void test_open_network_renders_secure_false() {
  WifiScanEntry e[] = {{String("cafe"), -70, false}};
  TEST_ASSERT_EQUAL_STRING(
      "{\"networks\":[{\"ssid\":\"cafe\",\"rssi\":-70,\"secure\":false}]}",
      buildWifiScanJson(e, 1).c_str());
}

static void test_networks_sorted_strongest_first() {
  WifiScanEntry e[] = {{String("weak"), -88, true},
                       {String("strong"), -40, true},
                       {String("mid"), -60, true}};
  TEST_ASSERT_EQUAL_STRING(
      "{\"networks\":["
      "{\"ssid\":\"strong\",\"rssi\":-40,\"secure\":true},"
      "{\"ssid\":\"mid\",\"rssi\":-60,\"secure\":true},"
      "{\"ssid\":\"weak\",\"rssi\":-88,\"secure\":true}]}",
      buildWifiScanJson(e, 3).c_str());
}

static void test_duplicate_ssids_keep_strongest_only() {
  // Multi-AP networks (mesh, repeaters) appear once per BSSID in a raw
  // scan; the picker wants one row per network at its best signal.
  WifiScanEntry e[] = {{String("mesh"), -80, true},
                       {String("mesh"), -45, true},
                       {String("other"), -60, true},
                       {String("mesh"), -70, true}};
  TEST_ASSERT_EQUAL_STRING(
      "{\"networks\":["
      "{\"ssid\":\"mesh\",\"rssi\":-45,\"secure\":true},"
      "{\"ssid\":\"other\",\"rssi\":-60,\"secure\":true}]}",
      buildWifiScanJson(e, 4).c_str());
}

static void test_hidden_ssids_are_dropped() {
  // A hidden AP scans as an empty ssid — useless in a pick list (manual
  // entry covers it).
  WifiScanEntry e[] = {{String(""), -30, true}, {String("visible"), -50, true}};
  TEST_ASSERT_EQUAL_STRING(
      "{\"networks\":[{\"ssid\":\"visible\",\"rssi\":-50,\"secure\":true}]}",
      buildWifiScanJson(e, 2).c_str());
}

static void test_ssid_json_metacharacters_are_escaped() {
  WifiScanEntry e[] = {{String("say \"hi\"\\now"), -50, false}};
  TEST_ASSERT_EQUAL_STRING(
      "{\"networks\":[{\"ssid\":\"say \\\"hi\\\"\\\\now\",\"rssi\":-50,"
      "\"secure\":false}]}",
      buildWifiScanJson(e, 1).c_str());
}

static void test_list_caps_at_maximum() {
  // Dense environments scan 30+ APs; the portal list (and the JSON String
  // in RAM) stays bounded. Entries are distinct SSIDs so the cap — not the
  // dedup — is what trims.
  WifiScanEntry e[WIFI_SCAN_JSON_MAX + 5];
  for (int i = 0; i < WIFI_SCAN_JSON_MAX + 5; i++) {
    e[i] = {String("net") + String(i), -30 - i, true};
  }
  String out = buildWifiScanJson(e, WIFI_SCAN_JSON_MAX + 5);
  int rows = 0;
  for (int at = out.indexOf("\"ssid\""); at >= 0;
       at = out.indexOf("\"ssid\"", at + 1)) {
    rows++;
  }
  TEST_ASSERT_EQUAL_INT(WIFI_SCAN_JSON_MAX, rows);
  // Strongest-first ordering means the cap drops the weakest tail.
  TEST_ASSERT_TRUE(out.indexOf("\"net0\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"net24\"") < 0);
}

static void test_late_stronger_entry_displaces_weakest_when_full() {
  // The cap test above only appends (monotonically weaker input); this one
  // forces the other branch: the list is already full when a mid-strength
  // entry arrives, so it must insert mid-list, shift the tail down, and
  // push the weakest kept network out.
  WifiScanEntry e[WIFI_SCAN_JSON_MAX + 1];
  for (int i = 0; i < WIFI_SCAN_JSON_MAX; i++) {
    e[i] = {String("net") + String(i), -30 - 2 * i, true};  // -30 .. -68
  }
  e[WIFI_SCAN_JSON_MAX] = {String("late"), -37, true};  // between net3/net4
  String out = buildWifiScanJson(e, WIFI_SCAN_JSON_MAX + 1);
  int rows = 0;
  for (int at = out.indexOf("\"ssid\""); at >= 0;
       at = out.indexOf("\"ssid\"", at + 1)) {
    rows++;
  }
  TEST_ASSERT_EQUAL_INT(WIFI_SCAN_JSON_MAX, rows);
  int atNet3 = out.indexOf("\"net3\"");
  int atLate = out.indexOf("\"late\"");
  int atNet4 = out.indexOf("\"net4\"");
  TEST_ASSERT_TRUE(atNet3 >= 0 && atLate >= 0 && atNet4 >= 0);
  TEST_ASSERT_TRUE(atNet3 < atLate);   // slots into rssi order...
  TEST_ASSERT_TRUE(atLate < atNet4);   // ...shifting the tail down
  TEST_ASSERT_TRUE(out.indexOf("\"net19\"") < 0);  // weakest evicted
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_empty_scan_yields_empty_array);
  RUN_TEST(test_single_network_renders_all_fields);
  RUN_TEST(test_open_network_renders_secure_false);
  RUN_TEST(test_networks_sorted_strongest_first);
  RUN_TEST(test_duplicate_ssids_keep_strongest_only);
  RUN_TEST(test_hidden_ssids_are_dropped);
  RUN_TEST(test_ssid_json_metacharacters_are_escaped);
  RUN_TEST(test_list_caps_at_maximum);
  RUN_TEST(test_late_stronger_entry_displaces_weakest_when_full);
  return UNITY_END();
}
