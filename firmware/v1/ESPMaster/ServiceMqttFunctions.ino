//MQTT / Home Assistant integration (issue #121). Design doc:
//docs/superpowers/specs/2026-07-05-mqtt-ha-integration-design.md
//
//The whole implementation is gated on MQTT_ENABLE (default false in
//ESPMaster.ino) — the #else branch at the bottom provides no-op stubs so
//the call sites in ESPMaster.ino compile without their own guards, and
//--gc-sections drops AsyncMqttClient entirely from the default image.
//
//Threading model (non-negotiable, see design doc): AsyncMqttClient
//callbacks run in LWIP/sys context. They ONLY copy bytes and set flags.
//Everything that touches the display, I2C, Strings-at-scale, or publishes
//discovery happens in loopMqtt() on the main loop.

#if MQTT_ENABLE == true

#include <AsyncMqttClient.h>
#include "MqttHelpers.h"

static AsyncMqttClient mqttClient;

//Lifecycle state — all millis()-based, no tickers.
static bool mqttInitialised = false;
static bool mqttStoppedForOta = false;
static unsigned long mqttReconnectAtMs = 0;        //0 = no reconnect scheduled
static unsigned long mqttReconnectBackoffMs = 2000; //doubles per failure, 30 s cap
static unsigned long mqttNextTelemetryMs = 0;

//LWIP-context -> main-loop hand-off flags.
static volatile bool mqttJustConnected = false;
static volatile bool mqttMessagePending = false;
static char mqttRxBuffer[MQTT_MAX_TEXT_LEN + 1];

//Show-then-revert notification state (MqttHelpers.h).
static MqttNotification mqttNotification;

//Topics, resolved once in initMqtt(). AsyncMqttClient stores POINTERS for
//client id / credentials / will — these Strings must outlive the client,
//which is why they are globals and never reassigned after init.
static String mqttResolvedDeviceId;
static String mqttTopicSet;
static String mqttTopicAvailability;
static String mqttTopicTelemetry;

//LWIP context: flag only. Subscribe/discovery/availability happen in
//loopMqtt() so the 512-byte discovery buffers never live on the sys stack.
static void onMqttConnect(bool sessionPresent) {
  (void)sessionPresent;
  mqttJustConnected = true;
}

//LWIP context: schedule a retry; never call connect() from here.
static void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  (void)reason;
  mqttReconnectAtMs = millis() + mqttReconnectBackoffMs;
  mqttReconnectBackoffMs *= 2;
  if (mqttReconnectBackoffMs > 30000UL) mqttReconnectBackoffMs = 30000UL;
}

//LWIP context: copy the chunk into the fixed buffer (truncating past
//MQTT_MAX_TEXT_LEN — the display path truncates to the display width
//anyway) and flag the loop when the final chunk lands. Only one topic is
//subscribed, so no topic dispatch is needed.
static void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties,
                          size_t len, size_t index, size_t total) {
  (void)topic; (void)properties;
  for (size_t i = 0; i < len; i++) {
    size_t pos = index + i;
    if (pos >= MQTT_MAX_TEXT_LEN) break;
    mqttRxBuffer[pos] = payload[i];
  }
  if (index + len >= total) {
    size_t end = total > MQTT_MAX_TEXT_LEN ? MQTT_MAX_TEXT_LEN : total;
    mqttRxBuffer[end] = '\0';
    mqttMessagePending = true;
  }
}

//Publishes the four retained HA discovery configs. Main loop only — the
//buffers are stack-allocated here on the 4 KB cont stack.
//Truncation guard (Task 3 review): snprintf returns the would-be length;
//>= bufLen means the buffer was too small (e.g. a very long custom device
//id) and the JSON/topic is cut mid-string — publishing that would poison
//HA's retained config, so skip and log instead. Topic buffer is 96 bytes:
//the longest template (homeassistant/sensor/%s_unit_errors/config) has 40
//fixed chars, leaving room for device ids up to 55 chars.
static void publishMqttDiscovery() {
  char topicBuf[96];
  char payloadBuf[512];
  for (int entity = 0; entity < DISCOVERY_ENTITY_COUNT; entity++) {
    size_t tLen = buildDiscoveryTopic(topicBuf, sizeof(topicBuf), entity, mqttResolvedDeviceId.c_str());
    size_t pLen = buildDiscoveryPayload(payloadBuf, sizeof(payloadBuf), entity, mqttResolvedDeviceId.c_str(), espVersion);
    if (tLen == 0 || tLen >= sizeof(topicBuf) || pLen == 0 || pLen >= sizeof(payloadBuf)) {
      SerialPrint(F("MQTT: discovery entity "));
      SerialPrint(entity);
      SerialPrintln(F(" skipped — topic/payload would truncate (device id too long?)"));
      continue;
    }
    mqttClient.publish(topicBuf, 0, true, payloadBuf);
  }
}

//Called once from setup(), normal boots only (never quiet-OTA or recovery —
//setup() returns before reaching the call in those modes).
void initMqtt() {
  if (mqttBrokerHost[0] == '\0') {
    SerialPrintln(F("MQTT: no broker configured (MqttCredentials.h) — MQTT disabled"));
    return;
  }
  mqttResolvedDeviceId  = (mqttDeviceId[0] != '\0') ? String(mqttDeviceId) : String(mdnsName);
  mqttTopicSet          = mqttTopic(mqttResolvedDeviceId, "text/set");
  mqttTopicAvailability = mqttTopic(mqttResolvedDeviceId, "availability");
  mqttTopicTelemetry    = mqttTopic(mqttResolvedDeviceId, "telemetry");

  mqttClient.setServer(mqttBrokerHost, mqttBrokerPort);
  if (mqttUsername[0] != '\0') {
    mqttClient.setCredentials(mqttUsername, mqttPassword);
  }
  mqttClient.setClientId(mqttResolvedDeviceId.c_str());
  mqttClient.setKeepAlive(30);
  //Last Will: broker publishes retained "offline" if we vanish uncleanly.
  mqttClient.setWill(mqttTopicAvailability.c_str(), 1, true, "offline");
  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onMessage(onMqttMessage);

  mqttInitialised = true;
  mqttReconnectAtMs = millis();  //first connect attempt on the next loop pass
  SerialPrint(F("MQTT: initialised, device id: "));
  SerialPrintln(mqttResolvedDeviceId);
}

//Main-loop pump: reconnect schedule, post-connect setup, inbound messages,
//periodic telemetry. Broker down = silent retry with backoff; the display
//stays fully functional throughout.
void loopMqtt() {
  if (!mqttInitialised || mqttStoppedForOta) {
    return;
  }

  //Event-driven reconnect (design doc: 2 s -> 30 s exponential backoff),
  //only while WiFi is up. millis() comparison is wraparound-safe.
  if (!mqttClient.connected() && mqttReconnectAtMs != 0 &&
      (long)(millis() - mqttReconnectAtMs) >= 0 &&
      WiFi.status() == WL_CONNECTED) {
    mqttReconnectAtMs = 0;
    SerialPrintln(F("MQTT: connecting to broker..."));
    mqttClient.connect();
  }

  if (mqttJustConnected) {
    mqttJustConnected = false;
    mqttReconnectBackoffMs = 2000;  //healthy connection resets the backoff
    SerialPrintln(F("MQTT: connected"));
    mqttClient.subscribe(mqttTopicSet.c_str(), 0);
    mqttClient.publish(mqttTopicAvailability.c_str(), 1, true, "online");
    publishMqttDiscovery();
    mqttNextTelemetryMs = millis();  //first telemetry immediately
  }

  if (mqttMessagePending) {
    mqttMessagePending = false;
    String payload(mqttRxBuffer);
    String text;
    long dwellSeconds = MQTT_TEXT_DWELL_S;
    if (!parseMqttTextPayload(payload, text, dwellSeconds)) {
      //Malformed/plain payload → show it verbatim with the default dwell.
      text = payload;
      dwellSeconds = MQTT_TEXT_DWELL_S;
    }
    SerialPrint(F("MQTT: notification (dwell "));
    SerialPrint(dwellSeconds);
    SerialPrint(F(" s): "));
    SerialPrintln(text);
    notificationStart(mqttNotification, text, dwellSeconds, millis());
  }

  if (mqttClient.connected() && (long)(millis() - mqttNextTelemetryMs) >= 0) {
    mqttNextTelemetryMs = millis() + MQTT_TELEMETRY_INTERVAL_S * 1000UL;
    char buf[96];
    buildTelemetryPayload(buf, sizeof(buf), ESP.getFreeHeap(), WiFi.RSSI(), lastShowUnitWriteErrors);
    mqttClient.publish(mqttTopicTelemetry.c_str(), 0, false, buf);
  }
}

//Called from loop()'s once-per-second mode selection. Returns true while an
//MQTT notification owns the display (and pushes its text through the normal
//showText path — same cleaning/width handling as web-UI input). On expiry
//returns false; the caller's normal mode selection re-shows the previous
//clock/text content via showText's lastWrittenText comparison.
bool mqttNotificationTick() {
  if (!notificationTick(mqttNotification, millis())) {
    return false;
  }
  showText(mqttNotification.text);
  return true;
}

//Upload started (#116 freeze): publish a retained "offline" (a clean
//DISCONNECT discards the will, so HA would otherwise show a stale "online"
//through the whole flash) and drop the TCP connection so WiFi bandwidth
//and heap belong to the upload. Runs in async context — publish/disconnect
//only enqueue, which is safe there.
void mqttStopForOta() {
  if (!mqttInitialised || mqttStoppedForOta) {
    return;
  }
  mqttStoppedForOta = true;
  if (mqttClient.connected()) {
    mqttClient.publish(mqttTopicAvailability.c_str(), 1, true, "offline");
    mqttClient.disconnect();
  }
  SerialPrintln(F("MQTT: stopped for master OTA upload"));
}

//Upload failed or stalled (display thawed): resume MQTT. On the success
//path the device reboots instead, so this never runs there.
void mqttResumeAfterOta() {
  if (!mqttInitialised || !mqttStoppedForOta) {
    return;
  }
  mqttStoppedForOta = false;
  mqttReconnectBackoffMs = 2000;
  mqttReconnectAtMs = millis();
  SerialPrintln(F("MQTT: resuming after OTA upload ended without reboot"));
}

#else  //MQTT_ENABLE == false — no-op stubs so call sites stay guard-free.

void initMqtt() {}
void loopMqtt() {}
bool mqttNotificationTick() { return false; }
void mqttStopForOta() {}
void mqttResumeAfterOta() {}

#endif
