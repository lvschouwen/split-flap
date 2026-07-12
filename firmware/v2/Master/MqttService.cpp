// MQTT / Home Assistant service (#224) — v1 ServiceMqttFunctions.ino ported
// onto espMqttClient and the v2 task model. See MqttService.h for the
// threading contract; deviations from v1 are called out inline and in the
// spec (docs/superpowers/specs/2026-07-12-v2-mqtt-ha-slice.md).

#include "MqttService.h"

#include <WiFi.h>
#include <espMqttClient.h>

#include <atomic>
#include <time.h>

#include "BuildVersion.h"  // GIT_REV — the discovery device block's sw field
#include "ClockPolicy.h"
#include "DisplayCommand.h"
#include "DisplayIpc.h"
#include "HelpersSerialHandling.h"
#include "MqttHelpers.h"
#include "MqttLifecyclePolicy.h"
#include "OtaService.h"
#include "SplitFlapProtocol.h"
#include "UnitHealth.h"
#include "WebEndpoints.h"

#define MQTT_TELEMETRY_INTERVAL_S 60
#define MQTT_MAX_TEXT_LEN 256

// A frozen session must never outlive a torn upload forever (v1 resumed
// from loop() when the display thawed; v2's upload callbacks resume on
// their error paths, which misses a dead-TCP upload) — failsafe deadline.
#define MQTT_OTA_FREEZE_FAILSAFE_MS (10UL * 60UL * 1000UL)

// The inbox POD must fit the wire: the longest command topic is
// "splitflap/<name>/alignment/set" and text/set payloads run to
// MQTT_MAX_TEXT_LEN (v1 truncation rule).
static_assert(10 + DEVICE_NAME_MAX_LEN + 14 + 1 <=
                  sizeof(MqttInboxMessage::topic),
              "MqttInboxMessage.topic too small for a command topic");
static_assert(MQTT_MAX_TEXT_LEN + 1 <= sizeof(MqttInboxMessage::payload),
              "MqttInboxMessage.payload too small for a text/set payload");

// The client runs WITHOUT its internal task: the default ESP32 constructor
// would spawn one on core 1 — the display core. UseInternalTask::NO means
// loop() (and therefore every callback) runs inside mqttServiceTick() on
// mqttTask, core 0.
static espMqttClient mqttClient(espMqttClientTypes::UseInternalTask::NO);

// --- service state (mqttTask-owned unless noted) -----------------------------
static bool mqttInitialised = false;
static bool mqttStoppedForOta = false;
static uint32_t mqttFrozenAtMs = 0;
static MqttBackoffState mqttBackoff;
static uint32_t mqttNextTelemetryMs = 0;

// Cross-task staging (any task -> tick). Atomics, not a mutex: each is an
// independent single-shot request flag.
static std::atomic<bool> stopForOtaRequested{false};
static std::atomic<bool> resumeAfterOtaRequested{false};
static std::atomic<bool> cancelNotificationRequested{false};
static std::atomic<bool> discoveryClearRequested{false};
static std::atomic<bool> connectedAtomic{false};
static std::atomic<bool> notifActiveAtomic{false};

// Callback -> tick hand-off (same task here, but callbacks stay dumb —
// v1's copy+flag rule kept structural).
static volatile bool mqttJustConnected = false;
static volatile bool mqttDisconnectedEvent = false;

// Show-then-revert notification state (MqttHelpers.h), mqttTask-owned.
// notifActiveAtomic mirrors .active for the cross-core clockTask gate.
static MqttNotification mqttNotification;

// Stable broker identity + topics (espMqttClient stores raw POINTERS for
// host/client id/credentials/will — these must outlive the client and never
// be reassigned after init; the settings Strings can be reassigned by a web
// POST mid-run, which is exactly why the client gets its own copies, v1 #57).
static String mqttResolvedDeviceId;
static String mqttActiveHost;
static String mqttActiveUser;
static String mqttActivePassword;
static String mqttTopicAvailability;
static String mqttTopicTelemetry;
static String mqttTopicModeState;
static MqttCommandTopics mqttCmdTopics;
static String mqttFwVersion;
static uint32_t mqttBootCount = 0;

// Event-driven state trackers (v1 #132): publish a retained topic only on
// change; reset to sentinels on each (re)connect to force a full republish.
static String mqttLastPublishedMode;
static String mqttLastPublishedText = "\x01";
static int mqttLastPublishedNotif = -1;
static int mqttLastPublishedWidth = -1;
static int mqttLastPublishedUnits = -1;
static int mqttLastPublishedSpeed = -1;
static String mqttLastPublishedAlignment = "\x01";

// Inbound chunk assembly (callback context). MQTT delivers one message at a
// time per connection, so a single staging pair suffices; payloads past the
// buffer truncate (v1 rule — the display path truncates to width anyway).
static char rxTopic[sizeof(MqttInboxMessage::topic)];
static char rxPayload[sizeof(MqttInboxMessage::payload)];

static void onMqttConnect(bool /*sessionPresent*/) { mqttJustConnected = true; }

static void onMqttDisconnect(espMqttClientTypes::DisconnectReason /*reason*/) {
  mqttDisconnectedEvent = true;
}

static void onMqttMessage(const espMqttClientTypes::MessageProperties& props,
                          const char* topic, const uint8_t* payload,
                          size_t len, size_t index, size_t total) {
  // Retained messages replay on every (re)connect — a retained command
  // would re-trigger after every broker restart or WiFi blip, forever.
  // Command topics are live-only by convention: drop them (v1 rule).
  if (props.retain) return;

  if (index == 0) {
    strncpy(rxTopic, topic, sizeof(rxTopic) - 1);
    rxTopic[sizeof(rxTopic) - 1] = '\0';
  }
  for (size_t i = 0; i < len; i++) {
    size_t pos = index + i;
    if (pos >= sizeof(rxPayload) - 1) break;
    rxPayload[pos] = (char)payload[i];
  }
  if (index + len >= total) {
    size_t end =
        total > sizeof(rxPayload) - 1 ? sizeof(rxPayload) - 1 : total;
    rxPayload[end] = '\0';
    MqttInboxMessage msg;
    strncpy(msg.topic, rxTopic, sizeof(msg.topic) - 1);
    strncpy(msg.payload, rxPayload, sizeof(msg.payload) - 1);
    if (!mqttInboxPost(msg)) {
      SerialPrintln(F("MQTT: inbox full — inbound command dropped"));
    }
  }
}

void mqttServiceInit(MasterSettings& settings, SettingsStore& store,
                     const String& effectiveDeviceName, uint32_t bootCount) {
  (void)store;
  mqttBootCount = bootCount;
  if (settings.mqttHost.length() == 0) {
    SerialPrintln(
        F("MQTT: no broker configured (web UI, General card) — MQTT disabled"));
    return;
  }
  mqttActiveHost = settings.mqttHost;
  mqttActiveUser = settings.mqttUser;
  mqttActivePassword = settings.mqttPassword;
  int port = settings.mqttPort;
  if (port < 1 || port > 65535) port = SETTINGS_DEFAULT_MQTT_PORT;

  // MQTT identity follows the per-device network identity (#125) — chip-id
  // default or the stored pretty name. No separate mqttDeviceId knob.
  mqttResolvedDeviceId = effectiveDeviceName;
  mqttFwVersion = GIT_REV;
  mqttCmdTopics = makeMqttCommandTopics(mqttResolvedDeviceId);
  mqttTopicAvailability = mqttTopic(mqttResolvedDeviceId, "availability");
  mqttTopicTelemetry = mqttTopic(mqttResolvedDeviceId, "telemetry");
  mqttTopicModeState = mqttTopic(mqttResolvedDeviceId, "mode");

  mqttClient.setServer(mqttActiveHost.c_str(), (uint16_t)port);
  if (mqttActiveUser.length() > 0) {
    mqttClient.setCredentials(mqttActiveUser.c_str(),
                              mqttActivePassword.c_str());
  }
  mqttClient.setClientId(mqttResolvedDeviceId.c_str());
  mqttClient.setKeepAlive(30);
  // Last Will: broker publishes retained "offline" if we vanish uncleanly.
  mqttClient.setWill(mqttTopicAvailability.c_str(), 1, true, "offline");
  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onMessage(onMqttMessage);

  mqttInitialised = true;
  mqttBackoffInit(mqttBackoff, millis());
  SerialPrintln("MQTT: initialised, device id: " + mqttResolvedDeviceId);
}

// Retained HA discovery configs, one per entity. Truncation guard (v1):
// snprintf returning >= bufLen means a cut-mid-string JSON/topic that would
// poison HA's retained config — skip and log instead.
static void publishMqttDiscovery() {
  char topicBuf[96];
  char payloadBuf[512];
  for (int entity = 0; entity < DISCOVERY_ENTITY_COUNT; entity++) {
    size_t tLen = buildDiscoveryTopic(topicBuf, sizeof(topicBuf), entity,
                                      mqttResolvedDeviceId.c_str());
    size_t pLen =
        buildDiscoveryPayload(payloadBuf, sizeof(payloadBuf), entity,
                              mqttResolvedDeviceId.c_str(),
                              mqttFwVersion.c_str());
    if (tLen == 0 || tLen >= sizeof(topicBuf) || pLen == 0 ||
        pLen >= sizeof(payloadBuf)) {
      SerialPrintf(
          "MQTT: discovery entity %d skipped — would truncate (device id too "
          "long?)\n",
          entity);
      continue;
    }
    mqttClient.publish(topicBuf, 0, true, payloadBuf);
  }
}

// Retained, static-ish diagnostics — once per connect (they change only on
// reboot/reconfig). Plain per-field payloads, no JSON escaping (v1 #132).
static void publishMqttDiagnostics() {
  mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "diag/ip").c_str(), 0,
                     true, WiFi.localIP().toString().c_str());
  mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "diag/ssid").c_str(), 0,
                     true, WiFi.SSID().c_str());
  mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "diag/reset").c_str(), 0,
                     true, webResetReasonString());
  char n[12];
  snprintf(n, sizeof(n), "%lu", (unsigned long)mqttBootCount);
  mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "diag/boots").c_str(), 0,
                     true, n);
  mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "diag/ota").c_str(), 0,
                     true, otaVerdictSnapshot().otaReverted ? "ON" : "OFF");
  mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "diag/tz").c_str(), 0,
                     true, webTimezoneSnapshot().c_str());
}

static void cancelNotificationLocal() {
  notificationCancel(mqttNotification);
  notifActiveAtomic.store(false);
}

// Device renamed (#125): while this run still IS the old identity, blank
// the old retained discovery configs (empty retained payload = "delete this
// entry" per the HA spec) and every retained state topic under the old id.
static void publishDiscoveryClear() {
  char topicBuf[96];
  for (int entity = 0; entity < DISCOVERY_ENTITY_COUNT; entity++) {
    size_t tLen = buildDiscoveryTopic(topicBuf, sizeof(topicBuf), entity,
                                      mqttResolvedDeviceId.c_str());
    if (tLen == 0 || tLen >= sizeof(topicBuf)) continue;
    mqttClient.publish(topicBuf, 0, true, "");
  }
  static const char* const stateSuffixes[] = {
      "mode",     "text/state", "notification", "width",     "units",
      "speed",    "alignment",  "units_faulty", "units/attrs",
      "diag/ip",  "diag/ssid",  "diag/reset",   "diag/boots",
      "diag/ota", "diag/tz"};
  for (unsigned i = 0; i < sizeof(stateSuffixes) / sizeof(stateSuffixes[0]);
       i++) {
    mqttClient.publish(mqttTopic(mqttResolvedDeviceId, stateSuffixes[i]).c_str(),
                       0, true, "");
  }
  SerialPrintln(
      F("MQTT: cleared retained discovery configs for old device id (rename)"));
}

void mqttServiceHandleInbox(const MqttInboxMessage& msg) {
  if (!mqttInitialised) return;
  String payload(msg.payload);
  switch (classifyMqttCommandTopic(mqttCmdTopics, msg.topic)) {
    case MqttCommand::Mode: {
      String requested = parseModeCommand(payload);
      if (requested.length() == 0) {
        SerialPrintln("MQTT: ignored invalid mode command: " + payload);
      } else if (webMqttApplyMode(requested)) {
        // Explicit mode change trumps a running notification (v1 #130); the
        // 1 Hz ticker re-shows the new mode's content by itself.
        cancelNotificationLocal();
        SerialPrintln("MQTT: mode set to " + requested);
      }
      break;
    }
    case MqttCommand::Speed: {
      int v = parseSpeedCommand(payload);
      if (v < 0) {
        SerialPrintln("MQTT: ignored invalid speed command: " + payload);
      } else if (webMqttApplySpeed(v)) {
        SerialPrintf("MQTT: flap speed set to %d\n", v);
      }
      break;
    }
    case MqttCommand::Alignment: {
      String a = parseAlignmentCommand(payload);
      if (a.length() == 0) {
        SerialPrintln("MQTT: ignored invalid alignment command: " + payload);
      } else if (webMqttApplyAlignment(a)) {
        SerialPrintln("MQTT: alignment set to " + a);
      }
      break;
    }
    case MqttCommand::Restart:
      // Defer to the netTask reboot dispatcher like every other trigger —
      // never restart inline (v1 #116: the response/LWT story needs the
      // socket torn down by the reboot, and the drain's grace period).
      if (parseRestartCommand(payload)) {
        SerialPrintln(
            F("MQTT: restart requested via HA button — reboot pending"));
        webRequestReboot();
      }
      break;
    case MqttCommand::Text: {
      String text;
      long dwellSeconds = MQTT_TEXT_DWELL_S;
      if (!parseMqttTextPayload(payload, text, dwellSeconds)) {
        // Malformed/plain payload → show it verbatim with the default dwell.
        text = payload;
        dwellSeconds = MQTT_TEXT_DWELL_S;
      }
      // Producer gate (#205): while a reflash job runs nothing display-
      // mutating queues. HA commands are live-only — drop, don't defer.
      if (reflashInProgress(displaySnapshotGet().reflash)) {
        SerialPrintln("MQTT: notification dropped (reflash running): " + text);
        break;
      }
      WebContentSnapshot content = webDisplayContentSnapshot();
      DisplayCommand cmd =
          makeShowTextCommand(text, content.alignment, content.flapSpeed);
      if (!displayEnqueue(cmd)) {
        SerialPrintln("MQTT: notification dropped (display queue full): " +
                      text);
        break;
      }
      SerialPrintf("MQTT: notification (dwell %ld s): %s\n", dwellSeconds,
                   text.c_str());
      notificationStart(mqttNotification, text, dwellSeconds, millis());
      notifActiveAtomic.store(true);
      break;
    }
    case MqttCommand::None:
    default:
      SerialPrintln("MQTT: message on unexpected topic dropped: " +
                    String(msg.topic));
      break;
  }
}

void mqttServiceTick() {
  if (!mqttInitialised) return;

  // Staged cross-task requests first — they must work even while frozen.
  if (cancelNotificationRequested.exchange(false)) cancelNotificationLocal();
  if (stopForOtaRequested.exchange(false) && !mqttStoppedForOta) {
    mqttStoppedForOta = true;
    mqttFrozenAtMs = millis();
    // Abrupt on purpose: no DISCONNECT packet → the broker fires the
    // retained "offline" LWT, so HA availability stays correct without a
    // publish that would only go out if the broker ACKed mid-upload.
    mqttClient.disconnect(true);
    connectedAtomic.store(false);
    SerialPrintln(F("MQTT: force-closed for master OTA upload"));
  }
  if (mqttStoppedForOta) {
    bool resume = resumeAfterOtaRequested.exchange(false);
    if (!resume &&
        (int32_t)(millis() - mqttFrozenAtMs) >
            (int32_t)MQTT_OTA_FREEZE_FAILSAFE_MS) {
      SerialPrintln(F("MQTT: OTA freeze failsafe — resuming"));
      resume = true;
    }
    if (!resume) {
      mqttClient.loop();  // lets the disconnect complete; no reconnects
      return;
    }
    mqttStoppedForOta = false;
    // The freeze's own force-close fired a disconnect event that was never
    // consumed while frozen — drain it (and a racing connect flag) so it
    // can't clobber the fresh immediate-attempt schedule (cpp-review MED).
    mqttDisconnectedEvent = false;
    mqttJustConnected = false;
    connectedAtomic.store(false);
    mqttBackoffInit(mqttBackoff, millis());
    SerialPrintln(F("MQTT: resuming after OTA upload ended without reboot"));
  }
  resumeAfterOtaRequested.store(false);

  // Pump the client: socket reads/writes + all callbacks fire in here.
  mqttClient.loop();

  // Notification dwell (v1 mqttNotificationTick): on expiry just release
  // the clockTask gate — the 1 Hz ticker re-shows the active mode's
  // content (clock time, or the retained input text) by itself.
  if (mqttNotification.active && !notificationTick(mqttNotification, millis())) {
    notifActiveAtomic.store(false);
    SerialPrintln(F("MQTT: notification expired — reverting"));
  }

  if (mqttDisconnectedEvent) {
    mqttDisconnectedEvent = false;
    connectedAtomic.store(false);
    mqttBackoffOnDisconnect(mqttBackoff, millis());
  }

  if (mqttBackoffShouldAttempt(mqttBackoff, millis(),
                               WiFi.status() == WL_CONNECTED,
                               mqttClient.connected())) {
    mqttBackoffAttemptStarted(mqttBackoff);
    SerialPrintln(F("MQTT: connecting to broker..."));
    if (!mqttClient.connect()) {
      // connect() failing here (CONNECT packet allocation) never reaches
      // the TCP layer, so no disconnect event will ever re-arm the
      // schedule — re-arm it ourselves or reconnects stop forever
      // (cpp-review HIGH).
      SerialPrintln(F("MQTT: connect attempt failed to start — rescheduled"));
      mqttBackoffOnDisconnect(mqttBackoff, millis());
    }
  }

  if (mqttJustConnected) {
    mqttJustConnected = false;
    connectedAtomic.store(true);
    mqttBackoffOnConnect(mqttBackoff);
    SerialPrintln(F("MQTT: connected"));
    mqttClient.subscribe(mqttCmdTopics.text.c_str(), 0);
    mqttClient.subscribe(mqttCmdTopics.mode.c_str(), 0);
    mqttClient.subscribe(mqttCmdTopics.speed.c_str(), 0);
    mqttClient.subscribe(mqttCmdTopics.alignment.c_str(), 0);
    mqttClient.subscribe(mqttCmdTopics.restart.c_str(), 0);
    mqttClient.publish(mqttTopicAvailability.c_str(), 1, true, "online");
    publishMqttDiscovery();
    publishMqttDiagnostics();
    mqttNextTelemetryMs = millis();  // first telemetry immediately
    // Force a fresh publish of all event-driven state on this connection.
    mqttLastPublishedMode = "";
    mqttLastPublishedText = "\x01";
    mqttLastPublishedNotif = -1;
    mqttLastPublishedWidth = -1;
    mqttLastPublishedUnits = -1;
    mqttLastPublishedSpeed = -1;
    mqttLastPublishedAlignment = "\x01";
  }

  if (!mqttClient.connected()) return;

  // Event-driven retained state (v1 #130/#132): publish only on change so
  // HA updates instantly. Cheap compares every pass. MUST run before the
  // rename clear below — the connect block resets the trackers, and running
  // after the clear would repopulate the just-blanked old-id topics.
  DisplaySnapshot snap = displaySnapshotGet();
  WebContentSnapshot content = webDisplayContentSnapshot();

  if (content.deviceMode != mqttLastPublishedMode &&
      (content.deviceMode == "text" || content.deviceMode == "clock")) {
    mqttClient.publish(mqttTopicModeState.c_str(), 0, true,
                       content.deviceMode.c_str());
    mqttLastPublishedMode = content.deviceMode;
  }
  String writtenText(snap.currentText);
  if (writtenText != mqttLastPublishedText) {
    mqttLastPublishedText = writtenText;
    mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "text/state").c_str(),
                       0, true, writtenText.c_str());
  }
  int notif = mqttNotification.active ? 1 : 0;
  if (notif != mqttLastPublishedNotif) {
    mqttLastPublishedNotif = notif;
    mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "notification").c_str(),
                       0, true, notif ? "ON" : "OFF");
  }
  if (snap.displayWidth != mqttLastPublishedWidth) {
    mqttLastPublishedWidth = snap.displayWidth;
    char n[12];
    snprintf(n, sizeof(n), "%d", (int)snap.displayWidth);
    mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "width").c_str(), 0,
                       true, n);
  }
  if (snap.detectedUnitCount != mqttLastPublishedUnits) {
    mqttLastPublishedUnits = snap.detectedUnitCount;
    char n[12];
    snprintf(n, sizeof(n), "%d", (int)snap.detectedUnitCount);
    mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "units").c_str(), 0,
                       true, n);
  }
  if (content.flapSpeed != mqttLastPublishedSpeed) {
    mqttLastPublishedSpeed = content.flapSpeed;
    char n[12];
    snprintf(n, sizeof(n), "%d", content.flapSpeed);
    mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "speed").c_str(), 0,
                       true, n);
  }
  if (content.alignment != mqttLastPublishedAlignment) {
    mqttLastPublishedAlignment = content.alignment;
    mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "alignment").c_str(), 0,
                       true, content.alignment.c_str());
  }

  if (discoveryClearRequested.exchange(false)) publishDiscoveryClear();

  // Periodic telemetry + per-unit health, 60 s (wraparound-safe). Deviation
  // from v1 (spec): no blocking bus poll from here — displayTask owns the
  // bus; faulty counts/attrs are probe-time facts from the snapshot.
  if ((int32_t)(millis() - mqttNextTelemetryMs) >= 0) {
    mqttNextTelemetryMs = millis() + MQTT_TELEMETRY_INTERVAL_S * 1000UL;
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t maxBlock = ESP.getMaxAllocHeap();
    int heapFragPct = (freeHeap > 0 && maxBlock <= freeHeap)
                          ? (int)(100 - (maxBlock * 100ULL) / freeHeap)
                          : 0;
    char buf[128];
    size_t tn = buildTelemetryPayload(
        buf, sizeof(buf), freeHeap, heapFragPct, WiFi.RSSI(),
        snap.lastShowWriteErrors, millis() / 1000UL,
        clockIsTimeSynced(time(nullptr)));
    if (tn > 0 && tn < sizeof(buf)) {
      mqttClient.publish(mqttTopicTelemetry.c_str(), 0, false, buf);
    }

    char fc[12];
    snprintf(fc, sizeof(fc), "%d", (int)snap.faultyUnitCount);
    mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "units_faulty").c_str(),
                       0, true, fc);
    // Static, not stack (~2 KB doesn't belong on mqttTask's stack) and not
    // heap (a forever-periodic alloc/free is churn the memory policy
    // forbids). mqttTask-only access, so no guard needed.
    static char health[UNIT_HEALTH_JSON_CAP];
    size_t hn = buildUnitHealthJson(health, UNIT_HEALTH_JSON_CAP, snap.units,
                                    snap.displayWidth, snap.faultyUnitCount,
                                    SFP_I2C_ADDRESS_BASE);
    if (hn > 0 && hn < UNIT_HEALTH_JSON_CAP) {
      mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "units/attrs").c_str(),
                         0, true, health);
    }
  }
}

bool mqttIsConnected() { return mqttInitialised && connectedAtomic.load(); }

bool mqttNotificationActive() { return notifActiveAtomic.load(); }

// Must NOT be gated on mqttInitialised: broker-less devices still clear the
// (never-set) overlay harmlessly, and the web drain calls this untangled
// from MQTT state (v1 cancelActiveNotification rule).
void mqttCancelNotification() { cancelNotificationRequested.store(true); }

void mqttStopForOta() {
  if (!mqttInitialised) return;
  stopForOtaRequested.store(true);
}

void mqttResumeAfterOta() {
  if (!mqttInitialised) return;
  resumeAfterOtaRequested.store(true);
}

void mqttRequestDiscoveryClear() {
  if (!mqttInitialised) return;
  discoveryClearRequested.store(true);
}
