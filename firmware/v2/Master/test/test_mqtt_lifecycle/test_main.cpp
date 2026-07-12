// Host-side unit tests for the pure MQTT lifecycle policy in
// MqttLifecyclePolicy.h (#224): reconnect backoff scheduling (v1
// ServiceMqttFunctions semantics — 2 s doubling to a 30 s cap, reset on a
// healthy connect, wraparound-safe due check) and command-topic
// classification (unknown topics fail closed).

#include <ArduinoFake.h>
#include <unity.h>
#include "../../MqttLifecyclePolicy.h"

using namespace fakeit;

void setUp() {
  ArduinoFakeReset();
}
void tearDown() {}

// ---- backoff scheduling ----

static void test_init_arms_an_immediate_first_attempt() {
  MqttBackoffState st;
  mqttBackoffInit(st, 5000);
  TEST_ASSERT_TRUE(mqttBackoffShouldAttempt(st, 5000, true, false));
}

static void test_no_attempt_while_wifi_is_down() {
  MqttBackoffState st;
  mqttBackoffInit(st, 5000);
  TEST_ASSERT_FALSE(mqttBackoffShouldAttempt(st, 5000, false, false));
}

static void test_no_attempt_while_connected() {
  MqttBackoffState st;
  mqttBackoffInit(st, 5000);
  TEST_ASSERT_FALSE(mqttBackoffShouldAttempt(st, 5000, true, true));
}

static void test_no_attempt_before_the_scheduled_time() {
  MqttBackoffState st;
  mqttBackoffInit(st, 5000);
  mqttBackoffAttemptStarted(st);
  mqttBackoffOnDisconnect(st, 10000);  // schedules 10000 + 2000
  TEST_ASSERT_FALSE(mqttBackoffShouldAttempt(st, 11999, true, false));
  TEST_ASSERT_TRUE(mqttBackoffShouldAttempt(st, 12000, true, false));
}

static void test_attempt_started_clears_pending() {
  MqttBackoffState st;
  mqttBackoffInit(st, 5000);
  mqttBackoffAttemptStarted(st);
  TEST_ASSERT_FALSE(mqttBackoffShouldAttempt(st, 5000, true, false));
}

static void test_backoff_doubles_per_failure_and_caps_at_30s() {
  MqttBackoffState st;
  mqttBackoffInit(st, 0);
  // Attempt delays (v1): 2000 -> 4000 -> 8000 -> 16000 -> 30000 (cap) ->
  // stays 30000. Each disconnect at nowMs=0 schedules at 0 + delay.
  mqttBackoffOnDisconnect(st, 0);
  TEST_ASSERT_EQUAL_UINT32(2000, st.attemptAtMs);
  mqttBackoffOnDisconnect(st, 0);
  TEST_ASSERT_EQUAL_UINT32(4000, st.attemptAtMs);
  mqttBackoffOnDisconnect(st, 0);
  TEST_ASSERT_EQUAL_UINT32(8000, st.attemptAtMs);
  mqttBackoffOnDisconnect(st, 0);
  TEST_ASSERT_EQUAL_UINT32(16000, st.attemptAtMs);
  mqttBackoffOnDisconnect(st, 0);
  TEST_ASSERT_EQUAL_UINT32(30000, st.attemptAtMs);
  mqttBackoffOnDisconnect(st, 0);
  TEST_ASSERT_EQUAL_UINT32(30000, st.attemptAtMs);
}

static void test_healthy_connect_resets_the_backoff() {
  MqttBackoffState st;
  mqttBackoffInit(st, 0);
  mqttBackoffOnDisconnect(st, 0);
  mqttBackoffOnDisconnect(st, 0);
  mqttBackoffOnConnect(st);
  mqttBackoffOnDisconnect(st, 0);
  TEST_ASSERT_EQUAL_UINT32(2000, st.attemptAtMs);
}

static void test_due_check_survives_millis_wraparound() {
  MqttBackoffState st;
  mqttBackoffInit(st, 0);
  mqttBackoffAttemptStarted(st);
  // Scheduled just before the uint32 wrap; "now" is just after it.
  mqttBackoffOnDisconnect(st, 0xFFFFFF00UL);  // due at 0xFFFFFF00 + 2000
  TEST_ASSERT_FALSE(mqttBackoffShouldAttempt(st, 0xFFFFFFFEUL, true, false));
  TEST_ASSERT_TRUE(mqttBackoffShouldAttempt(st, 0x000007D0UL, true, false));
}

// ---- command-topic classification ----

static MqttCommandTopics kitchenTopics() {
  return makeMqttCommandTopics(String("kitchen"));
}

static void test_classifies_every_command_topic() {
  MqttCommandTopics t = kitchenTopics();
  TEST_ASSERT_EQUAL(MqttCommand::Text,
                    classifyMqttCommandTopic(t, "splitflap/kitchen/text/set"));
  TEST_ASSERT_EQUAL(MqttCommand::Mode,
                    classifyMqttCommandTopic(t, "splitflap/kitchen/mode/set"));
  TEST_ASSERT_EQUAL(MqttCommand::Speed,
                    classifyMqttCommandTopic(t, "splitflap/kitchen/speed/set"));
  TEST_ASSERT_EQUAL(
      MqttCommand::Alignment,
      classifyMqttCommandTopic(t, "splitflap/kitchen/alignment/set"));
  TEST_ASSERT_EQUAL(
      MqttCommand::Restart,
      classifyMqttCommandTopic(t, "splitflap/kitchen/restart/set"));
}

static void test_unknown_topics_fail_closed() {
  MqttCommandTopics t = kitchenTopics();
  TEST_ASSERT_EQUAL(MqttCommand::None,
                    classifyMqttCommandTopic(t, "splitflap/kitchen/text"));
  TEST_ASSERT_EQUAL(MqttCommand::None,
                    classifyMqttCommandTopic(t, "splitflap/kitchen/"));
  TEST_ASSERT_EQUAL(MqttCommand::None, classifyMqttCommandTopic(t, ""));
  // State topics must never classify as commands.
  TEST_ASSERT_EQUAL(MqttCommand::None,
                    classifyMqttCommandTopic(t, "splitflap/kitchen/mode"));
}

static void test_other_device_ids_fail_closed() {
  MqttCommandTopics t = kitchenTopics();
  TEST_ASSERT_EQUAL(MqttCommand::None,
                    classifyMqttCommandTopic(t, "splitflap/hallway/text/set"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_init_arms_an_immediate_first_attempt);
  RUN_TEST(test_no_attempt_while_wifi_is_down);
  RUN_TEST(test_no_attempt_while_connected);
  RUN_TEST(test_no_attempt_before_the_scheduled_time);
  RUN_TEST(test_attempt_started_clears_pending);
  RUN_TEST(test_backoff_doubles_per_failure_and_caps_at_30s);
  RUN_TEST(test_healthy_connect_resets_the_backoff);
  RUN_TEST(test_due_check_survives_millis_wraparound);
  RUN_TEST(test_classifies_every_command_topic);
  RUN_TEST(test_unknown_topics_fail_closed);
  RUN_TEST(test_other_device_ids_fail_closed);
  return UNITY_END();
}
