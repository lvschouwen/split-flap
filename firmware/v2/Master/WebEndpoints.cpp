// Web endpoint core for the v2 master (#186) — shared state, init/loop
// lifecycle and the cross-task accessors. The route handlers live in the
// Web*.cpp family (#338 split): WebContent (assets/SSE), WebSettings
// (settings/wifi/mqtt-discover), WebSystem (diagnostics reads), WebFirmware
// (master OTA/rescue), WebMaintenance (unit ops), WebCluster (cluster wire +
// CORS middleware). Shared internals cross that family through
// WebEndpointsInternal.h ONLY.
//
// Async-context rule (v1 #150) carries over verbatim: handlers run in the
// async_tcp task. They may parse, validate, read state and respond — they
// must NOT mutate shared Strings, write NVS, or hold hardware buses.
// Mutating work is staged (pendingPost, pendingReboot) and drained from
// loop() via webEndpointsLoop().

#include "WebEndpoints.h"
#include "WebEndpointsInternal.h"

#include <ESPAsyncWebServer.h>
#include <WiFi.h>

#include "BuildVersion.h"
#include "ClockPolicy.h"
#include "ClockService.h"
#include "ClusterDigest.h"  // clusterCsrfRejectPost (inline upload gate)
#include "ClusterFollower.h"
#include "ClusterLeader.h"
#include "FlashLog.h"
#include "FollowerImageStore.h"
#include "HelpersSerialHandling.h"
#include "MqttService.h"
#include "OtaService.h"
#include "SettingsJson.h"
#include "SplitFlapProtocol.h"
#include "Tasks.h"

// Staged mutations, owned here; drained by webEndpointsLoop(). External
// linkage across the Web*.cpp family (WebEndpointsInternal.h).
PendingSettingsPost pendingPost;
bool pendingReboot = false;
uint32_t rebootRequestedAtMs = 0;
String pendingIntendedVersion;  // ?v= from /firmware/master (#190)
bool pendingIntendedVersionProvided = false;

// Live state the read handlers render. Set once in webEndpointsInit();
// handlers only ever read (async-context rule). Held as module globals
// rather than lambda-captured references so a handler can never outlive
// what it captured.
MasterSettings* liveSettings = nullptr;
SettingsStore* liveStore = nullptr;  // MQTT setters persist through it
String effectiveName;

// Runtime-only message state (#192, v1 parity: never persisted, "" at
// boot). Written by the drain, read by GET /settings and the clock ticker's
// webDisplayContentSnapshot() — all under webStateMutex.
String currentInputText;
String lastMessageStamp;

// Cross-task guard. Unlike v1's single-core cooperative ESP8266, the
// handlers here run in the async_tcp FreeRTOS task while the drain runs in
// loopTask — Arduino Strings shared between them (pendingPost, *liveSettings,
// lastWrittenText) need real synchronization or a reader can see a buffer
// mid-free. A mutex (not a spinlock) on purpose: the critical sections
// allocate Strings and the drain writes NVS, neither of which is allowed
// inside portENTER_CRITICAL.
SemaphoreHandle_t webStateMutex = nullptr;

const char* webResetReasonString() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "Power on";
    case ESP_RST_EXT:       return "External reset";
    case ESP_RST_SW:        return "Software reset";
    case ESP_RST_PANIC:     return "Exception/panic";
    case ESP_RST_INT_WDT:   return "Interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "Task watchdog";
    case ESP_RST_WDT:       return "Other watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
    case ESP_RST_BROWNOUT:  return "Brownout";
    case ESP_RST_SDIO:      return "SDIO reset";
    default:                return "Unknown";
  }
}

// #313: the CORS/CSRF server middleware fires only at PARSE_REQ_END — AFTER an
// upload route's onUpload callback has already streamed the body to flash. So
// the routes that WRITE inside onUpload (master OTA, rescue install, follower
// image) must gate CSRF INLINE at index==0, before the first write, exactly
// as the ESP-01 follower does. Returns true when the upload is a forged
// cross-site POST; the caller marks its own per-request rejection state.
bool webUploadCsrfRejected(AsyncWebServerRequest* request) {
  bool hasOrigin = request->hasHeader("Origin");
  return clusterCsrfRejectPost(true, hasOrigin,
                               hasOrigin ? request->header("Origin") : String());
}

// Gathers the full /settings JSON from the display snapshot, live settings and
// the OTA/cluster/MQTT services. Extracted so both GET /settings and the
// /status one-shot aggregate (#307) render the identical object.
String buildCurrentSettingsJson() {
  SettingsJsonFields f;
  DisplaySnapshot snap = displaySnapshotGet();
  int addrs[UNITS_AMOUNT];
  int fwStatus[UNITS_AMOUNT];
  String versions[UNITS_AMOUNT];
  int detected = 0;
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    if (snap.units[i].state != 0) {
      addrs[detected++] = SFP_I2C_ADDRESS_BASE + i;
    }
    fwStatus[i] = snap.units[i].fwStatus;
    versions[i] = snap.units[i].version;
  }
  f.unitsAmount = UNITS_AMOUNT;
  f.unitCount = snap.displayWidth;
  f.detectedUnitCount = detected;
  f.detectedUnitAddresses = addrs;
  f.detectedUnitVersionStatus = fwStatus;
  f.detectedUnitVersions = versions;
  // #335 per-board vitals — runtime, no settings lock needed. Same source as
  // the ESP-01 follower (heap bytes / RSSI dBm / uptime seconds); plat keeps
  // its "esp32s3" default. Lets S3 cluster members show vitals like the ESP-01.
  f.heapBytes = ESP.getFreeHeap();
  f.rssiDbm = WiFi.RSSI();
  f.upSeconds = millis() / 1000;
  {
    WebStateLock lock;
    f.alignment = liveSettings->alignment;
    f.flapSpeed = String(liveSettings->flapSpeed);
    f.deviceMode = liveSettings->deviceMode;
    f.deviceRole = liveSettings->deviceRole;
    // #329: nudge only while still on the default display role — detection
    // suggests, never self-demotes.
    f.headlessSuggested =
        headlessShouldSuggest(snap.headlessUnitless, liveSettings->deviceRole);
    f.timezonePosix = liveSettings->timezonePosix;
    f.unitCountOverride = liveSettings->unitCountOverride;
    f.deviceName = liveSettings->deviceName;
    f.effectiveDeviceName = effectiveName;
    f.mqttHost = liveSettings->mqttHost;
    f.mqttPort = String(liveSettings->mqttPort);
    f.mqttUser = liveSettings->mqttUser;
    f.mqttPasswordSet = liveSettings->mqttPassword.length() > 0;
    f.intendedVersion = liveSettings->intendedVersion;
    f.wifiSettingsResettable = liveSettings->wifiSsid.length() > 0;
    f.lastTimeReceivedMessageDateTime = lastMessageStamp;
  }
  f.lastWrittenText = String(snap.currentText);
  f.mqttConnected = mqttIsConnected();
  f.version = GIT_REV;
  f.sketchMd5 = ESP.getSketchMD5();
  OtaVerdict verdict = otaVerdictSnapshot();
  f.lastFlashResult = verdict.lastFlashResult;
  f.otaReverted = verdict.otaReverted;
  f.lastResetReason = webResetReasonString();
  ClusterFollowerView cluster = clusterFollowerViewGet();
  f.clusterState = clusterFollowerPhaseName(cluster.phase);
  f.clusterLeaderName = cluster.leaderName;
  f.clusterLeaderHost = cluster.leaderHost;
  f.clusterRow = cluster.row;
  f.clusterLeading = clusterLeaderEnabled();
  return buildSettingsJson(f);
}

void webEndpointsInit(AsyncWebServer& server, MasterSettings& settings,
                      SettingsStore& store,
                      const String& effectiveDeviceName) {
  server.addMiddleware(&webClusterCorsMiddleware());
  // #332: seed the leader's self-row role (clusterLeaderInit ran earlier in
  // setup(), so LeaderLock is safe); the settings drain pushes changes.
  clusterLeaderSetSelfRole(settings.deviceRole);
  // Handlers never write the store; the loop drain and the mqttTask-called
  // setters below do (both hold webStateMutex).
  webStateMutex = xSemaphoreCreateMutex();
  if (webStateMutex == nullptr) {
    // Boot-time OOM: WebStateLock on a null handle is UB, so fail loudly
    // instead — abort() panics into the coredump partition.
    Serial.println(F("FATAL: webStateMutex allocation failed"));
    abort();
  }
  liveSettings = &settings;
  liveStore = &store;
  effectiveName = effectiveDeviceName;

  // Per-module route registration (#338). Cross-module order is not
  // semantic — the server matches per path+method, same-path method pairs
  // stay within one module, and onNotFound/middleware are setters.
  webContentRegister(server);
  webSettingsRegister(server);
  webSystemRegister(server);
  webFirmwareRegister(server);
  webMaintenanceRegister(server);
  webClusterRegister(server);
}

void webEndpointsStart(AsyncWebServer& server) {
  // Idempotent: both the portal and the STA-online path call this — the
  // first netif up wins, a second begin() must not double-register.
  static bool started = false;
  if (started) return;
  started = true;
  server.begin();
  SerialPrintln(F("Web server started"));
}

void webEndpointsLoop(MasterSettings& settings, SettingsStore& store) {
  if (webStateMutex == nullptr) return;  // init hasn't run

  // Drain order is LOAD-BEARING and preserved verbatim from the pre-#338
  // monolith: (1) OTA stall watchdog, (2) settings-post drain under the
  // lock, (3) reboot hold fan-out, (4) rebootless TZ apply, (5) MQTT then
  // (6) cluster mDNS discovery (LWIP locks — never nested inside
  // webStateMutex), (7) flash-log + follower-image flushes (netTask is the
  // sole flash writer), (8) the reboot itself last.
  webFirmwareLoop();

  // The whole drain holds the mutex: applySettingsPost mutates the same
  // `settings` Strings GET /settings snapshots from the async task, and its
  // NVS writes are legal under a mutex (unlike a portENTER_CRITICAL
  // section). Worst case a handler blocks a few ms behind a flash commit.
  bool rebootDue = false;
  bool timezoneChanged = false;
  {
    WebStateLock lock;
    if (pendingPost.pending) {
      // Display-bound fields are read off the post before apply resets it.
      String messageText = pendingPost.inputText;
      bool messageProvided = pendingPost.inputTextProvided;
      String transientText = pendingPost.transientText;
      long transientDwell = pendingPost.transientDwell;
      bool transientProvided = pendingPost.transientTextProvided;
      bool modeProvided = pendingPost.deviceModeProvided;

      // "Last Received" tracks messages/mode switches, not settings saves —
      // per-card posts (#128) mean only those submissions stamp it.
      if (messageProvided || modeProvided) {
        lastMessageStamp = formatDateTime(time(nullptr), CLOCK_STAMP_FORMAT);
      }

      // Settings first, command second: a speed/alignment change riding the
      // same POST as a message must apply to that message (v1 ordering).
      String timezoneBefore = settings.timezonePosix;
      int unitCountBefore = settings.unitCountOverride;
      String deviceRoleBefore = settings.deviceRole;
      applySettingsPost(pendingPost, settings, store);
      timezoneChanged = settings.timezonePosix != timezoneBefore;

      // #289 dummy mode / #331 headless: push a changed override or deviceRole
      // to displayTask and queue a Probe so the width refolds now instead of
      // at the next bus op (a headless role forces displayWidth 0).
      if (settings.unitCountOverride != unitCountBefore) {
        tasksSetUnitCountOverride(settings.unitCountOverride);
        displayEnqueue(makeProbeCommand());
      }
      if (settings.deviceRole != deviceRoleBefore) {
        tasksSetDeviceRole(settings.deviceRole);
        clusterLeaderSetSelfRole(settings.deviceRole);  // #332 self row
        displayEnqueue(makeProbeCommand());
      }

      // Explicit mode switch or message send trumps a running notification
      // (v1 #130 rule) — cancel so the next 1 Hz tick (or the direct
      // enqueue below) flaps the new content, not the overlay. Reads the
      // pre-apply capture: applySettingsPost() just reset the post's
      // provided flags (#219 fix — the reset made this condition dead for
      // mode switches). Skipped when a transient rides the same POST: its
      // overlay arm below supersedes the old one anyway, and a staged
      // cancel draining one tick earlier than the arm would drop the
      // clockTask gate for a one-frame stray re-show (cpp-review MED).
      if ((modeProvided || messageProvided) && !transientProvided) {
        mqttCancelNotification();
      }

      // v1 parity: a posted message only takes effect in text mode, checked
      // AFTER any mode field riding the same POST. In clock mode it is
      // silently ignored — never shown, never retained.
      if (messageProvided && settings.deviceMode == "text") {
        // Retained in the display domain: ClockPolicy's dedup compares this
        // against snapshot text, which makeShowTextCommand truncates. A
        // LEADING master retains untruncated — the grid holds more than
        // one row's width, and its ticker path never enters that dedup.
        currentInputText = clusterLeaderEnabled()
                               ? messageText
                               : truncateForDisplay(messageText);
        // Reflash gate re-check at drain time (#205, Codex review): the
        // handler's 409 ran when the POST arrived; a job that started in
        // between must not get a ShowText queued behind it. Dropping is
        // self-healing — the retained text above is what the 1 Hz mode
        // ticker re-shows once the job ends.
        // Same re-check for the cluster gate (#272): a membership that
        // arrived between handler and drain must not slip a ShowText in.
        // (Lock order: webStateMutex → clusterMutex, never the reverse.)
        if (reflashInProgress(displaySnapshotGet().reflash) ||
            clusterFollowerViewGet().gated) {
          SerialPrintln("Message retained, not queued (reflash/cluster): " +
                        messageText);
        } else if (clusterLeaderEnabled()) {
          // Leader reroute (#273): the LOGICAL text goes to the cluster
          // layer — it slices the grid, stages our own row, and fans the
          // rest out from clusterTask.
          clusterLeaderSubmitText(messageText, settings.alignment,
                                  settings.flapSpeed);
          SerialPrintln("Message routed to the cluster grid: " + messageText);
        } else if (displayEnqueue(makeShowTextCommand(
                       messageText, settings.alignment,
                       settings.flapSpeed))) {
          SerialPrintln("Message queued for display: " + messageText);
        } else {
          // The handler's 503 pre-check makes this a wedged-queue signal,
          // not a normal path.
          SerialPrintln("Display queue full at drain — message DROPPED: " +
                        messageText);
        }
      } else if (messageProvided) {
        SerialPrintln("Message ignored (device in clock mode, v1 parity): " +
                      messageText);
      }

      // Transient text (#219, v1 #165/#176): calibration patterns and
      // clock-mode messages show regardless of mode and revert via the
      // overlay dwell — nothing persists, a clock display stays a clock
      // display. On a cluster LEADER this stays deliberately local: the
      // overlay shows on this master's own row only (launch scope is grid
      // text + cluster clock), and clusterTask's self-row re-show restores
      // the segment after the dwell. Ordering matters twice: after applySettingsPost so an
      // alignment/speed change riding the same POST applies to this show,
      // and the overlay arm (which drains behind the #130 cancel above)
      // keeps the transient alive when that same POST also switched mode.
      if (transientProvided) {
        if (reflashInProgress(displaySnapshotGet().reflash)) {
          SerialPrintln("Transient text dropped (reflash running): " +
                        transientText);
        } else if (displayEnqueue(makeShowTextCommand(
                       transientText, settings.alignment,
                       settings.flapSpeed))) {
          mqttStartNotificationDwell(transientDwell);
          SerialPrintln(
              "Transient text (dwell " +
              (transientDwell > 0 ? String(transientDwell) + " s"
                                  : String("default")) +
              "): " + transientText);
        } else {
          SerialPrintln("Display queue full at drain — transient DROPPED: " +
                        transientText);
        }
      }
    }
    if (pendingIntendedVersionProvided) {
      pendingIntendedVersionProvided = false;
      if (settings.intendedVersion != pendingIntendedVersion) {
        settings.intendedVersion = pendingIntendedVersion;
        saveIntendedVersion(store, pendingIntendedVersion);
      }
    }
    // Small grace period so the HTTP response flushes before the restart.
    rebootDue = pendingReboot && millis() - rebootRequestedAtMs > 750;
  }

  // #321: before an intentional reboot, announce a graceful hold to followers
  // (once) and wait until clusterTask has fanned it out — so a designated
  // backup doesn't take over during the reboot window. Bounded (4 s) so a stuck
  // member can't block the reboot. clusterLeader* take the LEADER lock, so they
  // run out here, never nested under the WebState lock above.
  if (rebootDue) {
    static bool rebootHoldAnnounced = false;
    if (!rebootHoldAnnounced) {
      rebootHoldAnnounced = true;
      clusterLeaderAnnounceRebootHold();
    }
    if (!clusterLeaderRebootHoldSent() &&
        millis() - rebootRequestedAtMs < 4000) {
      rebootDue = false;  // hold not fanned out yet — hold the reboot
    }
  }

  // Outside the lock: configTzTime takes the LWIP core lock — keep the two
  // lock domains from ever nesting (v1 #48 parity: TZ applies rebootless).
  if (timezoneChanged) clockServiceApplyTz(settings);

  // mDNS discovery drains (#224 MQTT, #274 cluster): blocking queries take
  // LWIP locks, so they run out here in netTask, outside webStateMutex.
  webSettingsDiscoverLoop();
  webClusterDiscoverLoop();

  // Flash-log drain (#206): netTask is the single flash writer.
  flashLogTick(rebootDue);  // force on reboot so the last lines land
  // Staged follower-image write (#304): same single-writer discipline — the
  // async upload handler accumulates in PSRAM, netTask commits it to flash.
  followerImageFlushTick();

  if (rebootDue) {
    SerialPrintln(F("Rebooting..."));
    Serial.flush();
    flashLogTick(true);  // catch the reboot line itself
    ESP.restart();
  }
}

WebContentSnapshot webDisplayContentSnapshot() {
  WebContentSnapshot c;
  // Both are set together in webEndpointsInit(); guard both anyway so this
  // stays safe if init ordering ever changes.
  if (webStateMutex == nullptr || liveSettings == nullptr) return c;
  WebStateLock lock;
  c.deviceMode = liveSettings->deviceMode;
  c.inputText = currentInputText;
  c.alignment = liveSettings->alignment;
  c.flapSpeed = liveSettings->flapSpeed;
  return c;
}

// --- MQTT command setters (#224) --------------------------------------------
// mqttTask applies validated HA commands through these: same mutex, same
// NVS write-through as the settings drain. Values arrive pre-validated by
// the MqttHelpers parsers; the emptiness guards are belt-and-braces. Each
// returns true when the value actually changed (the caller logs only then;
// v1 parity: an HA echo of the current value is a silent no-op).

static bool webStateReady() {
  return webStateMutex != nullptr && liveSettings != nullptr &&
         liveStore != nullptr;
}

bool webMqttApplyMode(const String& mode) {
  if (!webStateReady() || mode.length() == 0) return false;
  WebStateLock lock;
  if (liveSettings->deviceMode == mode) return false;
  liveSettings->deviceMode = mode;
  saveDeviceMode(*liveStore, mode);
  return true;
}

bool webMqttApplySpeed(int speed) {
  if (!webStateReady()) return false;
  WebStateLock lock;
  if (liveSettings->flapSpeed == speed) return false;
  liveSettings->flapSpeed = speed;
  saveFlapSpeed(*liveStore, speed);
  return true;
}

bool webMqttApplyAlignment(const String& alignment) {
  if (!webStateReady() || alignment.length() == 0) return false;
  WebStateLock lock;
  if (liveSettings->alignment == alignment) return false;
  liveSettings->alignment = alignment;
  saveAlignment(*liveStore, alignment);
  return true;
}

void webRequestReboot() {
  if (webStateMutex == nullptr) return;
  WebStateLock lock;
  pendingReboot = true;
  rebootRequestedAtMs = millis();
}

String webTimezoneSnapshot() {
  if (!webStateReady()) return String();
  WebStateLock lock;
  return liveSettings->timezonePosix;
}
