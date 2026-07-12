// Host-side unit tests for the v2 settings layer (NVS-backed on target).
// The logic under test is storage-agnostic: loadSettings() applies
// v1-parity defaults, sanitizes corrupted values back to defaults, and the
// save helpers carry the v1 write-only MQTT password rule. The fake store
// below stands in for NvsSettingsStore, which is a thin Preferences wrapper
// exercised on hardware only.

#include <ArduinoFake.h>
#include <unity.h>

#include <map>
#include <string>

#include "../../SettingsStore.h"
#include "../../Settings.h"

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// In-memory SettingsStore double.
// ---------------------------------------------------------------------------

class FakeSettingsStore : public SettingsStore {
 public:
  String getString(const char* key, const String& def) override {
    auto it = strings_.find(key);
    return it == strings_.end() ? def : String(it->second.c_str());
  }
  void putString(const char* key, const String& value) override {
    strings_[key] = value.c_str();
  }
  int getInt(const char* key, int def) override {
    auto it = ints_.find(key);
    return it == ints_.end() ? def : it->second;
  }
  void putInt(const char* key, int value) override { ints_[key] = value; }
  void remove(const char* key) override {
    strings_.erase(key);
    ints_.erase(key);
  }

  bool hasString(const char* key) const { return strings_.count(key) > 0; }

 private:
  std::map<std::string, std::string> strings_;
  std::map<std::string, int> ints_;
};

// ---------------------------------------------------------------------------
// Defaults: an empty store yields v1 fresh-init parity.
// ---------------------------------------------------------------------------

static void test_empty_store_yields_v1_defaults() {
  FakeSettingsStore store;
  MasterSettings s = loadSettings(store);
  TEST_ASSERT_EQUAL_STRING("left", s.alignment.c_str());
  TEST_ASSERT_EQUAL_INT(80, s.flapSpeed);
  TEST_ASSERT_EQUAL_STRING("text", s.deviceMode.c_str());
  TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3", s.timezonePosix.c_str());
  TEST_ASSERT_EQUAL_STRING("", s.deviceName.c_str());
  TEST_ASSERT_EQUAL_STRING("", s.mqttHost.c_str());
  TEST_ASSERT_EQUAL_INT(1883, s.mqttPort);
  TEST_ASSERT_EQUAL_STRING("", s.mqttUser.c_str());
  TEST_ASSERT_EQUAL_STRING("", s.mqttPassword.c_str());
  TEST_ASSERT_EQUAL_STRING("", s.intendedVersion.c_str());
}

// ---------------------------------------------------------------------------
// Round-trips through the save helpers.
// ---------------------------------------------------------------------------

static void test_display_settings_round_trip() {
  FakeSettingsStore store;
  saveAlignment(store, String("center"));
  saveFlapSpeed(store, 42);
  saveDeviceMode(store, String("clock"));
  saveTimezone(store, String("UTC0"));

  MasterSettings s = loadSettings(store);
  TEST_ASSERT_EQUAL_STRING("center", s.alignment.c_str());
  TEST_ASSERT_EQUAL_INT(42, s.flapSpeed);
  TEST_ASSERT_EQUAL_STRING("clock", s.deviceMode.c_str());
  TEST_ASSERT_EQUAL_STRING("UTC0", s.timezonePosix.c_str());
}

static void test_identity_and_flash_slots_round_trip() {
  FakeSettingsStore store;
  saveDeviceName(store, String("hallway-board"));
  saveIntendedVersion(store, String("4795031"));

  MasterSettings s = loadSettings(store);
  TEST_ASSERT_EQUAL_STRING("hallway-board", s.deviceName.c_str());
  TEST_ASSERT_EQUAL_STRING("4795031", s.intendedVersion.c_str());
}

static void test_mqtt_config_round_trip() {
  FakeSettingsStore store;
  saveMqttConfig(store, String("broker.local"), 8883, String("ha"), String("s3cret"));

  MasterSettings s = loadSettings(store);
  TEST_ASSERT_EQUAL_STRING("broker.local", s.mqttHost.c_str());
  TEST_ASSERT_EQUAL_INT(8883, s.mqttPort);
  TEST_ASSERT_EQUAL_STRING("ha", s.mqttUser.c_str());
  TEST_ASSERT_EQUAL_STRING("s3cret", s.mqttPassword.c_str());
}

// ---------------------------------------------------------------------------
// Write-only password: an empty submission keeps the stored secret (#57
// semantics carried over from v1).
// ---------------------------------------------------------------------------

static void test_empty_password_submission_keeps_stored_secret() {
  FakeSettingsStore store;
  saveMqttConfig(store, String("broker.local"), 1883, String("ha"), String("s3cret"));
  saveMqttConfig(store, String("broker2.local"), 1884, String("ha2"), String(""));

  MasterSettings s = loadSettings(store);
  TEST_ASSERT_EQUAL_STRING("broker2.local", s.mqttHost.c_str());
  TEST_ASSERT_EQUAL_INT(1884, s.mqttPort);
  TEST_ASSERT_EQUAL_STRING("ha2", s.mqttUser.c_str());
  TEST_ASSERT_EQUAL_STRING("s3cret", s.mqttPassword.c_str());
}

static void test_nonempty_password_submission_replaces_secret() {
  FakeSettingsStore store;
  saveMqttConfig(store, String("broker.local"), 1883, String("ha"), String("old"));
  saveMqttConfig(store, String("broker.local"), 1883, String("ha"), String("new"));

  MasterSettings s = loadSettings(store);
  TEST_ASSERT_EQUAL_STRING("new", s.mqttPassword.c_str());
}

// ---------------------------------------------------------------------------
// Sanitize-on-load: corrupted stored values fall back to defaults instead of
// propagating into the running config (defensive parity with v1's
// assessSettingsBlob discipline — NVS guards integrity, not semantics).
// ---------------------------------------------------------------------------

static void test_garbage_alignment_falls_back_to_default() {
  FakeSettingsStore store;
  store.putString("alignment", String("middle"));
  TEST_ASSERT_EQUAL_STRING("left", loadSettings(store).alignment.c_str());
}

static void test_out_of_range_speed_falls_back_to_default() {
  FakeSettingsStore store;
  store.putInt("flapSpeed", 0);
  TEST_ASSERT_EQUAL_INT(80, loadSettings(store).flapSpeed);
  store.putInt("flapSpeed", 101);
  TEST_ASSERT_EQUAL_INT(80, loadSettings(store).flapSpeed);
}

static void test_garbage_device_mode_falls_back_to_default() {
  FakeSettingsStore store;
  store.putString("deviceMode", String("disco"));
  TEST_ASSERT_EQUAL_STRING("text", loadSettings(store).deviceMode.c_str());
}

static void test_unprintable_timezone_falls_back_to_default() {
  FakeSettingsStore store;
  String tz("CET");
  tz += (char)0x07;
  store.putString("tzPosix", tz);
  TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3",
                           loadSettings(store).timezonePosix.c_str());
}

static void test_invalid_device_name_falls_back_to_chip_default_sentinel() {
  FakeSettingsStore store;
  store.putString("deviceName", String("Bad Name!"));
  // "" is the sentinel that makes resolveDeviceName() fall back to the
  // chip-id default identity.
  TEST_ASSERT_EQUAL_STRING("", loadSettings(store).deviceName.c_str());
}

static void test_out_of_range_port_falls_back_to_default() {
  FakeSettingsStore store;
  store.putInt("mqttPort", 70000);
  TEST_ASSERT_EQUAL_INT(1883, loadSettings(store).mqttPort);
  store.putInt("mqttPort", 0);
  TEST_ASSERT_EQUAL_INT(1883, loadSettings(store).mqttPort);
}

// ---------------------------------------------------------------------------
// WiFi credentials (#188): our NVS namespace is the single credential store
// (no SDK-sector persistence on v2) — reset must DELETE the keys, and load
// must sanitize what buggier firmware may have written.
// ---------------------------------------------------------------------------

static void test_wifi_defaults_are_unprovisioned() {
  FakeSettingsStore store;
  MasterSettings s = loadSettings(store);
  TEST_ASSERT_EQUAL_STRING("", s.wifiSsid.c_str());
  TEST_ASSERT_EQUAL_STRING("", s.wifiPass.c_str());
}

static void test_wifi_credentials_round_trip() {
  FakeSettingsStore store;
  saveWifiCredentials(store, String("My Home WiFi"), String("hunter2!"));
  MasterSettings s = loadSettings(store);
  TEST_ASSERT_EQUAL_STRING("My Home WiFi", s.wifiSsid.c_str());
  TEST_ASSERT_EQUAL_STRING("hunter2!", s.wifiPass.c_str());
}

static void test_wifi_open_network_stores_empty_password() {
  FakeSettingsStore store;
  saveWifiCredentials(store, String("cafe-open"), String(""));
  MasterSettings s = loadSettings(store);
  TEST_ASSERT_EQUAL_STRING("cafe-open", s.wifiSsid.c_str());
  TEST_ASSERT_EQUAL_STRING("", s.wifiPass.c_str());
}

static void test_clear_wifi_credentials_deletes_both_keys() {
  FakeSettingsStore store;
  saveWifiCredentials(store, String("HomeNet"), String("hunter2!"));
  clearWifiCredentials(store);
  TEST_ASSERT_FALSE(store.hasString(SETTINGS_KEY_WIFI_SSID));
  TEST_ASSERT_FALSE(store.hasString(SETTINGS_KEY_WIFI_PASS));
  MasterSettings s = loadSettings(store);
  TEST_ASSERT_EQUAL_STRING("", s.wifiSsid.c_str());
  TEST_ASSERT_EQUAL_STRING("", s.wifiPass.c_str());
}

static void test_invalid_stored_ssid_clears_both_credentials() {
  // A password without a usable ssid is dead weight — the pair sanitizes
  // together and the boot lands in the portal.
  FakeSettingsStore store;
  String badSsid;
  for (int i = 0; i < 40; i++) badSsid += 's';  // over the 32-byte ceiling
  store.putString(SETTINGS_KEY_WIFI_SSID, badSsid);
  store.putString(SETTINGS_KEY_WIFI_PASS, String("hunter2!"));
  MasterSettings s = loadSettings(store);
  TEST_ASSERT_EQUAL_STRING("", s.wifiSsid.c_str());
  TEST_ASSERT_EQUAL_STRING("", s.wifiPass.c_str());
}

static void test_invalid_stored_password_clears_password_only() {
  // Keep the ssid: the join fails fast and the portal shows it prefilled
  // territory; clearing the pair would erase a recoverable network name.
  FakeSettingsStore store;
  store.putString(SETTINGS_KEY_WIFI_SSID, String("HomeNet"));
  String badPass = "pw";
  badPass += (char)0x01;
  store.putString(SETTINGS_KEY_WIFI_PASS, badPass);
  MasterSettings s = loadSettings(store);
  TEST_ASSERT_EQUAL_STRING("HomeNet", s.wifiSsid.c_str());
  TEST_ASSERT_EQUAL_STRING("", s.wifiPass.c_str());
}

// ---------------------------------------------------------------------------
// Intended-version sanitation (#191): the /firmware/master ?v= drain and the
// boot load path share one rule — printable ASCII shorter than
// LEN_INTENDED_VERSION, else "".
// ---------------------------------------------------------------------------

static void test_intended_version_passes_clean_rev_string() {
  TEST_ASSERT_EQUAL_STRING(
      "47ac69d-dirty", sanitizeIntendedVersion(String("47ac69d-dirty")).c_str());
}

static void test_overlong_intended_version_falls_back_to_empty() {
  String longRev;
  for (int i = 0; i < LEN_INTENDED_VERSION; i++) longRev += 'a';
  TEST_ASSERT_EQUAL_STRING("", sanitizeIntendedVersion(longRev).c_str());
}

static void test_unprintable_intended_version_falls_back_to_empty() {
  TEST_ASSERT_EQUAL_STRING(
      "", sanitizeIntendedVersion(String("47ac\r\n9d")).c_str());
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_wifi_defaults_are_unprovisioned);
  RUN_TEST(test_wifi_credentials_round_trip);
  RUN_TEST(test_wifi_open_network_stores_empty_password);
  RUN_TEST(test_clear_wifi_credentials_deletes_both_keys);
  RUN_TEST(test_invalid_stored_ssid_clears_both_credentials);
  RUN_TEST(test_invalid_stored_password_clears_password_only);
  RUN_TEST(test_empty_store_yields_v1_defaults);
  RUN_TEST(test_display_settings_round_trip);
  RUN_TEST(test_identity_and_flash_slots_round_trip);
  RUN_TEST(test_mqtt_config_round_trip);
  RUN_TEST(test_empty_password_submission_keeps_stored_secret);
  RUN_TEST(test_nonempty_password_submission_replaces_secret);
  RUN_TEST(test_garbage_alignment_falls_back_to_default);
  RUN_TEST(test_out_of_range_speed_falls_back_to_default);
  RUN_TEST(test_garbage_device_mode_falls_back_to_default);
  RUN_TEST(test_unprintable_timezone_falls_back_to_default);
  RUN_TEST(test_invalid_device_name_falls_back_to_chip_default_sentinel);
  RUN_TEST(test_out_of_range_port_falls_back_to_default);
  RUN_TEST(test_intended_version_passes_clean_rev_string);
  RUN_TEST(test_overlong_intended_version_falls_back_to_empty);
  RUN_TEST(test_unprintable_intended_version_falls_back_to_empty);
  return UNITY_END();
}
