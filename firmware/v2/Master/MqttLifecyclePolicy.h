#pragma once
// MqttLifecyclePolicy.h — pure MQTT connection-lifecycle decisions (#224).
//
// The v1 semantics from ServiceMqttFunctions.ino, extracted so they are
// natively tested (test/test_mqtt_lifecycle): exponential reconnect backoff
// (2 s doubling to a 30 s cap, reset by a healthy connect, wraparound-safe
// due check) and the inbound command-topic dispatch (exact matches only —
// an unexpected topic fails closed instead of landing in the notification
// path). No client types and no networking; MqttService.cpp owns those.

#include <Arduino.h>

#include "MqttHelpers.h"

#define MQTT_BACKOFF_BASE_MS 2000UL
#define MQTT_BACKOFF_CAP_MS 30000UL

struct MqttBackoffState {
  bool attemptPending = false;
  uint32_t attemptAtMs = 0;         // valid only while attemptPending
  uint32_t scheduledDelayMs = MQTT_BACKOFF_BASE_MS;  // next failure's delay
};

// Arm the first attempt for "now" — the first tick after init connects.
inline void mqttBackoffInit(MqttBackoffState& st, uint32_t nowMs) {
  st.attemptPending = true;
  st.attemptAtMs = nowMs;
  st.scheduledDelayMs = MQTT_BACKOFF_BASE_MS;
}

// Disconnect event: schedule the next attempt after the current delay, then
// double it (30 s cap) for the failure after that.
inline void mqttBackoffOnDisconnect(MqttBackoffState& st, uint32_t nowMs) {
  st.attemptPending = true;
  st.attemptAtMs = nowMs + st.scheduledDelayMs;
  st.scheduledDelayMs *= 2;
  if (st.scheduledDelayMs > MQTT_BACKOFF_CAP_MS) {
    st.scheduledDelayMs = MQTT_BACKOFF_CAP_MS;
  }
}

// Healthy connection: the next failure starts from the base delay again.
inline void mqttBackoffOnConnect(MqttBackoffState& st) {
  st.scheduledDelayMs = MQTT_BACKOFF_BASE_MS;
}

// True when a connect attempt should start now. The signed difference makes
// the comparison millis()-wraparound-safe (v1 idiom).
inline bool mqttBackoffShouldAttempt(const MqttBackoffState& st,
                                     uint32_t nowMs, bool wifiUp,
                                     bool connected) {
  return st.attemptPending && !connected && wifiUp &&
         (int32_t)(nowMs - st.attemptAtMs) >= 0;
}

inline void mqttBackoffAttemptStarted(MqttBackoffState& st) {
  st.attemptPending = false;
}

// ---- inbound command-topic dispatch ----

enum class MqttCommand : uint8_t { None, Text, Mode, Speed, Alignment, Restart };

// The five subscribed command topics, resolved once from the device id (the
// same stable-copy rule as the broker identity: never rebuilt per message).
struct MqttCommandTopics {
  String text;
  String mode;
  String speed;
  String alignment;
  String restart;
};

inline MqttCommandTopics makeMqttCommandTopics(const String& deviceId) {
  MqttCommandTopics t;
  t.text = mqttTopic(deviceId, "text/set");
  t.mode = mqttTopic(deviceId, "mode/set");
  t.speed = mqttTopic(deviceId, "speed/set");
  t.alignment = mqttTopic(deviceId, "alignment/set");
  t.restart = mqttTopic(deviceId, "restart/set");
  return t;
}

// Exact matches only; anything else is None (fail closed — v1 rule: an
// unexpected broker delivery must never land in the notification path).
inline MqttCommand classifyMqttCommandTopic(const MqttCommandTopics& t,
                                            const char* topic) {
  if (strcmp(topic, t.mode.c_str()) == 0) return MqttCommand::Mode;
  if (strcmp(topic, t.speed.c_str()) == 0) return MqttCommand::Speed;
  if (strcmp(topic, t.alignment.c_str()) == 0) return MqttCommand::Alignment;
  if (strcmp(topic, t.restart.c_str()) == 0) return MqttCommand::Restart;
  if (strcmp(topic, t.text.c_str()) == 0) return MqttCommand::Text;
  return MqttCommand::None;
}
