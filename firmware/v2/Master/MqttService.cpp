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
#include "QuietPolicy.h"  // #227
#include "DisplayCommand.h"
#include "DisplayIpc.h"
#include "HelpersSerialHandling.h"
#include "MqttHelpers.h"
#include "WearPolicy.h"
#include "MqttLifecyclePolicy.h"
#include "OtaService.h"
#include "SplitFlapProtocol.h"
#include "UnitHealth.h"
#include "WallMqtt.h"
#include "WallShow.h"
#include "WallState.h"
#include "WallUpdatePolicy.h"  // wallUpdatePhaseName
#include "WebEndpoints.h"

#include <memory>

#define MQTT_TELEMETRY_INTERVAL_S 60
#define MQTT_MAX_TEXT_LEN 256

// Wall surfacing cadence: it copies the wall snapshot and every row's unit
// facts — too heavy for every ~10 ms pass, and Home Assistant needs nothing
// faster for supervision state.
#define MQTT_WALL_INTERVAL_MS 5000

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
// Web transient dwell request (#219), seconds; 0 = none. The requester
// pre-applies the 600 s default so 0 stays an unambiguous "no request".
static std::atomic<long> transientDwellRequested{0};
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

// Wall surfacing trackers. mqttLastWallConfigured is -1 on every fresh
// session: the first wall pass RECONCILES — publish the discovery config
// when the rows table has rows, blank the retained wall topics when not (the
// last row may have been released while the broker was unreachable, leaving
// a stale retained config only this sweep can remove; blanking never-set
// retained topics is a broker no-op). mqttWallCapacity caches the wall's
// total units between 5 s wall passes for the width publish below.
static int mqttLastWallConfigured = -1;
static int mqttLastWallState = -1;
// Quiet switch (#227, QuietPolicy.h): command topic, last published state
// (-1 = not yet this session).
static String mqttQuietCmdTopic;
static int mqttLastQuietState = -1;
static String mqttLastWallAttrs = "\x01";
static uint32_t mqttNextWallMs = 0;
static int mqttWallCapacity = 0;

// Inbound chunk assembly (callback context). MQTT delivers one message at a
// time per connection, so a single staging pair suffices; payloads past the
// buffer truncate (v1 rule — the display path truncates to width anyway).
static char rxTopic[sizeof(MqttInboxMessage::topic)];
static char rxPayload[sizeof(MqttInboxMessage::payload)];

static void onMqttConnect(bool /*sessionPresent*/) { mqttJustConnected = true; }

static std::atomic<uint32_t> mqttDropCounter{0};  // System tab (#245);
                                                  // mqttTask writes, sampler reads

uint32_t mqttDropCount() { return mqttDropCounter.load(); }

static void onMqttDisconnect(espMqttClientTypes::DisconnectReason /*reason*/) {
  mqttDisconnectedEvent = true;
  mqttDropCounter.fetch_add(1);
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
  mqttQuietCmdTopic = mqttTopic(mqttResolvedDeviceId, "quiet/set");
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
  // The wall sensor lives outside the shared enum — clear its config too
  // (blanking a never-set retained topic is harmless).
  size_t cLen = buildWallProblemDiscoveryTopic(
      topicBuf, sizeof(topicBuf), mqttResolvedDeviceId.c_str());
  if (cLen > 0 && cLen < sizeof(topicBuf)) {
    mqttClient.publish(topicBuf, 0, true, "");
  }
  // So does the quiet switch (#227), with its retained command.
  cLen = buildQuietDiscoveryTopic(topicBuf, sizeof(topicBuf),
                                  mqttResolvedDeviceId.c_str());
  if (cLen > 0 && cLen < sizeof(topicBuf)) {
    mqttClient.publish(topicBuf, 0, true, "");
  }
  mqttClient.publish(mqttQuietCmdTopic.c_str(), 0, true, "");
  static const char* const stateSuffixes[] = {
      "mode",     "text/state", "notification", "width",     "units",
      "speed",    "alignment",  "units_faulty", "units/attrs",
      "diag/ip",  "diag/ssid",  "diag/reset",   "diag/boots",
      "diag/ota", "diag/tz",    "wall_problem", "wall/attrs",
      "quiet"};
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
  // Quiet (#227): its own topic, outside the shared command table.
  if (mqttQuietCmdTopic == msg.topic) {
    bool want = false;
    if (!quietParseCommand(payload, want)) {
      SerialPrintln("MQTT: ignored invalid quiet command: " + payload);
    } else if (webMqttApplyQuiet(want)) {
      SerialPrintln(want ? F("quiet: ON (MQTT) — no flap commands")
                         : F("quiet: OFF (MQTT)"));
    }
    return;
  }
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
        webRequestReboot("restart command via MQTT/HA");
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
      if (reflashInProgress(displaySnapshotGet().reflash) || wallUnitUpdateRunning()) {
        SerialPrintln("MQTT: notification dropped (reflash running): " + text);
        break;
      }
      // Quiet (#227): dropped, not deferred — unless the sender forces it.
      if (quietBlocksContent(tasksQuiet(), quietTextForced(payload))) {
        SerialPrintln("MQTT: notification dropped (quiet): " + text);
        break;
      }
      // With row boards the notification deliberately stays on this master's
      // own row; the wall's own-row re-show restores its piece after the dwell.
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

// --- mqttServiceTick stages (#353): one static helper per mechanism the
// tick interleaves — OTA freeze, event-driven state, wall surfacing,
// periodic telemetry. All file-scope state; mqttTask-only execution.

// OTA freeze/resume (#116 semantics). Returns true while frozen — the
// caller skips the rest of the tick.
static bool mqttHandleOtaFreeze() {
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
    return true;
  }
  mqttStoppedForOta = false;
  // The freeze's own force-close fired a disconnect event that was never
  // consumed while frozen — drain it (and a racing connect flag) so it
  // can't clobber the fresh immediate-attempt schedule.
  mqttDisconnectedEvent = false;
  mqttJustConnected = false;
  connectedAtomic.store(false);
  mqttBackoffInit(mqttBackoff, millis());
  SerialPrintln(F("MQTT: resuming after OTA upload ended without reboot"));
}
resumeAfterOtaRequested.store(false);
  return false;
}

// Event-driven retained state (v1 #130/#132): publish only on change so HA
// updates instantly. Cheap compares every pass. MUST run before the rename
// clear — the connect block resets the trackers, and running after the
// clear would repopulate the just-blanked old-id topics.
static void mqttPublishEventDrivenState(const DisplaySnapshot& snap,
                                        const WebContentSnapshot& content,
                                        bool leading) {
if (content.deviceMode != mqttLastPublishedMode &&
    (content.deviceMode == "text" || content.deviceMode == "clock")) {
  mqttClient.publish(mqttTopicModeState.c_str(), 0, true,
                     content.deviceMode.c_str());
  mqttLastPublishedMode = content.deviceMode;
}
// With row boards, text/state carries the whole wall (published in
// the wall block below) — the own-row slice alone would be a lie.
// Shared-tracker contract: mqttLastPublishedText/Width track the TOPIC's
// last retained value, not a mode — this own-facts path and the wall
// path below share them ON PURPOSE (one topic, one dedup), and the
// transition branch resets both when ownership flips.
if (!leading) {
  String writtenText(snap.currentText);
  if (writtenText != mqttLastPublishedText) {
    mqttLastPublishedText = writtenText;
    mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "text/state").c_str(),
                       0, true, writtenText.c_str());
  }
}
int notif = mqttNotification.active ? 1 : 0;
if (notif != mqttLastPublishedNotif) {
  mqttLastPublishedNotif = notif;
  mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "notification").c_str(),
                     0, true, notif ? "ON" : "OFF");
}
// Text capacity = the whole wall's units. Capacity is the 5 s wall pass's
// cached fact, so the first publish after pairing may briefly show the own
// width — retained, self-corrects next pass.
int widthValue = (leading && mqttWallCapacity > 0) ? mqttWallCapacity
                                                   : snap.displayWidth;
if (widthValue != mqttLastPublishedWidth) {
  mqttLastPublishedWidth = widthValue;
  char n[12];
  snprintf(n, sizeof(n), "%d", widthValue);
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
}

// Quiet switch (#227): the discovery config once per session, the state
// retained on every change — Home Assistant reads back what the board did.
static void mqttPublishQuiet() {
  int state = tasksQuiet() ? 1 : 0;
  if (state == mqttLastQuietState) return;
  if (mqttLastQuietState < 0) {
    char topicBuf[96];
    char payloadBuf[512];
    size_t tLen = buildQuietDiscoveryTopic(topicBuf, sizeof(topicBuf),
                                           mqttResolvedDeviceId.c_str());
    size_t pLen = buildQuietDiscovery(payloadBuf, sizeof(payloadBuf),
                                      mqttResolvedDeviceId.c_str(),
                                      mqttFwVersion.c_str());
    if (tLen > 0 && tLen < sizeof(topicBuf) && pLen > 0 &&
        pLen < sizeof(payloadBuf)) {
      mqttClient.publish(topicBuf, 0, true, payloadBuf);
    }
  }
  // A change made anywhere but on the command topic (the web toggle, a
  // takeover) is written back to it, retained: the broker redelivers the
  // retained command on every reconnect, and a stale one would undo the
  // change. Not on the first publish of a session — the retained command HA
  // sent while this board was offline has not arrived yet and must win.
  if (mqttLastQuietState >= 0) {
    mqttClient.publish(mqttQuietCmdTopic.c_str(), 0, true,
                       state ? "ON" : "OFF");
  }
  mqttLastQuietState = state;
  mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "quiet").c_str(), 0, true,
                     state ? "ON" : "OFF");
}

// Entities of the firmware before the wall link: blanked once per session.
static bool mqttRetiredCleared = false;

static void mqttClearRetiredEntities() {
  char topicBuf[96];
  for (int i = 0;; i++) {
    size_t n = buildRetiredDiscoveryTopic(topicBuf, sizeof(topicBuf),
                                          mqttResolvedDeviceId.c_str(), i);
    if (n == 0) break;
    if (n < sizeof(topicBuf)) mqttClient.publish(topicBuf, 0, true, "");
  }
  static const char* const retiredState[] = {"cluster_degraded", "cluster/attrs",
                                             "leader_lost"};
  for (const char* suffix : retiredState) {
    mqttClient.publish(mqttTopic(mqttResolvedDeviceId, suffix).c_str(), 0, true, "");
  }
}

// Wall surfacing: the problem sensor, the boards as its attributes and the
// wall's text/state, 5 s cadence (MQTT_WALL_INTERVAL_MS; cadence check lives
// inside). `active` = the rows table has rows.
static void mqttPublishWallSurfacing(const DisplaySnapshot& snap, bool active) {
  if ((int32_t)(millis() - mqttNextWallMs) < 0) return;
  mqttNextWallMs = millis() + MQTT_WALL_INTERVAL_MS;
  if (!mqttRetiredCleared) {
    mqttRetiredCleared = true;
    mqttClearRetiredEntities();
  }
  int activeNow = active ? 1 : 0;
  if (activeNow != mqttLastWallConfigured) {
    char topicBuf[96];
    size_t tLen = buildWallProblemDiscoveryTopic(topicBuf, sizeof(topicBuf),
                                                 mqttResolvedDeviceId.c_str());
    if (tLen > 0 && tLen < sizeof(topicBuf)) {
      if (activeNow) {
        char payloadBuf[512];
        size_t pLen = buildWallProblemDiscovery(payloadBuf, sizeof(payloadBuf),
                                                mqttResolvedDeviceId.c_str(),
                                                mqttFwVersion.c_str());
        if (pLen > 0 && pLen < sizeof(payloadBuf)) {
          mqttClient.publish(topicBuf, 0, true, payloadBuf);
        } else {
          SerialPrintln(F("MQTT: wall discovery skipped — would truncate"));
        }
      } else {
        // No rows (the last one released, OR the reconcile sweep of a fresh
        // session): blank the retained config + topics so Home Assistant
        // drops the entity instead of showing it stale, and force the
        // width/text publishes back onto this board's own facts.
        mqttClient.publish(topicBuf, 0, true, "");
        mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "wall_problem").c_str(), 0, true,
                           "");
        mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "wall/attrs").c_str(), 0, true, "");
        mqttWallCapacity = 0;
        mqttLastPublishedWidth = -1;
        mqttLastPublishedText = "\x01";
      }
    }
    mqttLastWallState = -1;
    mqttLastWallAttrs = "\x01";
    mqttLastWallConfigured = activeNow;
  }
  if (!active) return;

  // Heap, not this task's stack: the snapshot and one row's unit facts.
  std::unique_ptr<WallSnapshot> wall(new WallSnapshot(wallStateGet()));
  std::unique_ptr<UnitFactsDoc> units(new UnitFactsDoc);
  WallMqttRow rows[CLUSTER_MAX_MEMBERS];
  char texts[CLUSTER_MAX_MEMBERS][WALL_ROW_TEXT_MAX + 1];
  const uint32_t nowMs = millis();
  const int count = wall->rows.count;
  for (int i = 0; i < count; i++) {
    const WallRowDef& def = wall->rows.rows[i];
    const WallRowLink& link = wall->link[i];
    WallMqttRow& r = rows[i];
    r.id = def.id;
    r.row = def.row;
    r.col = def.col;
    r.width = def.width;
    r.own = wallRowIsOwn(def);
    texts[i][0] = '\0';
    wallShowRowText(i, texts[i], sizeof(texts[i]));
    r.text = texts[i];
    if (r.own) {
      r.text = snap.currentText;  // what is on the flaps, as for a master on its own
      r.unitsKnown = true;
      r.unitsFound = snap.detectedUnitCount;
      r.unitsFaulty = snap.faultyUnitCount;
      r.unitsLost = (uint8_t)computeLostUnitCount(snap.units, snap.displayWidth);
      continue;
    }
    const WallRowReach reach = wallRowReach(link.contact, nowMs);
    r.reach = wallRowReachName(reach);
    // A row never heard since this master started is gone once the time a
    // connected one would have been written off has passed.
    r.lost = reach == WallRowReach::Lost ||
             (reach == WallRowReach::Never && nowMs >= WALL_LINK_LOST_MS);
    r.welcomed = link.everWelcomed;
    r.rev = link.rev;
    r.rescue = link.rescue;
    r.updateBlocked = link.updateBlocked;
    r.busDead = link.haveStatus && link.status.bus_dead;
    uint32_t atMs = 0;
    if (wallStateRowUnits(i, *units, atMs)) {
      r.unitsKnown = true;
      int found = 0;
      for (int u = 0; u < units->width && u < UNITS_AMOUNT; u++) {
        if (units->units[u].state != 0) found++;
      }
      r.unitsFound = (uint8_t)found;
      r.unitsFaulty = (uint8_t)units->faulty;
      r.unitsLost = (uint8_t)computeLostUnitCount(units->units, units->width);
    }
  }
  mqttWallCapacity = wallMqttCapacity(rows, count);
  int problem = wallMqttProblem(rows, count) ? 1 : 0;
  if (problem != mqttLastWallState) {
    mqttLastWallState = problem;
    mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "wall_problem").c_str(), 0, true,
                       problem ? "ON" : "OFF");
  }
  String attrs = wallMqttAttrsJson(
      rows, count, wallUpdatePhaseName((WallUpdatePhase)wall->updatePhase));
  if (attrs != mqttLastWallAttrs) {
    mqttLastWallAttrs = attrs;
    mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "wall/attrs").c_str(), 0, true,
                       attrs.c_str());
  }
  // Wall text/state — shares mqttLastPublishedText with the own-facts path
  // (same topic; see the contract note above the leading gate).
  String text = wallMqttStateText(rows, count);
  if (text != mqttLastPublishedText) {
    mqttLastPublishedText = text;
    mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "text/state").c_str(), 0, true,
                       text.c_str());
  }
}

// Periodic telemetry + per-unit health, 60 s (wraparound-safe; cadence
// check lives inside). Deviation from v1 (spec): no blocking bus poll from
// here — displayTask owns the bus; faulty counts/attrs are probe-time
// facts from the snapshot.
static void mqttPublishTelemetry(const DisplaySnapshot& snap) {
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
    char buf[160];
    size_t tn = buildTelemetryPayload(
        buf, sizeof(buf), freeHeap, heapFragPct, WiFi.RSSI(),
        snap.lastShowWriteErrors, millis() / 1000UL,
        clockIsTimeSynced(time(nullptr)),
        unitFleetVccMin(snap.units, snap.displayWidth),      // #366 HA vccMin sensor
        unitFleetRebootTotal(snap.units, snap.displayWidth),  // #368 HA reboot-total sensor
        unitFleetAnyJam(snap.units, snap.displayWidth),      // #365 HA jam binary_sensor
        unitFleetMaxExcess(snap.units, snap.displayWidth));  // #365 HA steps-excess sensor
    if (tn > 0 && tn < sizeof(buf)) {
      mqttClient.publish(mqttTopicTelemetry.c_str(), 0, false, buf);
    }

    char fc[12];
    snprintf(fc, sizeof(fc), "%d", (int)snap.faultyUnitCount);
    mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "units_faulty").c_str(),
                       0, true, fc);
    // Wear warning (#231): binary problem state + median/flagged attrs, both
    // computed from the same snapshot the health payload uses.
    WearAssessment wear;
    assessWear(snap.units, snap.displayWidth, wear);
    mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "units_wear").c_str(),
                       0, true, wear.flaggedCount > 0 ? "ON" : "OFF");
    char wearAttrs[100];
    wearAttrs[0] = '{';
    size_t wn = buildWearJson(wear, wearAttrs + 1, sizeof(wearAttrs) - 2);
    if (wn > 0 && wn < sizeof(wearAttrs) - 2) {
      wearAttrs[wn + 1] = '}';
      wearAttrs[wn + 2] = '\0';
      mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "units/wear").c_str(),
                         0, true, wearAttrs);
    }
    // Static, not stack (~2 KB doesn't belong on mqttTask's stack) and not
    // heap (a forever-periodic alloc/free is churn the memory policy
    // forbids). mqttTask-only access, so no guard needed.
    static char health[UNIT_HEALTH_JSON_CAP];
    size_t hn = buildUnitHealthJson(health, UNIT_HEALTH_JSON_CAP, snap.units,
                                    snap.displayWidth, snap.faultyUnitCount,
                                    SFP_I2C_ADDRESS_BASE, millis());
    if (hn > 0 && hn < UNIT_HEALTH_JSON_CAP) {
      mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "units/attrs").c_str(),
                         0, true, health);
    }
}
}

void mqttServiceTick() {
  // Overlay lifecycle runs BEFORE the initialised gate: web transients ride
  // this overlay on broker-less devices too (#219, v1 rule — calibration
  // transients exist without a broker). Cancel drains before start; expiry
  // just releases the clockTask gate — the 1 Hz ticker re-shows the active
  // mode's content by itself. The overlay is ONE slot with two producers
  // (this staged web path and the inbox's direct MQTT-text arm): within a
  // ~10 ms tick window the later writer silently replaces the earlier
  // dwell/text — v1's single-notification-slot last-write-wins, with the
  // tick boundary deciding "later" — an accepted race.
  if (cancelNotificationRequested.exchange(false)) cancelNotificationLocal();
  long transientDwell = transientDwellRequested.exchange(0);
  if (transientDwell > 0) {
    transientTextStart(mqttNotification, String(), transientDwell, millis());
    notifActiveAtomic.store(true);
  }
  if (mqttNotification.active &&
      !notificationTick(mqttNotification, millis())) {
    notifActiveAtomic.store(false);
    SerialPrintln(F("Notification/transient expired — reverting"));
  }

  if (!mqttInitialised) return;
  if (mqttHandleOtaFreeze()) return;

  // Pump the client: socket reads/writes + all callbacks fire in here.
  mqttClient.loop();

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
      // schedule — re-arm it ourselves or reconnects stop forever.
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
    mqttClient.subscribe(mqttQuietCmdTopic.c_str(), 0);  // #227
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
    // A fresh session reconciles the wall sensor's retained config and
    // re-publishes state/attrs on the first wall pass.
    mqttLastWallConfigured = -1;
    mqttLastWallState = -1;
    mqttLastWallAttrs = "\x01";
    mqttNextWallMs = millis();
    mqttRetiredCleared = false;
    mqttLastQuietState = -1;
  }

  if (!mqttClient.connected()) return;

  DisplaySnapshot snap = displaySnapshotGet();
  WebContentSnapshot content = webDisplayContentSnapshot();
  bool leading = wallShowActive();

  mqttPublishEventDrivenState(snap, content, leading);
  mqttPublishWallSurfacing(snap, leading);
  mqttPublishQuiet();

  if (discoveryClearRequested.exchange(false)) publishDiscoveryClear();

  mqttPublishTelemetry(snap);
}

bool mqttIsConnected() { return mqttInitialised && connectedAtomic.load(); }

bool mqttNotificationActive() { return notifActiveAtomic.load(); }

// Must NOT be gated on mqttInitialised: broker-less devices still clear the
// (never-set) overlay harmlessly, and the web drain calls this untangled
// from MQTT state (v1 cancelActiveNotification rule).
void mqttCancelNotification() { cancelNotificationRequested.store(true); }

// See MqttService.h (#219). The default is pre-applied here because 0 is
// the staging atomic's "no request" sentinel.
void mqttStartNotificationDwell(long dwellSeconds) {
  transientDwellRequested.store(
      dwellSeconds > 0 ? dwellSeconds : TRANSIENT_TEXT_DEFAULT_DWELL_SECONDS);
  // Gate flips from the caller's task, not the tick: the 1 Hz clock ticker
  // must not sneak a mode re-show in behind the just-queued transient
  // during the <=10 ms until the tick stamps the deadline.
  notifActiveAtomic.store(true);
}

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
