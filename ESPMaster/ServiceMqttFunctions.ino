// MQTT client service (issue #50). Connects to the broker configured via the
// web UI and publishes state/health/log telemetry + subscribes to command
// topics. Everything here is gated on `mqttHostSetting` being non-empty —
// zero runtime cost when MQTT is disabled.
//
// Transport: AsyncMqttClient on top of ESPAsyncTCP. Shares the async TCP
// stack with the web server, so no additional Wire/Serial overhead and the
// event handlers run off the network task (do NOT block in them).
//
// Lifecycle:
//   setup() -> mqttInit()        register handlers, schedule first connect
//   loop()  -> mqttServiceLoop() reconnect backoff, reconfig flag, heartbeat
//   onConnect                    publishes availability=online, subscribes,
//                                  flushes pre-connect log ring, logs heap
//   onDisconnect                 schedules a reconnect after MQTT_RECONNECT_MS
//   onMessage                    parses topic suffix, dispatches cmd/*
//
// Topic layout (base = "split-flap/<mdnsName>"):
//   <base>/availability            LWT: "online"/"offline", retained
//   <base>/state/message           last text shown, retained
//   <base>/state/ip,uptime,rssi,freeheap   heartbeat, retained
//   <base>/cmd/message             plain text -> showText() on next tick
//   <base>/cmd/stop                any payload -> abort + broadcastHome + clear
//   <base>/cmd/home_all            any payload -> broadcastHome
//   <base>/log                     log sink (wired in task #10)
//
// Architecture guard: only the master is on MQTT. cmd/unit/<N>/* (task #5)
// are still master-scoped operations — the master receives, looks up unit N,
// and speaks I2C on its behalf. Units are never MQTT-addressable.

#include <AsyncMqttClient.h>
#include "WebLog.h"
#include "MqttLogTap.h"
#include "MqttTopicDecoder.h"

#ifndef MQTT_RECONNECT_MS
#define MQTT_RECONNECT_MS 5000
#endif
#ifndef MQTT_STATE_QOS
#define MQTT_STATE_QOS 0
#endif
//Heartbeat republish cadence for numeric state (ip, uptime, rssi, freeheap).
//Short enough that HA's value_updated stamps feel fresh, long enough that we
//don't spam the broker. state/message is published reactively on change.
#ifndef MQTT_HEARTBEAT_MS
#define MQTT_HEARTBEAT_MS 60000
#endif

static AsyncMqttClient mqttClient;
static bool      mqttConfigured    = false;
static bool      mqttConnected     = false;
static uint32_t  mqttLastAttempt   = 0;
static uint32_t  mqttLastHeartbeat = 0;
//Tracks the last state/message we published so we can short-circuit
//identical republishes — showText() calls through every tick even when the
//displayed text hasn't changed.
static String    mqttLastPublishedMessage;
static String    mqttTopicBase;
static String    mqttAvailTopic;
static String    mqttCmdTopicPrefix;    // cached "<base>/cmd/" for fast suffix strip
static uint16_t  mqttPortCached = 1883;

//Pending-work flags set from the async onMessage callback and consumed on
//the next mqttServiceLoop() tick. Keeps Wire/webServer work off the async
//TCP thread — matches the POST /stop -> abortCurrentShow pattern.
static volatile bool mqttPendingStop     = false;
static volatile bool mqttPendingHomeAll  = false;
static volatile bool mqttPendingMessage  = false;
static String        mqttPendingMessagePayload;

//Per-unit pending action — last-wins single-slot queue. Command volume is
//interactive (a user pressing a HA button), not automated, so dropping
//older pending actions is fine. Kind values: 0=none, 1=home, 2=reboot.
static volatile uint8_t mqttPendingUnitKind    = 0;
static volatile uint8_t mqttPendingUnitAddress = 0;

//Forward decls — bound in mqttInit(), defined below. Note: mqttOnMessage
//is NOT a named function because its signature mentions
//AsyncMqttClientMessageProperties, which the Arduino preprocessor can't
//auto-prototype correctly (it substitutes `int` and the build fails with
//a mismatched std::function conversion). Using a lambda at the bind site
//sidesteps the preprocessor entirely — see CLAUDE.md "Gotchas" section.
static void mqttOnConnect(bool sessionPresent);
static void mqttOnDisconnect(AsyncMqttClientDisconnectReason reason);
static void mqttPublishHeartbeat();
static void mqttSubscribeCommands();

static void mqttRebuildTopics() {
  mqttTopicBase      = String("split-flap/") + String(mdnsName);
  mqttAvailTopic     = mqttTopicBase + "/availability";
  mqttCmdTopicPrefix = mqttTopicBase + "/cmd/";
}

bool mqttIsConnected() { return mqttConnected; }

//Apply current settings to the AsyncMqttClient. Call before each connect()
//so config changes actually take effect. Returns false if the config is
//incomplete (empty host or bad port) — caller treats that as "disabled".
static bool mqttApplyConfig() {
  if (mqttHostSetting.length() == 0) return false;

  long asInt = mqttPortSetting.toInt();
  if (asInt < 1 || asInt > 65535) asInt = 1883;
  mqttPortCached = (uint16_t)asInt;

  mqttRebuildTopics();
  mqttClient.setClientId(mdnsName);
  mqttClient.setServer(mqttHostSetting.c_str(), mqttPortCached);
  if (mqttUserSetting.length() > 0) {
    mqttClient.setCredentials(mqttUserSetting.c_str(),
                              mqttPassSetting.length() ? mqttPassSetting.c_str() : nullptr);
  } else {
    mqttClient.setCredentials(nullptr, nullptr);
  }
  mqttClient.setWill(mqttAvailTopic.c_str(), MQTT_STATE_QOS, /*retain=*/true, "offline");
  mqttClient.setKeepAlive(30);
  return true;
}

void mqttInit() {
  mqttClient.onConnect(mqttOnConnect);
  mqttClient.onDisconnect(mqttOnDisconnect);
  //Lambda (see forward-decl note above): the Arduino preprocessor ignores
  //lambdas, so the AsyncMqttClientMessageProperties type resolves correctly
  //from <AsyncMqttClient.h> included in this TU. Body stays non-blocking —
  //sets pending flags; real work happens in mqttServiceLoop().
  mqttClient.onMessage([](char* topic, char* payload,
                          AsyncMqttClientMessageProperties /*properties*/,
                          size_t len, size_t /*index*/, size_t /*total*/) {
    //Decoding is a pure function (MqttTopicDecoder.h) — covered by the
    //native test env. The async callback here just dispatches on the
    //decoded kind; no parsing knowledge lives in the lambda itself.
    MqttCmdDecoded d = mqttDecodeCmdTopic(String(topic), mqttCmdTopicPrefix);
    switch (d.kind) {
      case MqttCmdDecoded::STOP:
        mqttPendingStop = true;
        break;
      case MqttCmdDecoded::HOME_ALL:
        mqttPendingHomeAll = true;
        break;
      case MqttCmdDecoded::MESSAGE:
        //Validate BEFORE copying into the pending String so a malicious
        //payload (unbounded length, control chars) can't pressure the
        //heap. Silent reject matches the rest of the dispatcher's policy
        //— HA will see the display unchanged, which is the right signal.
        if (!mqttValidateMessagePayload((const char*)payload, len)) break;
        mqttPendingMessagePayload = String();
        mqttPendingMessagePayload.reserve(len);
        for (size_t i = 0; i < len; i++) mqttPendingMessagePayload += (char)payload[i];
        mqttPendingMessage = true;
        break;
      case MqttCmdDecoded::UNIT_HOME:
        mqttPendingUnitAddress = (uint8_t)d.unitAddress;
        mqttPendingUnitKind    = 1;
        break;
      case MqttCmdDecoded::UNIT_REBOOT:
        mqttPendingUnitAddress = (uint8_t)d.unitAddress;
        mqttPendingUnitKind    = 2;
        break;
      case MqttCmdDecoded::NONE:
      default:
        //Unknown or malformed topic — silently drop. Matches the "master
        //in control" contract: unsupported actions don't reach the bus.
        break;
    }
  });

  mqttConfigured = mqttApplyConfig();
  if (!mqttConfigured) {
    SerialPrintln(F("MQTT disabled (no host configured)"));
    return;
  }
  SerialPrint(F("MQTT init: "));
  SerialPrint(mqttHostSetting);
  SerialPrint(F(":"));
  SerialPrintln(mqttPortCached);
  mqttLastAttempt = millis();
  mqttClient.connect();
}

//Factor out the /stop sequence so MQTT cmd/stop matches the HTTP path byte
//for byte — cleanest way to stop the two diverging. Safe to call from the
//main loop; not safe from the onMessage callback (Wire bus). Issue #50.
static void mqttPerformStop() {
  abortCurrentShow = true;
  broadcastHome();
  inputText = "";
  lastWrittenText = "";
  SerialPrintln(F("MQTT cmd/stop applied"));
}

void mqttServiceLoop() {
  if (isPendingMqttReconfig) {
    isPendingMqttReconfig = false;
    SerialPrintln(F("MQTT: applying new config"));
    if (mqttConnected) mqttClient.disconnect(true);
    mqttConfigured = mqttApplyConfig();
    if (!mqttConfigured) return;
    mqttLastAttempt = millis();
    mqttClient.connect();
    return;
  }

  //Deferred command dispatch. Runs on the main loop so Wire/broadcastHome
  //calls don't reenter from the async TCP thread.
  if (mqttPendingStop) {
    mqttPendingStop = false;
    mqttPerformStop();
  }
  if (mqttPendingHomeAll) {
    mqttPendingHomeAll = false;
    broadcastHome();
    SerialPrintln(F("MQTT cmd/home_all applied"));
  }
  if (mqttPendingMessage) {
    mqttPendingMessage = false;
    //Match the POST / text-mode semantics: new inputText + force deviceMode
    //to text so the clock doesn't overwrite the message on its next tick.
    //Users who want the clock back can flip the mode in the web UI.
    inputText = mqttPendingMessagePayload;
    deviceMode = DEVICE_MODE_TEXT;
    alignmentUpdated = true;
    mqttPendingMessagePayload = String();  //release the heap copy
    SerialPrintln(F("MQTT cmd/message applied"));
  }
  if (mqttPendingUnitKind != 0) {
    uint8_t kind = mqttPendingUnitKind;
    uint8_t addr = mqttPendingUnitAddress;
    mqttPendingUnitKind = 0;
    //kind 1=home, 2=reboot. Both are safe, idempotent recoveries; the
    //destructive bootloader command is intentionally never routed here.
    if (kind == 1) {
      homeUnit((int)addr);
      SerialPrint(F("MQTT cmd/unit home addr=0x")); SerialPrintln(String(addr, HEX));
    } else if (kind == 2) {
      rebootUnit((int)addr);
      SerialPrint(F("MQTT cmd/unit reboot addr=0x")); SerialPrintln(String(addr, HEX));
    }
  }

  if (!mqttConfigured) return;
  if (!mqttConnected) {
    uint32_t now = millis();
    if ((now - mqttLastAttempt) < MQTT_RECONNECT_MS) return;
    mqttLastAttempt = now;
    mqttClient.connect();
    return;
  }

  //Connected path: reactively publish state/message on change, and rate-
  //limit the numeric heartbeat to MQTT_HEARTBEAT_MS so we don't spam.
  if (lastWrittenText != mqttLastPublishedMessage) {
    mqttLastPublishedMessage = lastWrittenText;
    String topic = mqttTopicBase + "/state/message";
    mqttClient.publish(topic.c_str(), MQTT_STATE_QOS, /*retain=*/true,
                       lastWrittenText.c_str(), lastWrittenText.length());
  }

  uint32_t now = millis();
  if ((now - mqttLastHeartbeat) >= MQTT_HEARTBEAT_MS) {
    mqttLastHeartbeat = now;
    mqttPublishHeartbeat();
  }
}

//Single-field publish helper. Wraps the raw mqttClient.publish() in
//String-aware signature handling so the call sites stay readable.
static inline void mqttPubRetainedStr(const String& topic, const String& value) {
  mqttClient.publish(topic.c_str(), MQTT_STATE_QOS, /*retain=*/true,
                     value.c_str(), value.length());
}

static void mqttPublishHeartbeat() {
  //All fields retained so a late-subscribing HA backfills its state from
  //the broker instead of waiting for the next heartbeat tick.
  mqttPubRetainedStr(mqttTopicBase + "/state/ip",       WiFi.localIP().toString());
  mqttPubRetainedStr(mqttTopicBase + "/state/uptime",   String((uint32_t)(millis() / 1000UL)));
  mqttPubRetainedStr(mqttTopicBase + "/state/rssi",     String((int)WiFi.RSSI()));
  mqttPubRetainedStr(mqttTopicBase + "/state/freeheap", String((uint32_t)ESP.getFreeHeap()));

  //Per-unit publishes (#50). Walk detectedUnitAddresses[] — the I2C probe
  //keeps this up to date. Each unit contributes one I2C round-trip for
  //readUnitStatus() + 8 MQTT publishes; with 10 units that's ~80 publishes
  //per heartbeat tick. We interleave a yield() between units so the async
  //TCP task can drain its out queue and onConnect's "online" publish
  //doesn't starve. If heap pressure shows up on real hardware, stagger
  //these across ticks (one unit per tick) instead.
  for (int i = 0; i < detectedUnitCount; i++) {
    int addr = detectedUnitAddresses[i];
    //Skip units that aren't in sketch mode — readUnitStatus only makes
    //sense for the running firmware. Bootloader-only units (state 2) have
    //no status payload to return.
    if (detectedUnitStates[i] != 1) continue;

    UnitStatus s;
    if (!readUnitStatus(addr, s)) continue;  //old firmware / Wire error

    String base = mqttTopicBase + "/unit/" + String(addr);
    mqttPubRetainedStr(base + "/uptime",          String((uint32_t)s.uptimeSeconds));
    mqttPubRetainedStr(base + "/mcusr",           String((uint32_t)s.mcusrAtBoot));
    mqttPubRetainedStr(base + "/brownout",        String((uint32_t)s.lifetimeBrownoutCount));
    mqttPubRetainedStr(base + "/wdt",             String((uint32_t)s.lifetimeWatchdogCount));
    mqttPubRetainedStr(base + "/badCmd",          String((uint32_t)s.badCommandCount));
    mqttPubRetainedStr(base + "/moving",          (s.flags & 0x01) ? String("1") : String("0"));
    mqttPubRetainedStr(base + "/homeFailed",      (s.flags & 0x02) ? String("1") : String("0"));
    mqttPubRetainedStr(base + "/lastHomingSteps", String((uint32_t)s.lastHomingStepCount));

    yield();  //let AsyncTCP drain between units; I2C is serial anyway
  }
}

//Wildcard subscribe to every command under the base. Cheaper on the broker
//than N individual subscribes, and the topic string we receive in onMessage
//tells us which one. Issue #50.
static void mqttSubscribeCommands() {
  String sub = mqttTopicBase + "/cmd/#";
  mqttClient.subscribe(sub.c_str(), /*qos=*/1);
}

//HA MQTT discovery (issue #50). HA scans homeassistant/<component>/<nodeId>/
//<objectId>/config on subscribe and auto-creates entities. We publish every
//entity we want it to surface — retained, so HA rediscovers on restart
//without us needing to republish.
//
//Sizing: each payload is ~150-300 B. With 10 units * ~8 entities + master
//* ~8 entities, total one-shot JSON is ~30 KB of broker traffic. We build
//one payload at a time to keep heap pressure bounded (String reused across
//iterations; grows to max ~400 B then shrinks).
//
//HA short keys used (saves ~40% payload bytes vs long form):
//  uniq_id -> unique_id, stat_t -> state_topic, cmd_t -> command_topic,
//  avty_t -> availability_topic, dev_cla -> device_class,
//  unit_of_meas -> unit_of_measurement, ent_cat -> entity_category.

//Wraps the JSON-escape rules we care about (no user-supplied strings here,
//so we only need to escape the few quotes / backslashes in our own fields).
static inline void appendJsonStr(String& out, const char* key, const String& value) {
  out += '"'; out += key; out += F("\":\""); out += value; out += '"';
}
static inline void appendJsonStr(String& out, const char* key, const char* value) {
  out += '"'; out += key; out += F("\":\""); out += value; out += '"';
}

//Publishes a single discovery config. `component` is HA's entity domain
//(sensor, binary_sensor, button, text). `objId` must be unique per-device.
//Extra JSON is injected verbatim into the top-level object, already
//comma-prefixed — callers pass e.g. `,"unit_of_meas":"s"`.
static void mqttPubDiscovery(const char* component,
                             const String& deviceUid,
                             const char* objId,
                             const char* displayName,
                             const String& deviceBlock,
                             const String& extras) {
  String topic = String(F("homeassistant/")) + component + '/' + deviceUid + '/' + objId + F("/config");
  String uid   = deviceUid + '_' + objId;

  String p;
  p.reserve(360);
  p += '{';
  appendJsonStr(p, "uniq_id", uid);           p += ',';
  appendJsonStr(p, "name",    displayName);   p += ',';
  appendJsonStr(p, "avty_t",  mqttAvailTopic);
  p += extras;
  p += ','; p += deviceBlock;
  p += '}';

  mqttClient.publish(topic.c_str(), MQTT_STATE_QOS, /*retain=*/true,
                     p.c_str(), p.length());
  yield();  //let AsyncTCP drain before the next payload
}

//Builds the "device":{...} block shared by every entity belonging to the
//given device. HA groups entities by matching identifiers, so the object
//must be byte-identical across all entities in the same device.
static String mqttBuildDeviceBlock(const String& identifier, const String& name, const char* model) {
  String d;
  d.reserve(160);
  d += F("\"device\":{\"identifiers\":[\"");
  d += identifier;
  d += F("\"],\"name\":\"");
  d += name;
  d += F("\",\"model\":\"");
  d += model;
  d += F("\",\"manufacturer\":\"split-flap\",\"via_device\":\"");
  d += mdnsName;  //units are "via" the master
  d += F("\"}");
  return d;
}

static void mqttPublishDiscovery() {
  //Master device — identifier uses mdnsName (unique per display on the LAN).
  String masterUid  = String(mdnsName);
  String masterName = String(F("Split-Flap ")) + mdnsName;
  String masterDev;
  {
    String d;
    d.reserve(160);
    d += F("\"device\":{\"identifiers\":[\"");
    d += masterUid;
    d += F("\"],\"name\":\"");
    d += masterName;
    d += F("\",\"model\":\"ESP-01 master\",\"manufacturer\":\"split-flap\"}");
    masterDev = d;
  }

  //---- Master entities ----
  String ex;

  //state/message sensor
  ex = String(F(",\"stat_t\":\"")) + mqttTopicBase + F("/state/message\"");
  mqttPubDiscovery("sensor", masterUid, "message", "Current message", masterDev, ex);

  //state/uptime sensor (seconds)
  ex = String(F(",\"stat_t\":\"")) + mqttTopicBase + F("/state/uptime\"")
     + F(",\"unit_of_meas\":\"s\",\"dev_cla\":\"duration\",\"ent_cat\":\"diagnostic\"");
  mqttPubDiscovery("sensor", masterUid, "uptime", "Uptime", masterDev, ex);

  //state/rssi sensor (dBm)
  ex = String(F(",\"stat_t\":\"")) + mqttTopicBase + F("/state/rssi\"")
     + F(",\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\",\"ent_cat\":\"diagnostic\"");
  mqttPubDiscovery("sensor", masterUid, "rssi", "WiFi RSSI", masterDev, ex);

  //state/freeheap sensor (bytes)
  ex = String(F(",\"stat_t\":\"")) + mqttTopicBase + F("/state/freeheap\"")
     + F(",\"unit_of_meas\":\"B\",\"ent_cat\":\"diagnostic\"");
  mqttPubDiscovery("sensor", masterUid, "freeheap", "Free heap", masterDev, ex);

  //cmd/message text input — HA "text" entity with both stat_t and cmd_t so
  //the user sees the currently-displayed text and can overwrite it.
  ex = String(F(",\"stat_t\":\"")) + mqttTopicBase + F("/state/message\"")
     + F(",\"cmd_t\":\"")          + mqttTopicBase + F("/cmd/message\"")
     + F(",\"max\":75");  //matches the web UI's <input maxlength="75">
  mqttPubDiscovery("text", masterUid, "set_message", "Set message", masterDev, ex);

  //cmd/stop button
  ex = String(F(",\"cmd_t\":\"")) + mqttTopicBase + F("/cmd/stop\"")
     + F(",\"pl_prs\":\"1\"");  //payload_press
  mqttPubDiscovery("button", masterUid, "stop", "Stop", masterDev, ex);

  //cmd/home_all button
  ex = String(F(",\"cmd_t\":\"")) + mqttTopicBase + F("/cmd/home_all\"")
     + F(",\"pl_prs\":\"1\"");
  mqttPubDiscovery("button", masterUid, "home_all", "Home all units", masterDev, ex);

  //---- Per-unit entities ----
  //One device per detected unit. Each unit's entities are marked
  //entity_category=diagnostic to keep them tucked away in HA's UI — they're
  //observability, not primary controls.
  for (int i = 0; i < detectedUnitCount; i++) {
    int addr = detectedUnitAddresses[i];
    if (detectedUnitStates[i] != 1) continue;  //skip bootloader/silent units

    String addrStr = String(addr);
    String unitUid  = masterUid + F("_unit_") + addrStr;
    String unitName = String(F("Split-Flap ")) + mdnsName + F(" unit 0x");
    if (addr < 0x10) unitName += '0';
    unitName += String(addr, HEX);
    String unitDev  = mqttBuildDeviceBlock(unitUid, unitName, "Nano unit");

    String base = mqttTopicBase + F("/unit/") + addrStr;

    ex = String(F(",\"stat_t\":\"")) + base + F("/uptime\"")
       + F(",\"unit_of_meas\":\"s\",\"dev_cla\":\"duration\",\"ent_cat\":\"diagnostic\"");
    mqttPubDiscovery("sensor", unitUid, "uptime", "Uptime", unitDev, ex);

    ex = String(F(",\"stat_t\":\"")) + base + F("/mcusr\",\"ent_cat\":\"diagnostic\"");
    mqttPubDiscovery("sensor", unitUid, "mcusr", "Reset cause (MCUSR)", unitDev, ex);

    ex = String(F(",\"stat_t\":\"")) + base + F("/brownout\",\"ent_cat\":\"diagnostic\"");
    mqttPubDiscovery("sensor", unitUid, "brownout", "Brownout count", unitDev, ex);

    ex = String(F(",\"stat_t\":\"")) + base + F("/wdt\",\"ent_cat\":\"diagnostic\"");
    mqttPubDiscovery("sensor", unitUid, "wdt", "Watchdog resets", unitDev, ex);

    ex = String(F(",\"stat_t\":\"")) + base + F("/badCmd\",\"ent_cat\":\"diagnostic\"");
    mqttPubDiscovery("sensor", unitUid, "badCmd", "Bad I2C commands", unitDev, ex);

    ex = String(F(",\"stat_t\":\"")) + base + F("/lastHomingSteps\",\"ent_cat\":\"diagnostic\"");
    mqttPubDiscovery("sensor", unitUid, "homingSteps", "Last homing steps", unitDev, ex);

    //Binary sensors use "1"/"0" payload mapping.
    ex = String(F(",\"stat_t\":\"")) + base + F("/moving\"")
       + F(",\"pl_on\":\"1\",\"pl_off\":\"0\",\"dev_cla\":\"motion\",\"ent_cat\":\"diagnostic\"");
    mqttPubDiscovery("binary_sensor", unitUid, "moving", "Moving", unitDev, ex);

    ex = String(F(",\"stat_t\":\"")) + base + F("/homeFailed\"")
       + F(",\"pl_on\":\"1\",\"pl_off\":\"0\",\"dev_cla\":\"problem\",\"ent_cat\":\"diagnostic\"");
    mqttPubDiscovery("binary_sensor", unitUid, "homeFailed", "Home failed", unitDev, ex);

    //Per-unit command buttons. cmd/unit/<addr>/home and /reboot.
    ex = String(F(",\"cmd_t\":\"")) + mqttTopicBase + F("/cmd/unit/") + addrStr + F("/home\"")
       + F(",\"pl_prs\":\"1\"");
    mqttPubDiscovery("button", unitUid, "home", "Re-home", unitDev, ex);

    ex = String(F(",\"cmd_t\":\"")) + mqttTopicBase + F("/cmd/unit/") + addrStr + F("/reboot\"")
       + F(",\"pl_prs\":\"1\"");
    mqttPubDiscovery("button", unitUid, "reboot", "Soft reboot", unitDev, ex);
  }
}

//---- MQTT log sink (task #10) --------------------------------------------
//
// Two paths:
//   a) `mqttLogTap` captures live SerialPrint* output line-by-line and
//      publishes each completed line to <base>/log. Active only while
//      MQTT is connected; short-circuits otherwise so the ring buffer
//      (WebLog.h) remains the pre-connect / disconnected fallback.
//   b) `mqttFlushLogRing()` is called on every onConnect. It publishes
//      whatever's currently in the ring as a single chunk — catches up
//      the broker on everything that happened before MQTT came up (or
//      during the last disconnect).
//
// Between the two, any log line written while MQTT is connected appears
// exactly once (via the tap), and any line from before connect appears
// once (via the ring flush).

MqttLogTap mqttLogTap;          //referenced from HelpersSerialHandling.h
static String mqttLogTapLineBuf;  //accumulator for the current line

//Line-length cap. Log lines should be well under this; the cap is purely a
//defense against a pathological caller that prints a huge blob without a
//newline. Prevents the buffer from growing unbounded on the ESP-01's heap.
static const size_t MQTT_LOG_LINE_MAX = 256;

//Publish helper shared by the tap and the ring flush. Keeps the topic
//computation + QoS/retain policy in one place.
static void mqttPublishLogLine(const char* data, size_t len) {
  if (!mqttConnected || len == 0) return;
  String topic = mqttTopicBase + F("/log");
  //Not retained — log lines are transient by nature, and the broker should
  //not replay last N log lines to every new subscriber.
  mqttClient.publish(topic.c_str(), MQTT_STATE_QOS, /*retain=*/false, data, len);
}

size_t MqttLogTap::write(uint8_t b) {
  if (!mqttConnected) {
    //While disconnected the ring buffer is the source of truth; keep the
    //tap accumulator empty so we don't dump a stale partial line after
    //reconnect alongside the ring flush.
    if (mqttLogTapLineBuf.length() > 0) mqttLogTapLineBuf = String();
    return 1;
  }
  if (b == '\n' || b == '\r') {
    if (mqttLogTapLineBuf.length() > 0) {
      mqttPublishLogLine(mqttLogTapLineBuf.c_str(), mqttLogTapLineBuf.length());
      mqttLogTapLineBuf = String();
    }
  } else if (mqttLogTapLineBuf.length() < MQTT_LOG_LINE_MAX) {
    mqttLogTapLineBuf += (char)b;
  }
  //Silently drop chars past MQTT_LOG_LINE_MAX — better than growing heap.
  return 1;
}

size_t MqttLogTap::write(const uint8_t* buffer, size_t size) {
  for (size_t i = 0; i < size; i++) write(buffer[i]);
  return size;
}

void mqttFlushLogRing() {
  if (!mqttConnected) return;
  String content = webLogRead();
  if (content.length() == 0) return;
  mqttPublishLogLine(content.c_str(), content.length());
}

static void mqttOnConnect(bool sessionPresent) {
  mqttConnected = true;
  (void)sessionPresent;
  mqttClient.publish(mqttAvailTopic.c_str(), MQTT_STATE_QOS, /*retain=*/true, "online");

  //Force the heartbeat to fire on the next service tick so HA gets state
  //immediately instead of waiting up to MQTT_HEARTBEAT_MS.
  mqttLastHeartbeat = 0;
  //Force state/message republish even if it matches cache — the cache is
  //ours, the broker might not have our retained value yet (fresh session).
  mqttLastPublishedMessage = String();

  mqttSubscribeCommands();

  SerialPrint(F("MQTT connected. Free heap pre-discovery: "));
  SerialPrintln(ESP.getFreeHeap());

  //HA auto-discovery — runs once per connect. Payloads are retained, so HA
  //rediscovers on its own restart even when the ESP is quiet. Only publishes
  //per-unit entities for units we actually saw on the I2C bus, so adding a
  //unit later requires a reboot of the master (same constraint as the web UI).
  mqttPublishDiscovery();

  SerialPrint(F("MQTT post-discovery free heap: "));
  SerialPrintln(ESP.getFreeHeap());

  mqttFlushLogRing();
}

static void mqttOnDisconnect(AsyncMqttClientDisconnectReason reason) {
  mqttConnected = false;
  SerialPrint(F("MQTT disconnected, reason="));
  SerialPrintln((int)reason);
  mqttLastAttempt = millis();
}

