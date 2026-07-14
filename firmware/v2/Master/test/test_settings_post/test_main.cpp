// Host-side tests for the POST / staging pipeline (#186).
// v1 semantics carried over: every field optional (per-card saves, #128),
// all-or-nothing per POST (an invalid field rejects the whole submission),
// overlay across POSTs (a second card's post before the drain adds its own
// fields), write-only MQTT password, only-if-changed persistence.

#include <ArduinoFake.h>
#include <unity.h>

#include <map>
#include <string>

#include "../../SettingsStore.h"
#include "../../PendingSettingsPost.h"

void setUp() {}
void tearDown() {}

class FakeSettingsStore : public SettingsStore {
 public:
  String getString(const char* key, const String& def) override {
    auto it = strings_.find(key);
    return it == strings_.end() ? def : String(it->second.c_str());
  }
  void putString(const char* key, const String& value) override {
    strings_[key] = value.c_str();
    writes_++;
  }
  int getInt(const char* key, int def) override {
    auto it = ints_.find(key);
    return it == ints_.end() ? def : it->second;
  }
  void putInt(const char* key, int value) override {
    ints_[key] = value;
    writes_++;
  }
  void remove(const char* key) override {
    strings_.erase(key);
    ints_.erase(key);
  }

  bool hasString(const char* key) const { return strings_.count(key) > 0; }
  int writeCount() const { return writes_; }

 private:
  std::map<std::string, std::string> strings_;
  std::map<std::string, int> ints_;
  int writes_ = 0;
};

static MasterSettings liveDefaults() {
  MasterSettings s;
  s.alignment = "left";
  s.flapSpeed = 80;
  s.deviceMode = "text";
  s.timezonePosix = "UTC0";
  s.deviceName = "";
  s.mqttHost = "";
  s.mqttPort = 1883;
  s.mqttUser = "";
  s.mqttPassword = "";
  return s;
}

// ---------------------------------------------------------------------------
// stageSettingsParam: per-field validation and normalization.
// ---------------------------------------------------------------------------

static void test_unknown_param_is_ignored() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL(SettingsParamResult::Ignored,
                    stageSettingsParam(post, "bogus", "1"));
  TEST_ASSERT_FALSE(post.alignmentProvided);
}

static void test_valid_alignment_is_staged() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL(SettingsParamResult::Accepted,
                    stageSettingsParam(post, PARAM_ALIGNMENT, "center"));
  TEST_ASSERT_TRUE(post.alignmentProvided);
  TEST_ASSERT_EQUAL_STRING("center", post.alignment.c_str());
}

static void test_invalid_alignment_is_rejected_not_staged() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL(SettingsParamResult::Invalid,
                    stageSettingsParam(post, PARAM_ALIGNMENT, "diagonal"));
  TEST_ASSERT_FALSE(post.alignmentProvided);
}

static void test_flap_speed_bounds() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL(SettingsParamResult::Invalid,
                    stageSettingsParam(post, PARAM_FLAP_SPEED, "0"));
  TEST_ASSERT_EQUAL(SettingsParamResult::Invalid,
                    stageSettingsParam(post, PARAM_FLAP_SPEED, "101"));
  TEST_ASSERT_EQUAL(SettingsParamResult::Accepted,
                    stageSettingsParam(post, PARAM_FLAP_SPEED, "100"));
  TEST_ASSERT_EQUAL_STRING("100", post.flapSpeed.c_str());
}

static void test_device_mode_validated() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL(SettingsParamResult::Accepted,
                    stageSettingsParam(post, PARAM_DEVICEMODE, "clock"));
  TEST_ASSERT_EQUAL(SettingsParamResult::Invalid,
                    stageSettingsParam(post, PARAM_DEVICEMODE, "disco"));
}

static void test_input_text_accepted_verbatim() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL(SettingsParamResult::Accepted,
                    stageSettingsParam(post, PARAM_INPUT_TEXT, "HELLO WORLD"));
  TEST_ASSERT_TRUE(post.inputTextProvided);
  TEST_ASSERT_EQUAL_STRING("HELLO WORLD", post.inputText.c_str());
}

static void test_timezone_length_bounded() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL(SettingsParamResult::Accepted,
                    stageSettingsParam(post, PARAM_TIMEZONE,
                                       "CET-1CEST,M3.5.0,M10.5.0/3"));
  String tooLong;
  for (int i = 0; i < LEN_TIMEZONE + 1; i++) tooLong += 'X';
  TEST_ASSERT_EQUAL(SettingsParamResult::Invalid,
                    stageSettingsParam(post, PARAM_TIMEZONE, tooLong));
}

static void test_device_name_is_normalized_lowercase() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL(SettingsParamResult::Accepted,
                    stageSettingsParam(post, PARAM_DEVICE_NAME, "KITCHEN"));
  TEST_ASSERT_EQUAL_STRING("kitchen", post.deviceName.c_str());
}

static void test_empty_device_name_means_reset_to_default() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL(SettingsParamResult::Accepted,
                    stageSettingsParam(post, PARAM_DEVICE_NAME, ""));
  TEST_ASSERT_TRUE(post.deviceNameProvided);
  TEST_ASSERT_EQUAL_STRING("", post.deviceName.c_str());
}

static void test_invalid_device_name_rejected() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL(SettingsParamResult::Invalid,
                    stageSettingsParam(post, PARAM_DEVICE_NAME, "my display!"));
}

static void test_mqtt_host_trimmed_before_validation() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL(SettingsParamResult::Accepted,
                    stageSettingsParam(post, PARAM_MQTT_HOST, "  broker.local  "));
  TEST_ASSERT_EQUAL_STRING("broker.local", post.mqttHost.c_str());
}

static void test_mqtt_host_with_inner_space_rejected() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL(SettingsParamResult::Invalid,
                    stageSettingsParam(post, PARAM_MQTT_HOST, "bro ker"));
}

static void test_mqtt_port_validated() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL(SettingsParamResult::Accepted,
                    stageSettingsParam(post, PARAM_MQTT_PORT, "8883"));
  TEST_ASSERT_EQUAL(SettingsParamResult::Invalid,
                    stageSettingsParam(post, PARAM_MQTT_PORT, "70000"));
}

static void test_empty_mqtt_password_is_ignored_keep_stored() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL(SettingsParamResult::Ignored,
                    stageSettingsParam(post, PARAM_MQTT_PASSWORD, ""));
  TEST_ASSERT_FALSE(post.mqttPasswordProvided);
}

static void test_nonempty_mqtt_password_is_staged() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL(SettingsParamResult::Accepted,
                    stageSettingsParam(post, PARAM_MQTT_PASSWORD, "s3cret"));
  TEST_ASSERT_TRUE(post.mqttPasswordProvided);
}

static void test_transient_dwell_trimmed_and_bounded() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL(SettingsParamResult::Accepted,
                    stageSettingsParam(post, PARAM_TRANSIENT_DWELL, " 60 "));
  TEST_ASSERT_EQUAL(60, (int)post.transientDwell);
  TEST_ASSERT_EQUAL(SettingsParamResult::Invalid,
                    stageSettingsParam(post, PARAM_TRANSIENT_DWELL, "4"));
}

// ---------------------------------------------------------------------------
// settingsPostConsistent: cross-field rule (#176 — dwell needs its text).
// ---------------------------------------------------------------------------

static void test_dwell_without_text_is_inconsistent() {
  PendingSettingsPost post;
  stageSettingsParam(post, PARAM_TRANSIENT_DWELL, "60");
  TEST_ASSERT_FALSE(settingsPostConsistent(post));
}

static void test_dwell_with_text_is_consistent() {
  PendingSettingsPost post;
  stageSettingsParam(post, PARAM_TRANSIENT_TEXT, "CALIBRATE");
  stageSettingsParam(post, PARAM_TRANSIENT_DWELL, "60");
  TEST_ASSERT_TRUE(settingsPostConsistent(post));
}

// ---------------------------------------------------------------------------
// settingsPostNeedsReboot: verdict for the "ok-reboot" response (#128).
// ---------------------------------------------------------------------------

static void test_alignment_only_needs_no_reboot() {
  PendingSettingsPost post;
  stageSettingsParam(post, PARAM_ALIGNMENT, "center");
  TEST_ASSERT_FALSE(settingsPostNeedsReboot(post, liveDefaults()));
}

static void test_device_name_change_needs_reboot() {
  PendingSettingsPost post;
  stageSettingsParam(post, PARAM_DEVICE_NAME, "kitchen");
  TEST_ASSERT_TRUE(settingsPostNeedsReboot(post, liveDefaults()));
}

static void test_same_device_name_needs_no_reboot() {
  PendingSettingsPost post;
  stageSettingsParam(post, PARAM_DEVICE_NAME, "kitchen");
  MasterSettings live = liveDefaults();
  live.deviceName = "kitchen";
  TEST_ASSERT_FALSE(settingsPostNeedsReboot(post, live));
}

static void test_mqtt_change_needs_reboot() {
  PendingSettingsPost post;
  stageSettingsParam(post, PARAM_MQTT_HOST, "broker.local");
  TEST_ASSERT_TRUE(settingsPostNeedsReboot(post, liveDefaults()));
}

static void test_same_mqtt_port_needs_no_reboot() {
  PendingSettingsPost post;
  stageSettingsParam(post, PARAM_MQTT_PORT, "1883");
  TEST_ASSERT_FALSE(settingsPostNeedsReboot(post, liveDefaults()));
}

// ---------------------------------------------------------------------------
// mergeSettingsPost: overlay across POSTs.
// ---------------------------------------------------------------------------

static void test_merge_overlays_fields_from_two_posts() {
  PendingSettingsPost shared;

  PendingSettingsPost a;
  stageSettingsParam(a, PARAM_ALIGNMENT, "right");
  mergeSettingsPost(shared, a);

  PendingSettingsPost b;
  stageSettingsParam(b, PARAM_MQTT_HOST, "broker.local");
  mergeSettingsPost(shared, b);

  TEST_ASSERT_TRUE(shared.pending);
  TEST_ASSERT_TRUE(shared.alignmentProvided);
  TEST_ASSERT_EQUAL_STRING("right", shared.alignment.c_str());
  TEST_ASSERT_TRUE(shared.mqttHostProvided);
  TEST_ASSERT_EQUAL_STRING("broker.local", shared.mqttHost.c_str());
}

static void test_merge_second_post_wins_same_field() {
  PendingSettingsPost shared;

  PendingSettingsPost a;
  stageSettingsParam(a, PARAM_ALIGNMENT, "right");
  mergeSettingsPost(shared, a);

  PendingSettingsPost b;
  stageSettingsParam(b, PARAM_ALIGNMENT, "left");
  mergeSettingsPost(shared, b);

  TEST_ASSERT_EQUAL_STRING("left", shared.alignment.c_str());
}

// ---------------------------------------------------------------------------
// applySettingsPost: drain to settings + store, then reset.
// ---------------------------------------------------------------------------

static void test_apply_updates_settings_and_store() {
  FakeSettingsStore store;
  MasterSettings settings = liveDefaults();
  PendingSettingsPost post;
  stageSettingsParam(post, PARAM_ALIGNMENT, "center");
  stageSettingsParam(post, PARAM_FLAP_SPEED, "42");
  stageSettingsParam(post, PARAM_DEVICEMODE, "clock");
  stageSettingsParam(post, PARAM_TIMEZONE, "EST5EDT,M3.2.0,M11.1.0");
  post.pending = true;

  applySettingsPost(post, settings, store);

  TEST_ASSERT_EQUAL_STRING("center", settings.alignment.c_str());
  TEST_ASSERT_EQUAL_INT(42, settings.flapSpeed);
  TEST_ASSERT_EQUAL_STRING("clock", settings.deviceMode.c_str());
  TEST_ASSERT_EQUAL_STRING("EST5EDT,M3.2.0,M11.1.0", settings.timezonePosix.c_str());

  MasterSettings reloaded = loadSettings(store);
  TEST_ASSERT_EQUAL_STRING("center", reloaded.alignment.c_str());
  TEST_ASSERT_EQUAL_INT(42, reloaded.flapSpeed);
}

static void test_apply_skips_unchanged_values() {
  FakeSettingsStore store;
  MasterSettings settings = liveDefaults();
  PendingSettingsPost post;
  stageSettingsParam(post, PARAM_ALIGNMENT, "left");  // same as live
  post.pending = true;

  applySettingsPost(post, settings, store);
  TEST_ASSERT_EQUAL_INT(0, store.writeCount());
}

static void test_apply_only_touches_provided_fields() {
  FakeSettingsStore store;
  MasterSettings settings = liveDefaults();
  settings.deviceName = "kitchen";
  PendingSettingsPost post;
  stageSettingsParam(post, PARAM_ALIGNMENT, "center");
  post.pending = true;

  applySettingsPost(post, settings, store);
  TEST_ASSERT_EQUAL_STRING("kitchen", settings.deviceName.c_str());
  TEST_ASSERT_FALSE(store.hasString(SETTINGS_KEY_DEVICE_NAME));
}

static void test_apply_mqtt_keeps_stored_password_when_not_provided() {
  FakeSettingsStore store;
  store.putString(SETTINGS_KEY_MQTT_PASS, "oldsecret");
  MasterSettings settings = liveDefaults();
  settings.mqttPassword = "oldsecret";

  PendingSettingsPost post;
  stageSettingsParam(post, PARAM_MQTT_HOST, "broker.local");
  post.pending = true;
  applySettingsPost(post, settings, store);

  TEST_ASSERT_EQUAL_STRING("broker.local", settings.mqttHost.c_str());
  TEST_ASSERT_EQUAL_STRING("oldsecret",
                           store.getString(SETTINGS_KEY_MQTT_PASS, "").c_str());
}

static void test_apply_writes_new_password_when_provided() {
  FakeSettingsStore store;
  MasterSettings settings = liveDefaults();
  PendingSettingsPost post;
  stageSettingsParam(post, PARAM_MQTT_PASSWORD, "newsecret");
  post.pending = true;
  applySettingsPost(post, settings, store);

  TEST_ASSERT_EQUAL_STRING("newsecret",
                           store.getString(SETTINGS_KEY_MQTT_PASS, "").c_str());
  TEST_ASSERT_EQUAL_STRING("newsecret", settings.mqttPassword.c_str());
}

static void test_apply_resets_post_including_secrets() {
  FakeSettingsStore store;
  MasterSettings settings = liveDefaults();
  PendingSettingsPost post;
  stageSettingsParam(post, PARAM_MQTT_PASSWORD, "newsecret");
  stageSettingsParam(post, PARAM_INPUT_TEXT, "HELLO");
  post.pending = true;
  applySettingsPost(post, settings, store);

  TEST_ASSERT_FALSE(post.pending);
  TEST_ASSERT_FALSE(post.mqttPasswordProvided);
  TEST_ASSERT_EQUAL_STRING("", post.mqttPassword.c_str());
  TEST_ASSERT_FALSE(post.inputTextProvided);
  TEST_ASSERT_EQUAL_STRING("", post.inputText.c_str());
}

// --- unit-count override (#289) ---------------------------------------------

static void test_stage_unit_count_accepts_and_trims() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL((int)SettingsParamResult::Accepted,
                    (int)stageSettingsParam(post, PARAM_UNIT_COUNT, " 8 "));
  TEST_ASSERT_TRUE(post.unitCountProvided);
  TEST_ASSERT_EQUAL_STRING("8", post.unitCount.c_str());
}

static void test_stage_unit_count_rejects_out_of_range() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL((int)SettingsParamResult::Invalid,
                    (int)stageSettingsParam(post, PARAM_UNIT_COUNT, "17"));
  TEST_ASSERT_EQUAL((int)SettingsParamResult::Invalid,
                    (int)stageSettingsParam(post, PARAM_UNIT_COUNT, "abc"));
  TEST_ASSERT_FALSE(post.unitCountProvided);
}

static void test_apply_persists_unit_count_override() {
  FakeSettingsStore store;
  MasterSettings settings = loadSettings(store);
  PendingSettingsPost post;
  stageSettingsParam(post, PARAM_UNIT_COUNT, "8");
  applySettingsPost(post, settings, store);
  TEST_ASSERT_EQUAL_INT(8, settings.unitCountOverride);
  TEST_ASSERT_EQUAL_INT(8, loadSettings(store).unitCountOverride);
  // Back to auto.
  stageSettingsParam(post, PARAM_UNIT_COUNT, "0");
  applySettingsPost(post, settings, store);
  TEST_ASSERT_EQUAL_INT(0, settings.unitCountOverride);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_unknown_param_is_ignored);
  RUN_TEST(test_valid_alignment_is_staged);
  RUN_TEST(test_invalid_alignment_is_rejected_not_staged);
  RUN_TEST(test_flap_speed_bounds);
  RUN_TEST(test_device_mode_validated);
  RUN_TEST(test_input_text_accepted_verbatim);
  RUN_TEST(test_timezone_length_bounded);
  RUN_TEST(test_device_name_is_normalized_lowercase);
  RUN_TEST(test_empty_device_name_means_reset_to_default);
  RUN_TEST(test_invalid_device_name_rejected);
  RUN_TEST(test_mqtt_host_trimmed_before_validation);
  RUN_TEST(test_mqtt_host_with_inner_space_rejected);
  RUN_TEST(test_mqtt_port_validated);
  RUN_TEST(test_empty_mqtt_password_is_ignored_keep_stored);
  RUN_TEST(test_nonempty_mqtt_password_is_staged);
  RUN_TEST(test_transient_dwell_trimmed_and_bounded);
  RUN_TEST(test_dwell_without_text_is_inconsistent);
  RUN_TEST(test_dwell_with_text_is_consistent);
  RUN_TEST(test_alignment_only_needs_no_reboot);
  RUN_TEST(test_device_name_change_needs_reboot);
  RUN_TEST(test_same_device_name_needs_no_reboot);
  RUN_TEST(test_mqtt_change_needs_reboot);
  RUN_TEST(test_same_mqtt_port_needs_no_reboot);
  RUN_TEST(test_merge_overlays_fields_from_two_posts);
  RUN_TEST(test_merge_second_post_wins_same_field);
  RUN_TEST(test_apply_updates_settings_and_store);
  RUN_TEST(test_apply_skips_unchanged_values);
  RUN_TEST(test_apply_only_touches_provided_fields);
  RUN_TEST(test_apply_mqtt_keeps_stored_password_when_not_provided);
  RUN_TEST(test_apply_writes_new_password_when_provided);
  RUN_TEST(test_apply_resets_post_including_secrets);
  RUN_TEST(test_stage_unit_count_accepts_and_trims);
  RUN_TEST(test_stage_unit_count_rejects_out_of_range);
  RUN_TEST(test_apply_persists_unit_count_override);
  return UNITY_END();
}
