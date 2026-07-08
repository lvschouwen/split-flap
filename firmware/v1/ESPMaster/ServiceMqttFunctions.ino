//MQTT / Home Assistant integration (issue #121). Design doc:
//docs/superpowers/specs/2026-07-05-mqtt-ha-integration-design.md
//
//Always compiled in since #57: broker config lives in EEPROM (web UI,
//applied on reboot) and an empty host leaves everything inert — initMqtt()
//returns before any client state is touched.
//
//Threading model (non-negotiable, see design doc): AsyncMqttClient
//callbacks run in LWIP/sys context. They ONLY copy bytes and set flags.
//Everything that touches the display, I2C, Strings-at-scale, or publishes
//discovery happens in loopMqtt() on the main loop.

#include <AsyncMqttClient.h>
#include "MqttHelpers.h"

static AsyncMqttClient mqttClient;

//Lifecycle state — all millis()-based, no tickers.
static bool mqttInitialised = false;
static bool mqttStoppedForOta = false;
static volatile bool mqttDisconnectedEvent = false;  //set in LWIP ctx, consumed in loopMqtt
static bool mqttReconnectPending = false;
static unsigned long mqttReconnectAtMs = 0;          //valid only while mqttReconnectPending
static unsigned long mqttReconnectBackoffMs = 2000; //doubles per failure, 30 s cap
static unsigned long mqttNextTelemetryMs = 0;

//LWIP-context -> main-loop hand-off flags.
static volatile bool mqttJustConnected = false;
static volatile bool mqttMessagePending = false;
static char mqttRxBuffer[MQTT_MAX_TEXT_LEN + 1];

//Mode command hand-off (#130): same copy-and-flag pattern as the text
//topic. 16 bytes fits "text"/"clock" with room for junk we'll reject.
static volatile bool mqttModeCommandPending = false;
static char mqttModeRxBuffer[16];

//Control command hand-off (#132 Stage B): same copy-and-flag pattern. Speed
//holds up to "100"; alignment holds "center"; restart holds "PRESS".
static volatile bool mqttSpeedCommandPending = false;
static char mqttSpeedRxBuffer[8];
static volatile bool mqttAlignmentCommandPending = false;
static char mqttAlignmentRxBuffer[16];
static volatile bool mqttRestartCommandPending = false;
static char mqttRestartRxBuffer[16];

//Last mode published to the retained state topic; "" forces a publish on
//the first connected loop pass (and after each reconnect reset below).
static String mqttLastPublishedMode;

//Event-driven state trackers (#132): publish a retained topic only when the
//value changes, so HA sees current text / notification / width / units
//instantly and the broker keeps the last value. Reset on each (re)connect to
//force a full re-publish. Sentinels are values no real state can take.
static String mqttLastPublishedText = "\x01";
static int mqttLastPublishedNotif = -1;
static int mqttLastPublishedWidth = -1;
static int mqttLastPublishedUnits = -1;
static String mqttLastPublishedSpeed = "\x01";      //flap speed (#132 Stage B)
static String mqttLastPublishedAlignment = "\x01";  //alignment (#132 Stage B)

//Show-then-revert notification state (MqttHelpers.h).
static MqttNotification mqttNotification;

//Device-rename hand-off (#125). The settings POST handler runs in async
//context, so it only sets this flag; loopMqtt() does the actual publishes.
static volatile bool mqttDiscoveryClearPending = false;

//Topics + broker identity, resolved once in initMqtt(). AsyncMqttClient
//stores POINTERS for server host / client id / credentials / will — these
//Strings must outlive the client and never be reassigned after init. The
//EEPROM-backed mqtt*Setting globals CAN be reassigned mid-run by the web
//POST handler (persist-now, apply-on-reboot), which is exactly why the
//client gets its own stable copies here (#57).
static String mqttResolvedDeviceId;
static String mqttTopicSet;
static String mqttTopicModeSet;
static String mqttTopicModeState;
static String mqttTopicSpeedSet;      //#132 Stage B
static String mqttTopicAlignmentSet;  //#132 Stage B
static String mqttTopicRestartSet;    //#132 Stage B
static String mqttTopicAvailability;
static String mqttTopicTelemetry;
static String mqttActiveHost;
static String mqttActiveUser;
static String mqttActivePassword;

//LWIP context: flag only. Subscribe/discovery/availability happen in
//loopMqtt() so the 512-byte discovery buffers never live on the sys stack.
static void onMqttConnect(bool sessionPresent) {
  (void)sessionPresent;
  mqttJustConnected = true;
}

//LWIP context: flag only. Backoff arithmetic and scheduling happen in
//loopMqtt() — callbacks never compute or connect.
static void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  (void)reason;
  mqttDisconnectedEvent = true;
}

//LWIP context: copy one MQTT chunk into a fixed buffer (truncating past its
//capacity) and raise `flag` when the final chunk lands. Heap-free byte copy
//only — safe for the sys-context callback. Shared by the #132 control topics.
static void mqttCopyChunk(char* buf, size_t bufSize, const char* payload,
                          size_t len, size_t index, size_t total, volatile bool* flag) {
  for (size_t i = 0; i < len; i++) {
    size_t pos = index + i;
    if (pos >= bufSize - 1) break;
    buf[pos] = payload[i];
  }
  if (index + len >= total) {
    size_t end = total > bufSize - 1 ? bufSize - 1 : total;
    buf[end] = '\0';
    *flag = true;
  }
}

//LWIP context: copy the chunk into the right fixed buffer (truncating past
//its capacity — the display path truncates to the display width anyway)
//and flag the loop when the final chunk lands. Several command topics are
//subscribed (#130, #132), so dispatch on the topic with a heap-free strcmp.
static void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties,
                          size_t len, size_t index, size_t total) {
  //Retained messages replay on every (re)connect — a retained command
  //would re-trigger after every broker restart or WiFi blip, forever.
  //Command topics are live-only by convention: drop them.
  if (properties.retain) {
    return;
  }

  //Mode select command (#130): tiny payload, own buffer + flag.
  if (strcmp(topic, mqttTopicModeSet.c_str()) == 0) {
    for (size_t i = 0; i < len; i++) {
      size_t pos = index + i;
      if (pos >= sizeof(mqttModeRxBuffer) - 1) break;
      mqttModeRxBuffer[pos] = payload[i];
    }
    if (index + len >= total) {
      size_t end = total > sizeof(mqttModeRxBuffer) - 1 ? sizeof(mqttModeRxBuffer) - 1 : total;
      mqttModeRxBuffer[end] = '\0';
      mqttModeCommandPending = true;
    }
    return;
  }

  //Flap-speed number command (#132).
  if (strcmp(topic, mqttTopicSpeedSet.c_str()) == 0) {
    mqttCopyChunk(mqttSpeedRxBuffer, sizeof(mqttSpeedRxBuffer), payload, len, index, total, &mqttSpeedCommandPending);
    return;
  }
  //Alignment select command (#132).
  if (strcmp(topic, mqttTopicAlignmentSet.c_str()) == 0) {
    mqttCopyChunk(mqttAlignmentRxBuffer, sizeof(mqttAlignmentRxBuffer), payload, len, index, total, &mqttAlignmentCommandPending);
    return;
  }
  //Restart button command (#132).
  if (strcmp(topic, mqttTopicRestartSet.c_str()) == 0) {
    mqttCopyChunk(mqttRestartRxBuffer, sizeof(mqttRestartRxBuffer), payload, len, index, total, &mqttRestartCommandPending);
    return;
  }

  //Text notification command. Explicit match so a future third
  //subscription (or an unexpected broker delivery) fails closed instead
  //of silently landing in the notification path.
  if (strcmp(topic, mqttTopicSet.c_str()) == 0) {
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
}

//Publishes the retained HA discovery configs (one per MqttDiscoveryEntity).
//Main loop only — the
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

//Retained, static-ish diagnostics (#132) — published once per connect (they
//change only on reboot/reconfig). Plain per-field payloads: no JSON escaping,
//and each HA sensor's stat_t points straight at its own topic.
static void publishMqttDiagnostics() {
  mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "diag/ip").c_str(),    0, true, WiFi.localIP().toString().c_str());
  mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "diag/ssid").c_str(),  0, true, WiFi.SSID().c_str());
  mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "diag/reset").c_str(), 0, true, lastResetReason.c_str());
  char n[12];
  snprintf(n, sizeof(n), "%lu", (unsigned long)readBootStateRtc().bootCounter);
  mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "diag/boots").c_str(), 0, true, n);
  mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "diag/ota").c_str(),   0, true, otaReverted ? "ON" : "OFF");
  String tz = timezonePosixSetting.length() ? timezonePosixSetting : String(timezonePosix);
  mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "diag/tz").c_str(),    0, true, tz.c_str());
}

//Called once from setup(), normal boots only (never quiet-OTA or recovery —
//setup() returns before reaching the call in those modes).
void initMqtt() {
  if (mqttHostSetting.length() == 0) {
    SerialPrintln(F("MQTT: no broker configured (web UI, General card) — MQTT disabled"));
    return;
  }
  //Stable copies for the client (see comment at the declarations).
  mqttActiveHost     = mqttHostSetting;
  mqttActiveUser     = mqttUserSetting;
  mqttActivePassword = mqttPasswordSetting;
  //The POST handler validated the port (empty or 1..65535); toInt() on an
  //empty string is 0 -> default 1883.
  long portValue = mqttPortSetting.toInt();
  if (portValue < 1 || portValue > 65535) portValue = 1883;

  //MQTT identity follows the per-device network identity (#125) — chip-id
  //default or the EEPROM pretty name. No separate mqttDeviceId knob.
  mqttResolvedDeviceId  = effectiveDeviceName;
  mqttTopicSet          = mqttTopic(mqttResolvedDeviceId, "text/set");
  mqttTopicModeSet      = mqttTopic(mqttResolvedDeviceId, "mode/set");
  mqttTopicModeState    = mqttTopic(mqttResolvedDeviceId, "mode");
  mqttTopicSpeedSet     = mqttTopic(mqttResolvedDeviceId, "speed/set");
  mqttTopicAlignmentSet = mqttTopic(mqttResolvedDeviceId, "alignment/set");
  mqttTopicRestartSet   = mqttTopic(mqttResolvedDeviceId, "restart/set");
  mqttTopicAvailability = mqttTopic(mqttResolvedDeviceId, "availability");
  mqttTopicTelemetry    = mqttTopic(mqttResolvedDeviceId, "telemetry");

  mqttClient.setServer(mqttActiveHost.c_str(), (uint16_t)portValue);
  if (mqttActiveUser.length() > 0) {
    mqttClient.setCredentials(mqttActiveUser.c_str(), mqttActivePassword.c_str());
  }
  mqttClient.setClientId(mqttResolvedDeviceId.c_str());
  mqttClient.setKeepAlive(30);
  //Last Will: broker publishes retained "offline" if we vanish uncleanly.
  mqttClient.setWill(mqttTopicAvailability.c_str(), 1, true, "offline");
  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onMessage(onMqttMessage);

  mqttInitialised = true;
  mqttReconnectPending = true;
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

  //Disconnect event → schedule the next attempt with exponential backoff
  //(2 s -> 30 s cap). Computed here, not in the callback (LWIP ctx).
  if (mqttDisconnectedEvent) {
    mqttDisconnectedEvent = false;
    mqttReconnectPending = true;
    mqttReconnectAtMs = millis() + mqttReconnectBackoffMs;
    mqttReconnectBackoffMs *= 2;
    if (mqttReconnectBackoffMs > 30000UL) mqttReconnectBackoffMs = 30000UL;
  }

  //Event-driven reconnect, only while WiFi is up. millis() comparison is
  //wraparound-safe.
  if (mqttReconnectPending && !mqttClient.connected() &&
      (long)(millis() - mqttReconnectAtMs) >= 0 &&
      WiFi.status() == WL_CONNECTED) {
    mqttReconnectPending = false;
    SerialPrintln(F("MQTT: connecting to broker..."));
    mqttClient.connect();
  }

  if (mqttJustConnected) {
    mqttJustConnected = false;
    mqttReconnectBackoffMs = 2000;  //healthy connection resets the backoff
    SerialPrintln(F("MQTT: connected"));
    mqttClient.subscribe(mqttTopicSet.c_str(), 0);
    mqttClient.subscribe(mqttTopicModeSet.c_str(), 0);
    mqttClient.subscribe(mqttTopicSpeedSet.c_str(), 0);       //#132 Stage B
    mqttClient.subscribe(mqttTopicAlignmentSet.c_str(), 0);   //#132 Stage B
    mqttClient.subscribe(mqttTopicRestartSet.c_str(), 0);     //#132 Stage B
    mqttClient.publish(mqttTopicAvailability.c_str(), 1, true, "online");
    publishMqttDiscovery();
    publishMqttDiagnostics();        //retained diagnostics, once per connect (#132)
    mqttNextTelemetryMs = millis();  //first telemetry immediately
    mqttLastPublishedMode = "";      //re-assert the retained mode state below
    //Force a fresh publish of all event-driven state on this connection (#132).
    mqttLastPublishedText = "\x01";
    mqttLastPublishedNotif = -1;
    mqttLastPublishedWidth = -1;
    mqttLastPublishedUnits = -1;
    mqttLastPublishedSpeed = "\x01";
    mqttLastPublishedAlignment = "\x01";
  }

  //Retained mode state (#130): publish whenever reality differs from what
  //HA last heard — covers boot, reconnect, HA commands and web-UI changes
  //without coupling to any of those paths. MUST run before the rename
  //clear below (like publishMqttDiscovery above): the connect block resets
  //the tracker, and if this ran after the clear it would immediately
  //repopulate the just-blanked old state topic in the same pass.
  if (mqttClient.connected() && deviceMode != mqttLastPublishedMode &&
      (deviceMode == DEVICE_MODE_TEXT || deviceMode == DEVICE_MODE_CLOCK)) {
    mqttClient.publish(mqttTopicModeState.c_str(), 0, true, deviceMode.c_str());
    mqttLastPublishedMode = deviceMode;
  }

  //Event-driven state (#132): publish retained topics only on change so HA
  //updates instantly. Cheap compares every pass; a publish only on transition.
  //MUST run before the rename-clear below, same as the mode state above: the
  //connect block resets these trackers, so running after the clear would
  //repopulate the just-blanked old-identity topics in the same pass.
  if (mqttClient.connected()) {
    if (lastWrittenText != mqttLastPublishedText) {
      mqttLastPublishedText = lastWrittenText;
      mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "text/state").c_str(), 0, true, lastWrittenText.c_str());
    }
    int notif = mqttNotification.active ? 1 : 0;
    if (notif != mqttLastPublishedNotif) {
      mqttLastPublishedNotif = notif;
      mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "notification").c_str(), 0, true, notif ? "ON" : "OFF");
    }
    if (displayWidth != mqttLastPublishedWidth) {
      mqttLastPublishedWidth = displayWidth;
      char n[12]; snprintf(n, sizeof(n), "%d", displayWidth);
      mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "width").c_str(), 0, true, n);
    }
    int units = countRespondingUnits(detectedUnitStates, UNITS_AMOUNT);
    if (units != mqttLastPublishedUnits) {
      mqttLastPublishedUnits = units;
      char n[12]; snprintf(n, sizeof(n), "%d", units);
      mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "units").c_str(), 0, true, n);
    }
    //Control states (#132 Stage B) — mirror HA-set AND web-UI-set changes.
    if (flapSpeed != mqttLastPublishedSpeed) {
      mqttLastPublishedSpeed = flapSpeed;
      mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "speed").c_str(), 0, true, flapSpeed.c_str());
    }
    if (alignment != mqttLastPublishedAlignment) {
      mqttLastPublishedAlignment = alignment;
      mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "alignment").c_str(), 0, true, alignment.c_str());
    }
  }

  //Device renamed (#125): while this run still IS the old MQTT identity,
  //blank the old retained HA discovery configs so the post-reboot identity
  //doesn't leave an orphaned device in Home Assistant. Empty retained
  //payload = "delete this discovery entry" per the HA MQTT discovery spec.
  if (mqttDiscoveryClearPending && mqttClient.connected()) {
    mqttDiscoveryClearPending = false;
    char topicBuf[96];
    for (int entity = 0; entity < DISCOVERY_ENTITY_COUNT; entity++) {
      size_t tLen = buildDiscoveryTopic(topicBuf, sizeof(topicBuf), entity, mqttResolvedDeviceId.c_str());
      if (tLen == 0 || tLen >= sizeof(topicBuf)) continue;
      mqttClient.publish(topicBuf, 0, true, "");
    }
    //Also blank every retained STATE topic (#130, #132) — without their
    //configs they'd be orphaned retained messages on the old id.
    static const char* const stateSuffixes[] = {
      "mode", "text/state", "notification", "width", "units", "speed", "alignment",
      "units_faulty", "units/attrs",
      "diag/ip", "diag/ssid", "diag/reset", "diag/boots", "diag/ota", "diag/tz"
    };
    for (unsigned i = 0; i < sizeof(stateSuffixes) / sizeof(stateSuffixes[0]); i++) {
      mqttClient.publish(mqttTopic(mqttResolvedDeviceId, stateSuffixes[i]).c_str(), 0, true, "");
    }
    SerialPrintln(F("MQTT: cleared retained discovery configs for old device id (rename)"));
  }

  //HA mode select command (#130).
  if (mqttModeCommandPending) {
    mqttModeCommandPending = false;
    String modePayload(mqttModeRxBuffer);
    if (mqttModeCommandPending) {
      //Torn copy (new command landed mid-read) — reprocess next pass.
      return;
    }
    String requestedMode = parseModeCommand(modePayload);
    if (requestedMode.length() == 0) {
      SerialPrintln("MQTT: ignored invalid mode command: " + modePayload);
    }
    else if (requestedMode != deviceMode) {
      deviceMode = requestedMode;
      saveDeviceMode();
      notificationCancel(mqttNotification);
      SerialPrintln("MQTT: mode set to " + deviceMode);
    }
  }

  //Flap-speed number command (#132 Stage B).
  if (mqttSpeedCommandPending) {
    mqttSpeedCommandPending = false;
    String p(mqttSpeedRxBuffer);
    if (mqttSpeedCommandPending) return;  //torn copy — reprocess next pass
    int v = parseSpeedCommand(p);
    if (v < 0) {
      SerialPrintln("MQTT: ignored invalid speed command: " + p);
    }
    else if (String(v) != flapSpeed) {
      flapSpeed = String(v);
      saveFlapSpeed();
      SerialPrintln("MQTT: flap speed set to " + flapSpeed);
    }
  }

  //Alignment select command (#132 Stage B).
  if (mqttAlignmentCommandPending) {
    mqttAlignmentCommandPending = false;
    String p(mqttAlignmentRxBuffer);
    if (mqttAlignmentCommandPending) return;  //torn copy — reprocess next pass
    String a = parseAlignmentCommand(p);
    if (a.length() == 0) {
      SerialPrintln("MQTT: ignored invalid alignment command: " + p);
    }
    else if (a != alignment) {
      alignment = a;
      saveAlignment();
      SerialPrintln("MQTT: alignment set to " + alignment);
    }
  }

  //Restart button command (#132 Stage B). Defer to loop()'s reboot dispatcher
  //(isPendingReboot) like every other reboot trigger — it parks the display
  //before ESP.restart() so a flap isn't left mid-move/energized during eboot
  //(#116). Never ESP.restart() inline here. On reboot the dropped TCP socket
  //fires our retained "offline" LWT; the device republishes online + full
  //state on the next connect.
  if (mqttRestartCommandPending) {
    mqttRestartCommandPending = false;
    String p(mqttRestartRxBuffer);
    if (mqttRestartCommandPending) return;  //torn copy — reprocess next pass
    if (parseRestartCommand(p)) {
      SerialPrintln(F("MQTT: restart requested via HA button — reboot pending"));
      isPendingReboot = true;
    }
  }

  if (mqttMessagePending) {
    mqttMessagePending = false;
    String payload(mqttRxBuffer);
    if (mqttMessagePending) {
      //A new message finished landing while we were copying — the copy may
      //interleave old and new bytes. Drop it; the fresh flag reprocesses
      //the complete new payload on the next pass.
      return;
    }
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
    char buf[128];
    bool ntpSynced = time(nullptr) > 1600000000L;
    size_t tn = buildTelemetryPayload(buf, sizeof(buf), ESP.getFreeHeap(), (int)ESP.getHeapFragmentation(),
                          WiFi.RSSI(), lastShowUnitWriteErrors, millis() / 1000UL, ntpSynced);
    //Reject a truncated payload (same discipline as the discovery builders).
    if (tn > 0 && tn < sizeof(buf)) {
      mqttClient.publish(mqttTopicTelemetry.c_str(), 0, false, buf);
    }

    //Per-unit health (#137): refresh the shared cache (also serves the #45 web
    //card) and publish the integer faulty count + the per-unit attrs. Retained
    //so HA has the last snapshot after a restart. Blocking I2C, but we're in
    //loop() on the telemetry tick — same context the web refresh drain uses.
    pollUnitHealth();
    char fc[12];
    snprintf(fc, sizeof(fc), "%d", faultyUnitCount);
    mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "units_faulty").c_str(), 0, true, fc);
    mqttClient.publish(mqttTopic(mqttResolvedDeviceId, "units/attrs").c_str(), 0, true, unitHealthJson);
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

//Calibration "Send to all" (#165): show a transient test pattern through the
//same show-then-revert state instead of persistently flipping the device to
//text mode. Loop() context only — applyPendingSettingsPost() calls it while
//draining a POST; mqttNotification is loop-owned state.
void showCalibrationText(const String& text) {
  calibrationTextStart(mqttNotification, text, millis());
}

//Upload started (#116 freeze): force-close the MQTT TCP session NOW so
//nothing MQTT-related is alive during flash writes. Deliberately NOT a
//graceful disconnect: an abrupt close without a DISCONNECT packet makes
//the broker fire our registered Last Will — the retained "offline" — so
//HA availability stays correct without us needing a publish that would
//only go out if the broker felt like ACKing it mid-upload. Runs in async
//context; disconnect(true) only tears down, which is safe there.
void mqttStopForOta() {
  if (!mqttInitialised || mqttStoppedForOta) {
    return;
  }
  mqttStoppedForOta = true;
  mqttClient.disconnect(true);
  SerialPrintln(F("MQTT: force-closed for master OTA upload"));
}

//Explicit mode switch or message send trumps a running notification /
//calibration pattern (#130/#165): cancel so the next 1 s tick re-flaps the
//new content. Loop() context only (callers sit in applyPendingSettingsPost's
//drain) — mqttNotification is loop-owned, so cancel directly. Must NOT be
//gated on mqttInitialised: calibration transients exist without a broker.
void cancelActiveNotification() {
  notificationCancel(mqttNotification);
}

//Called from the settings POST handler when the device name changed (#125).
//Async context: flag only — loopMqtt() publishes the empty retained configs
//on the next main-loop pass. If MQTT is down/disabled the flag is simply
//never consumed and the stale HA device needs manual cleanup (documented).
void mqttRequestDiscoveryClear() {
  if (!mqttInitialised) {
    return;
  }
  mqttDiscoveryClearPending = true;
}

//Upload failed or stalled (display thawed): resume MQTT. On the success
//path the device reboots instead, so this never runs there.
void mqttResumeAfterOta() {
  if (!mqttInitialised || !mqttStoppedForOta) {
    return;
  }
  mqttStoppedForOta = false;
  mqttReconnectBackoffMs = 2000;
  mqttReconnectPending = true;
  mqttReconnectAtMs = millis();
  SerialPrintln(F("MQTT: resuming after OTA upload ended without reboot"));
}

//For /settings — lets the web UI (and the bench-acceptance checklist) see
//broker connectivity at a glance (#57).
bool mqttIsConnected() {
  return mqttInitialised && mqttClient.connected();
}
