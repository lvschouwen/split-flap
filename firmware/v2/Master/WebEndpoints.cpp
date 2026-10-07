// Web endpoint core for the v2 master (#186) — shared state, init/loop
// lifecycle and the cross-task accessors. The route handlers live in the
// Web*.cpp family (#338 split): WebContent (assets/SSE), WebSettings
// (settings/wifi/mqtt-discover), WebSystem (diagnostics reads), WebFirmware
// (master OTA/rescue, row image), WebMaintenance (unit ops), WebWall (rows,
// pairing, unit jobs). Shared internals cross that family through
// WebEndpointsInternal.h ONLY.
//
// Async-context rule (v1 #150) carries over verbatim: handlers run in the
// async_tcp task. They may parse, validate, read state and respond — they
// must NOT mutate shared Strings, write NVS, or hold hardware buses.
// Mutating work is staged (pendingPost, pendingReboot) and drained from
// loop() via webEndpointsLoop().

#include "WebEndpoints.h"
#include "WallShow.h"
#include "WallState.h"
#include "WebEndpointsInternal.h"

#include "QuietPolicy.h"  // quietBlocksContent (#227)
#include "BootTrace.h"  // bootTraceJson (#504)
#include "NetLiveness.h"  // netLivenessJson (#501)
#include "CrashContext.h"  // crashCtxReportJson (#504)

#include <ESPAsyncWebServer.h>
#include <WiFi.h>

#include "BuildVersion.h"
#include "ClockPolicy.h"
#include "ClockService.h"
#include "FactorySlot.h"
#include "EventRecord.h"
#include "FlashLog.h"
#include "FollowerImageStore.h"
#include "HelpersSerialHandling.h"
#include "LanOrigin.h"  // lanCsrfRejectPost
#include "MqttService.h"
#include "OtaService.h"
#include "RebootCause.h"  // #432
#include "SettingsJson.h"
#include "SplitFlapProtocol.h"
#include "Tasks.h"
#include "UnitBus.h"  // the abort flag of Stop
#include "ReflashPlan.h"
#include "WebBodyLimitGuard.h"  // pre-auth body-size guard (#347)

// Staged mutations, owned here; drained by webEndpointsLoop(). External
// linkage across the Web*.cpp family (WebEndpointsInternal.h).
PendingSettingsPost pendingPost;
bool pendingReboot = false;
uint32_t rebootRequestedAtMs = 0;
String pendingRebootCause;  // #432: stamped to NVS at the drain
// #432: consumed once in webEndpointsInit (single-threaded boot), read-only
// afterwards — "" when this boot did not follow a stamped deliberate reboot.
static String bootRebootCause;
String pendingIntendedVersion;  // ?v= from /firmware/master (#190)
bool pendingIntendedVersionProvided = false;
String pendingRescueRev;  // ?v= from /firmware/rescue (#391)
bool pendingRescueRecord = false;

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
  return webResetReasonName((int)esp_reset_reason());
}

const char* webResetReasonName(int reason) {
  switch ((esp_reset_reason_t)reason) {
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
// the routes that WRITE inside onUpload (master OTA, rescue install, row
// image) must gate CSRF INLINE at index==0, before the first write, exactly
// as the ESP-01 row board does. Returns true when the upload is a forged
// cross-site POST; the caller marks its own per-request rejection state.
bool webUploadCsrfRejected(AsyncWebServerRequest* request) {
  bool hasOrigin = request->hasHeader("Origin");
  return lanCsrfRejectPost(true, hasOrigin,
                               hasOrigin ? request->header("Origin") : String());
}

// #313 CSRF gate: a mutating POST carrying a browser Origin that is not a LAN
// pane is cross-site forgery — 403 BEFORE the handler runs, so the whole
// class of mutating routes is covered without a per-route allowlist to
// drift. curl sends no Origin and passes; the board's own page sends a LAN
// origin and passes. No route answers another origin's read: the browser
// talks to this board only.
static AsyncMiddlewareFunction csrfMiddleware(
    [](AsyncWebServerRequest* request, ArMiddlewareNext next) {
      bool hasOrigin = request->hasHeader("Origin");
      if (lanCsrfRejectPost(request->method() == HTTP_POST, hasOrigin,
                            hasOrigin ? request->header("Origin") : String())) {
        request->send(403, "text/plain",
                      F("Cross-origin POST refused (CSRF guard)"));
        return;  // handler chain stops — next() is never called
      }
      next();
    });

AsyncMiddlewareFunction& webCsrfMiddleware() { return csrfMiddleware; }

// Gathers the full /settings JSON from the display snapshot, live settings and
// the OTA/MQTT services. Extracted so both GET /settings and the
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
  // #335 per-board vitals — runtime, no settings lock needed.
  f.heapBytes = ESP.getFreeHeap();
  f.rssiDbm = WiFi.RSSI();
  f.upSeconds = millis() / 1000;
  {
    WebStateLock lock;
    f.alignment = liveSettings->alignment;
    f.flapSpeed = String(liveSettings->flapSpeed);
    f.deviceMode = liveSettings->deviceMode;
    f.timezonePosix = liveSettings->timezonePosix;
    f.unitCountOverride = liveSettings->unitCountOverride;
    f.reflashOnBoot = liveSettings->reflashOnBoot;
    f.quiet = liveSettings->quiet;  // #227
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
  // Cached since boot (#391) — no sha256 on the async path.
  RescueSlotFacts rescue = rescueSlotCurrent();
  f.rescueRev = rescue.rev;
  f.rescueSlot = rescueSlotStateLabel(rescue.state);
  f.rescueSlotWarn = rescue.warn;
  f.lastResetReason = webResetReasonString();
  f.lastRebootCause = bootRebootCause;  // #432
  f.bootTrace = bootTraceJson();         // #504
  f.netLiveness = netLivenessJson();     // #501
  f.crashContext = crashCtxReportJson(); // #504
  return buildSettingsJson(f);
}

void webEndpointsInit(AsyncWebServer& server, MasterSettings& settings,
                      SettingsStore& store,
                      const String& effectiveDeviceName) {
  server.addMiddleware(&webCsrfMiddleware());
  // Pre-auth body-size guard (#347) — before any route so it wins the
  // first-match-wins scan for an oversized body.
  attachBodyLimitGuard(server);
  wallStateSetRowSettings(settings.timezonePosix, settings.reflashOnBoot);
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
  bootRebootCause = rebootCauseConsume();  // #432

  // Per-module route registration (#338). Cross-module order is not
  // semantic — the server matches per path+method, same-path method pairs
  // stay within one module, and onNotFound/middleware are setters.
  webContentRegister(server);
  webSettingsRegister(server);
  webSystemRegister(server);
  webFirmwareRegister(server);
  webMaintenanceRegister(server);
  webWallRegister(server);
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
  // lock, (3) rebootless TZ apply, (4) MQTT mDNS discovery (LWIP locks —
  // never nested inside webStateMutex), (5) flash-log + row-image flushes
  // (netTask is the sole flash writer), (6) the reboot itself last.
  webFirmwareLoop();

  // The whole drain holds the mutex: applySettingsPost mutates the same
  // `settings` Strings GET /settings snapshots from the async task, and its
  // NVS writes are legal under a mutex (unlike a portENTER_CRITICAL
  // section). Worst case a handler blocks a few ms behind a flash commit.
  bool rebootDue = false;
  bool timezoneChanged = false;
  bool rescueRecordDue = false;
  String rescueRevToRecord;
  String rebootCauseToStamp;  // #432: copied out under the lock
  {
    WebStateLock lock;
    if (pendingPost.pending) {
      // Display-bound fields are read off the post before apply resets it.
      String messageText = pendingPost.inputText;
      bool messageProvided = pendingPost.inputTextProvided;
      String transientText = pendingPost.transientText;
      long transientDwell = pendingPost.transientDwell;
      bool transientProvided = pendingPost.transientTextProvided;
      bool transientWall = pendingPost.transientWall;
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
      bool reflashOnBootBefore = settings.reflashOnBoot;
      bool quietBefore = settings.quiet;
      applySettingsPost(pendingPost, settings, store);
      timezoneChanged = settings.timezonePosix != timezoneBefore;

      // #289 dummy mode: push a changed override to displayTask and queue a
      // Probe so the width refolds now instead of at the next bus op.
      if (settings.unitCountOverride != unitCountBefore) {
        tasksSetUnitCountOverride(settings.unitCountOverride);
        displayEnqueue(makeProbeCommand());
      }
      // #412: push the brake to displayTask so a mid-session change lands
      // without a reboot — the gate is only read at boot, but the operator
      // sets this BEFORE rebooting into the campaign.
      if (settings.reflashOnBoot != reflashOnBootBefore) {
        tasksSetReflashOnBoot(settings.reflashOnBoot);
        wallStateSetRowSettings(settings.timezonePosix, settings.reflashOnBoot);
      }
      if (settings.quiet != quietBefore) {
        tasksSetQuiet(settings.quiet);  // #227
        SerialPrintln(settings.quiet ? F("quiet: ON (web) — no flap commands")
                                     : F("quiet: OFF (web)"));
      }
      // #227: content that arrives while quiet is dropped, not queued. A
      // quiet toggle riding the same POST has been applied above, so "wake
      // and show this" works in one request.
      if (quietBlocksContent(settings.quiet, false) &&
          (messageProvided || transientProvided)) {
        SerialPrintln(F("web: text dropped (quiet)"));
        messageProvided = false;
        transientProvided = false;
      }

      // Explicit mode switch or message send trumps a running notification
      // (v1 #130 rule) — cancel so the next 1 Hz tick (or the direct
      // enqueue below) flaps the new content, not the overlay. Reads the
      // pre-apply capture: applySettingsPost() just reset the post's
      // provided flags (#219 fix — the reset made this condition dead for
      // mode switches). Skipped when a transient rides the same POST: its
      // overlay arm below supersedes the old one anyway, and a staged
      // cancel draining one tick earlier than the arm would drop the
      // clockTask gate for a one-frame stray re-show.
      if ((modeProvided || messageProvided) && !transientProvided) {
        mqttCancelNotification();
      }

      // v1 parity: a posted message only takes effect in text mode, checked
      // AFTER any mode field riding the same POST. In clock mode it is
      // silently ignored — never shown, never retained.
      if (messageProvided && settings.deviceMode == "text") {
        // Retained in the display domain: ClockPolicy's dedup compares this
        // against snapshot text, which makeShowTextCommand truncates. A
        // master with row boards retains untruncated — the grid holds more
        // than one row's width, and its ticker path never enters that dedup.
        currentInputText = wallShowActive()
                               ? messageText
                               : truncateForDisplay(messageText);
        // Reflash gate re-check at drain time (#205, Codex review): the
        // handler's 409 ran when the POST arrived; a job that started in
        // between must not get a ShowText queued behind it. Dropping is
        // self-healing — the retained text above is what the 1 Hz mode
        // ticker re-shows once the job ends.
        if (reflashInProgress(displaySnapshotGet().reflash) || wallUnitUpdateRunning()) {
          SerialPrintln("Message retained, not queued (reflash): " +
                        messageText);
        } else if (wallShowActive()) {
          // A wall with row boards (#566): the logical text goes to the wall.
          wallShowText(messageText, settings.alignment, settings.flapSpeed);
          SerialPrintln("Message routed to the wall: " + messageText);
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
      // display. With row boards the form's transient stays deliberately
      // local (a calibration pattern for this board's row: the overlay shows
      // on the own row only, and the wall's own-row re-show restores the
      // segment after the dwell); the `show` action's goes to every row, and
      // the clock ticker, held back for the dwell, hands the wall its content
      // again afterwards. Ordering matters twice: after applySettingsPost so an
      // alignment/speed change riding the same POST applies to this show,
      // and the overlay arm (which drains behind the #130 cancel above)
      // keeps the transient alive when that same POST also switched mode.
      if (transientProvided) {
        if (reflashInProgress(displaySnapshotGet().reflash) || wallUnitUpdateRunning()) {
          SerialPrintln("Transient text dropped (reflash running): " +
                        transientText);
        } else if (transientWall && wallShowActive()) {
          wallShowText(transientText, settings.alignment, settings.flapSpeed);
          mqttStartNotificationDwell(transientDwell);
          SerialPrintln("Transient text routed to the wall: " + transientText);
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
    // #391: only CAPTURE here. The record costs an NVS write plus a ~60 ms
    // sha256 over the rescue image — far longer than the "few ms behind a
    // flash commit" this lock budgets for, and it would stall every async
    // handler. Runs below,
    // outside the lock, like rebootDue.
    if (pendingRescueRecord) {
      pendingRescueRecord = false;
      rescueRecordDue = true;
      rescueRevToRecord = pendingRescueRev;
    }
    // Small grace period so the HTTP response flushes before the restart.
    rebootDue = pendingReboot && millis() - rebootRequestedAtMs > 750;
    if (rebootDue) rebootCauseToStamp = pendingRebootCause;
  }

  // #391: rescue-slot record — NVS write + sha256, on loopTask and outside
  // the WebState lock. An empty rev clears the record rather than storing a
  // placeholder that would read as a false STALE.
  if (rescueRecordDue) {
    rescueSlotRecordInstall(rescueRevToRecord);
  }

  // Outside the lock: configTzTime takes the LWIP core lock — keep the two
  // lock domains from ever nesting (v1 #48 parity: TZ applies rebootless).
  if (timezoneChanged) {
    clockServiceApplyTz(settings);
    wallStateSetRowSettings(settings.timezonePosix, settings.reflashOnBoot);
  }

  // mDNS discovery drain (#224 MQTT): blocking queries take LWIP locks, so
  // they run out here in netTask, outside webStateMutex.
  webSettingsDiscoverLoop();

  // Flash-log drain (#206): netTask is the single flash writer.
  flashLogTick(rebootDue);  // force on reboot so the last lines land
  eventRecordTick(rebootDue);  // #570: the same writer, the same rule
  // Staged row-image write (#304): same single-writer discipline — the
  // async upload handler accumulates in PSRAM, netTask commits it to flash.
  followerImageFlushTick();
  // Staged coredump-partition purge (#431), same discipline.
  webSystemCoredumpEraseTick();

  if (rebootDue) {
    // #432: durable cause — the flash-log ring above only holds hours.
    rebootCauseStamp(rebootCauseToStamp.length()
                         ? rebootCauseToStamp
                         : String(F("reboot requested (unattributed)")));
    SerialPrintln(F("Rebooting..."));
    Serial.flush();
    flashLogTick(true);  // catch the reboot line itself
    ESP.restart();
  }
}

WebStage webStagePost(const PendingSettingsPost& local, bool& needsReboot,
                      bool& deviceNameChanged) {
  // The reflash gate (#205) applies only to the display-bound part — pure
  // settings saves don't touch the display queue and stay allowed.
  const bool text = local.inputTextProvided || local.transientTextProvided;
  if (text && (reflashInProgress(displaySnapshotGet().reflash) || wallUnitUpdateRunning())) {
    return WebStage::UnitUpdate;
  }
  // Message/transient sends become display commands at drain time; report
  // a full queue now instead of accepting one that would be dropped.
  if (text && displayQueueFull()) return WebStage::QueueFull;
  {
    // Verdict + merge sit in one locked section so the comparison can't race
    // a half-applied post.
    WebStateLock lock;
    needsReboot = settingsPostNeedsReboot(local, *liveSettings);
    deviceNameChanged = local.deviceNameProvided && local.deviceName != liveSettings->deviceName;
    mergeSettingsPost(pendingPost, local);
  }
  // Device renamed (#125): flag only from async context — mqttTask blanks
  // the old identity's retained discovery configs before the reboot swaps
  // identities.
  if (deviceNameChanged) mqttRequestDiscoveryClear();
  return WebStage::Staged;
}

const char* webStageReboot(const char* cause) {
  // #395: a reboot mid-unit-reflash leaves the Nano row parked in twiboot;
  // mid-master-OTA it tears the upload session. /stop remains the only
  // cancel path.
  if (reflashInProgress(displaySnapshotGet().reflash) || wallUnitUpdateRunning()) {
    return "a unit update is running, retry when it has finished";
  }
  if (webFirmwareOtaUploadActive()) {
    return "a firmware upload is running, retry when it has finished "
           "(a stalled one clears in 30 s)";
  }
  webRequestReboot(cause);
  return nullptr;
}

bool webStopWall(uint32_t& seq) {
  // Order is load-bearing: the abort flag is set BEFORE the enqueue so the
  // queue's happens-before guarantees displayTask's Stop always finds it
  // set — set-after-enqueue races an idle displayTask clearing it first,
  // stranding the flag ON for every future wait. A full queue rolls the flag
  // back (nothing queued to abort).
  DisplayCommand cmd = makeStopCommand(displayNextMaintSeq());
  unitBusRequestAbort();
  if (!displayEnqueue(cmd)) {
    unitBusClearAbort();
    return false;
  }
  // Stop blanks the WHOLE wall: the command above handles this board's own
  // row, this the row boards (a no-op without any).
  wallShowBlank();
  seq = cmd.seq;
  return true;
}

bool webQuietNow() {
  if (webStateMutex == nullptr || liveSettings == nullptr) return false;
  WebStateLock lock;
  return liveSettings->quiet;
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

// #227: the HA Quiet switch. Same write-through as a mode change.
bool webMqttApplyQuiet(bool quiet) {
  if (!webStateReady()) return false;
  WebStateLock lock;
  if (liveSettings->quiet == quiet) return false;
  liveSettings->quiet = quiet;
  saveQuiet(*liveStore, quiet);
  tasksSetQuiet(quiet);
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

void webRequestReboot(const char* cause) {
  if (webStateMutex == nullptr) return;
  WebStateLock lock;
  pendingReboot = true;
  pendingRebootCause = cause;
  rebootRequestedAtMs = millis();
}

String webTimezoneSnapshot() {
  if (!webStateReady()) return String();
  WebStateLock lock;
  return liveSettings->timezonePosix;
}
