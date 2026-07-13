// Web endpoint registration for the v2 master (#186) — port of v1's
// ServiceWebEndpoints.ino onto the ESP32Async server stack.
//
// Async-context rule (v1 #150) carries over verbatim: handlers run in the
// async_tcp task. They may parse, validate, read state and respond — they
// must NOT mutate shared Strings, write NVS, or hold hardware buses.
// Mutating work is staged (pendingPost, pendingReboot) and drained from
// loop() via webEndpointsLoop().

#include "WebEndpoints.h"

#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <memory>

#include "BuildVersion.h"
#include "ClockPolicy.h"
#include "ClockService.h"
#include "ClusterFollower.h"
#include "ClusterLayout.h"  // CLUSTER_MAX_MEMBERS / CLUSTER_HOST_MAX_LEN
#include "ClusterLeader.h"
#include "DisplayEvents.h"
#include "FactorySlot.h"
#include "FlashLog.h"
#include "HelpersSerialHandling.h"
#include "MaintenancePolicy.h"
#include "OtaService.h"
#include "PendingSettingsPost.h"
#include "SettingsJson.h"
#include "MdnsDiscovery.h"
#include "MqttService.h"
#include "SplitFlapProtocol.h"
#include "SystemStats.h"
#include "SystemStatsPolicy.h"
#include "Tasks.h"
#include "UnitBus.h"
#include "WearPolicy.h"
#include "WebAssets.h"
#include "WebLog.h"
#include "WifiService.h"

#include <ESPmDNS.h>

// SSE display push (#251): netTask sends a "display" event the moment the
// snapshot's text changes (webDisplayEventsTick below); onConnect hands a
// fresh client the current state so it paints without waiting for a
// change. All send-side calls come from netTask; the esp32async event
// source queues per-client internally.
static AsyncEventSource sseEvents("/events");

// Staged mutations, owned here; drained by webEndpointsLoop().
static PendingSettingsPost pendingPost;
static bool pendingReboot = false;
static uint32_t rebootRequestedAtMs = 0;
static String pendingIntendedVersion;  // ?v= from /firmware/master (#190)
static bool pendingIntendedVersionProvided = false;

// OTA upload rejection state (v1 ServiceBootModes pattern): the upload
// callback can't respond, so it records the rejection and the completion
// callback sends it. Both callbacks run in the async_tcp task — same task,
// no cross-task race, no mutex (uploads are serialized by the TCP stream).
static int otaRejectionStatus = 0;  // 0 = not rejected
static String otaRejectionReason;

// Concurrent-upload guard (#191): the request that owns the live Update
// session. A second overlapping POST must not abort/hijack it — the
// overlapping request is marked rejected via its _tempObject (freed by the
// request destructor) and answered 409, and it never touches the shared
// rejection state above, which belongs to the owner. Same async_tcp task
// for all callbacks — no lock needed. Compared only, never dereferenced.
static AsyncWebServerRequest* otaOwnerRequest = nullptr;

// Upload throughput measurement (#248): decides whether flash erase or the
// network dominates OTA wall time before any speed work is designed. Owned
// by the async_tcp task like the session state above.
static uint32_t otaUploadStartMs = 0;

// Same pattern for POST /firmware/rescue (#195). Separate state on purpose:
// a rescue install and a master OTA are different flows and must not read
// each other's leftovers. (Concurrent uploads remain #191 territory.)
static int rescueRejectionStatus = 0;
static String rescueRejectionReason;

// Live state the read handlers render. Set once in webEndpointsInit();
// handlers only ever read (async-context rule). Held as TU-local statics
// rather than lambda-captured references so a handler can never outlive
// what it captured.
static MasterSettings* liveSettings = nullptr;
static SettingsStore* liveStore = nullptr;  // MQTT setters persist through it
static String effectiveName;

// MQTT broker mDNS discovery staging (#224, v1 /mqtt/discover contract):
// the POST arms the flag, netTask's drain runs the blocking query and
// caches the JSON, the GET answers 202 while pending / 200 from the cache.
static bool mqttDiscoverPending = false;
static String mqttDiscoverResultJson;

// Runtime-only message state (#192, v1 parity: never persisted, "" at
// boot). Written by the drain, read by GET /settings and the clock ticker's
// webDisplayContentSnapshot() — all under webStateMutex.
static String currentInputText;
static String lastMessageStamp;

// Cross-task guard. Unlike v1's single-core cooperative ESP8266, the
// handlers here run in the async_tcp FreeRTOS task while the drain runs in
// loopTask — Arduino Strings shared between them (pendingPost, *liveSettings,
// lastWrittenText) need real synchronization or a reader can see a buffer
// mid-free. A mutex (not a spinlock) on purpose: the critical sections
// allocate Strings and the drain writes NVS, neither of which is allowed
// inside portENTER_CRITICAL.
static SemaphoreHandle_t webStateMutex = nullptr;

struct WebStateLock {
  WebStateLock() { xSemaphoreTake(webStateMutex, portMAX_DELAY); }
  ~WebStateLock() { xSemaphoreGive(webStateMutex); }
  WebStateLock(const WebStateLock&) = delete;
  WebStateLock& operator=(const WebStateLock&) = delete;
};

// Serves a gzipped PROGMEM asset (v1 #159). Cache policy: PROGMEM assets
// only change via a master OTA, so the browser must revalidate every
// navigation or an OTA that swaps the UI would leave stale HTML/JS behind.
static void serveGzipAsset(AsyncWebServerRequest* request,
                           const char* contentType, const uint8_t* asset,
                           size_t assetLen) {
  AsyncWebServerResponse* resp =
      request->beginResponse(200, contentType, asset, assetLen);
  resp->addHeader("Content-Encoding", "gzip");
  resp->addHeader("Cache-Control", "no-cache");
  request->send(resp);
}

// --- maintenance endpoint helpers (#204) ---------------------------------------

// v1 parseCalibrationAddress as a seam: policy verdicts come from the pure
// MaintenancePolicy.h, this only translates request → verdict → response.
// Validates against the caller's snapshot COPY — a fast, possibly stale
// view; displayTask re-runs whatever check the queue delay can invalidate.
static bool maintCheckAddress(AsyncWebServerRequest* request,
                              const DisplaySnapshot& snap, int& outAddr) {
  String raw;
  bool provided = request->hasParam("address");
  if (provided) raw = request->getParam("address")->value();
  MaintVerdict verdict = maintValidateAddress(provided ? raw.c_str() : nullptr,
                                              snap.units, UNITS_AMOUNT,
                                              outAddr);
  if (verdict.httpStatus != 200) {
    request->send(verdict.httpStatus, "text/plain", verdict.message);
    return false;
  }
  return true;
}

// Required numeric query param (strtol base 0, v1 hex support). Sends the
// 400 itself so callers just early-return.
static bool maintRequireLongParam(AsyncWebServerRequest* request,
                                  const char* name, long& out) {
  if (!request->hasParam(name)) {
    String msg = "Missing '";
    msg += name;
    msg += "' query param";
    request->send(400, "text/plain", msg);
    return false;
  }
  String raw = request->getParam(name)->value();
  char* end = nullptr;
  out = strtol(raw.c_str(), &end, 0);
  if (end == raw.c_str()) {
    String msg = "'";
    msg += name;
    msg += "' must be a number";
    request->send(400, "text/plain", msg);
    return false;
  }
  return true;
}

// Producer gate (#205): while a reflash job runs, display-mutating POSTs
// answer 409 instead of enqueueing — nothing piles up behind the job to
// burst-drain afterwards. /stop is the ONE exception (it is the cancel)
// and deliberately does not call this.
static bool rejectWhileReflashing(AsyncWebServerRequest* request) {
  if (!reflashInProgress(displaySnapshotGet().reflash)) return false;
  request->send(409, "text/plain",
                F("Unit reflash in progress — retry when it finishes"));
  return true;
}

// Enqueue-or-503 tail shared by every maintenance POST: the client either
// gets its correlation seq or an honest busy — never a silently dropped op.
// Every caller except /stop funnels through here, so the reflash gate
// lives here too.
static void maintEnqueue(AsyncWebServerRequest* request,
                         const DisplayCommand& cmd) {
  if (rejectWhileReflashing(request)) return;
  if (!displayEnqueue(cmd)) {
    request->send(503, "text/plain",
                  F("Display queue full — try again in a moment"));
    return;
  }
  char buf[24];
  snprintf(buf, sizeof(buf), "{\"seq\":%lu}", (unsigned long)cmd.seq);
  request->send(200, "application/json", buf);
}

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

void webEndpointsInit(AsyncWebServer& server, MasterSettings& settings,
                      SettingsStore& store,
                      const String& effectiveDeviceName) {
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

  // --- SSE (#251) ----------------------------------------------------------
  // A fresh client gets the current display text immediately; every later
  // change is pushed by webDisplayEventsTick() from netTask.
  sseEvents.onConnect([](AsyncEventSourceClient* client) {
    client->send(
        buildDisplayEventJson(displaySnapshotGet().currentText).c_str(),
        "display", millis());
  });
  server.addHandler(&sseEvents);

  // --- static assets, all from PROGMEM (generated by build_assets.py) -----
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    serveGzipAsset(request, "text/html", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
  });
  server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest* request) {
    serveGzipAsset(request, "text/html", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
  });
  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest* request) {
    serveGzipAsset(request, "application/javascript", SCRIPT_JS_GZ,
                   SCRIPT_JS_GZ_LEN);
  });
  server.on("/md5.js", HTTP_GET, [](AsyncWebServerRequest* request) {
    serveGzipAsset(request, "application/javascript", MD5_JS_GZ, MD5_JS_GZ_LEN);
  });
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest* request) {
    serveGzipAsset(request, "text/css", STYLE_CSS_GZ, STYLE_CSS_GZ_LEN);
  });
  server.on("/favicon.png", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(request->beginResponse(200, "image/png", FAVICON_PNG,
                                         FAVICON_PNG_LEN));
  });
  // Full IANA→POSIX timezone table (#252), baked from the vendored
  // posix_tz_db zones.csv; the Settings type-ahead fetches it lazily.
  server.on("/tz.json", HTTP_GET, [](AsyncWebServerRequest* request) {
    serveGzipAsset(request, "application/json", TZ_JSON_GZ, TZ_JSON_GZ_LEN);
  });

  // --- reads ---------------------------------------------------------------
  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest* request) {
    SettingsJsonFields f;
    // Bus facts from the display task's snapshot copy (#203) — same values
    // v1 kept in the probe globals, same /settings wire shape.
    DisplaySnapshot snap = displaySnapshotGet();
    int addrs[UNITS_AMOUNT];
    int fwStatus[UNITS_AMOUNT];
    String versions[UNITS_AMOUNT];
    int detected = 0;
    for (int i = 0; i < UNITS_AMOUNT; i++) {
      // Same state != 0 predicate countRespondingUnits() derived
      // snap.detectedUnitCount from — `detected` only orders the addresses.
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
    {
      // Snapshot the shared Strings under the lock; serialize outside it.
      WebStateLock lock;
      f.alignment = liveSettings->alignment;
      f.flapSpeed = String(liveSettings->flapSpeed);
      f.deviceMode = liveSettings->deviceMode;
      f.timezonePosix = liveSettings->timezonePosix;
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
    // Display state comes from the display task's snapshot (#187), not from
    // web-side shadows — the display domain owns what's on the flaps.
    f.lastWrittenText = String(snap.currentText);
    f.mqttConnected = mqttIsConnected();
    f.version = GIT_REV;
    f.sketchMd5 = ESP.getSketchMD5();
    // Verdict synthesized from esp_ota partition state (#190) — v1 wire
    // vocabulary plus "pending" while the health confirm hasn't run yet.
    OtaVerdict verdict = otaVerdictSnapshot();
    f.lastFlashResult = verdict.lastFlashResult;
    f.otaReverted = verdict.otaReverted;
    f.lastResetReason = webResetReasonString();
    // Cluster membership (#272): drives the follower banner + card gating.
    ClusterFollowerView cluster = clusterFollowerViewGet();
    f.clusterState = clusterFollowerPhaseName(cluster.phase);
    f.clusterLeaderName = cluster.leaderName;
    f.clusterLeaderHost = cluster.leaderHost;
    f.clusterRow = cluster.row;
    request->send(200, "application/json", buildSettingsJson(f));
  });

  server.on("/health", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "text/plain", "Healthy");
  });

  server.on("/log", HTTP_GET, [](AsyncWebServerRequest* request) {
    // Don't SerialPrintln here; every log request would otherwise stamp
    // itself into the buffer on every poll and drown out real activity.
    request->send(200, "text/plain", webLogRead());
  });

  // --- persistent flash log (#206) ------------------------------------------
  // Serves the LittleFS log files directly (chunked by the async layer;
  // esp_littlefs serializes fs access internally, and the flush path
  // open→append→closes so no write handle is ever shared). Known-benign
  // race: between the exists() check and the response's real open(), a
  // rotation or drained clear can remove the file — AsyncFileResponse then
  // degrades to a 404, so a request racing a rotate occasionally 404s;
  // retry. ?prev=1 = the rotated file. Content up to the last ~5 s flush.
  server.on("/log/flash", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!flashLogAvailable()) {
      request->send(503, "text/plain",
                    F("Flash log unavailable (storage mount failed)"));
      return;
    }
    const char* path = request->hasParam("prev") ? flashLogPreviousPath()
                                                 : flashLogCurrentPath();
    if (!LittleFS.exists(path)) {
      request->send(404, "text/plain", F("No flash log yet"));
      return;
    }
    request->send(LittleFS, path, "text/plain");
  });

  // Clear is staged and drained by netTask's flashLogTick — handlers never
  // write flash (async rule).
  server.on("/log/flash/clear", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!flashLogAvailable()) {
      request->send(503, "text/plain",
                    F("Flash log unavailable (storage mount failed)"));
      return;
    }
    flashLogRequestClear();
    request->send(202, "text/plain", F("Flash log clear queued"));
  });

  // --- settings/message form (v1 wire contract) ----------------------------
  server.on("/", HTTP_POST, [](AsyncWebServerRequest* request) {
    // All-or-nothing per POST: parse into a local post, merge only when
    // every provided field validated (v1 #128/#153 semantics, pure logic
    // in PendingSettingsPost.h — natively tested).
    PendingSettingsPost local;
    bool submissionError = false;

    int params = request->params();
    for (int i = 0; i < params; i++) {
      const AsyncWebParameter* p = request->getParam(i);
      if (!p->isPost()) continue;
      SettingsParamResult result =
          stageSettingsParam(local, p->name(), p->value());
      if (result == SettingsParamResult::Invalid) {
        SerialPrintln("Invalid value for '" + p->name() + "': " + p->value());
        submissionError = true;
      }
    }
    if (!settingsPostConsistent(local)) {
      SerialPrintln(F("Transient dwell provided without transient text."));
      submissionError = true;
    }

    // Per-card fetch() saves (#128) send ajax=1 and want a status code
    // instead of the classic redirect.
    bool isAjax = request->hasParam("ajax", true);

    if (submissionError) {
      if (isAjax) request->send(400, "text/plain", F("invalid"));
      else request->redirect("/?invalid-submission=true");
      return;
    }

    // Message/transient sends become display commands at drain time; report
    // a full queue now instead of accepting one that would be dropped. (The
    // stub worker drains instantly, so this only fires if something wedges.)
    // The reflash gate (#205) applies only to the display-bound part — pure
    // settings saves don't touch the display queue and stay allowed.
    if ((local.inputTextProvided || local.transientTextProvided) &&
        reflashInProgress(displaySnapshotGet().reflash)) {
      if (isAjax) {
        request->send(409, "text/plain", F("reflash in progress"));
      } else {
        request->redirect("/?display-busy=true");
      }
      return;
    }
    // Cluster producer gate (#272): a clustered follower's text/mode belong
    // to the leader — 409; the banner explains why. Transients stay allowed
    // (they are the calibration vehicle — maintenance is local), and so do
    // pure settings saves.
    if ((local.inputTextProvided || local.deviceModeProvided) &&
        clusterFollowerViewGet().gated) {
      if (isAjax) request->send(409, "text/plain", F("clustered"));
      else request->redirect("/?clustered=true");
      return;
    }
    if ((local.inputTextProvided || local.transientTextProvided) &&
        displayQueueFull()) {
      SerialPrintln(F("Display command queue full — message rejected."));
      if (isAjax) request->send(503, "text/plain", F("display busy"));
      else request->redirect("/?display-busy=true");
      return;
    }

    // Verdict against the live values, then stage; the apply itself runs in
    // webEndpointsLoop(). Verdict + merge sit in one locked section so the
    // comparison can't race a half-applied post.
    bool needsReboot;
    bool deviceNameChanged;
    {
      WebStateLock lock;
      needsReboot = settingsPostNeedsReboot(local, *liveSettings);
      deviceNameChanged = local.deviceNameProvided &&
                          local.deviceName != liveSettings->deviceName;
      mergeSettingsPost(pendingPost, local);
    }

    // Device renamed (#125): flag only from async context — mqttTask blanks
    // the old identity's retained discovery configs before the reboot swaps
    // identities.
    if (deviceNameChanged) mqttRequestDiscoveryClear();

    if (isAjax) {
      request->send(200, "text/plain", needsReboot ? F("ok-reboot") : F("ok"));
    } else {
      request->redirect(deviceNameChanged ? "/?device-name-saved=true"
                        : needsReboot     ? "/?mqtt-saved=true"
                                          : "/");
    }
  });

  // POST, not GET (v1 #145): state-changing actions must not be triggerable
  // by a drive-by <img src> on the LAN.
  server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest* request) {
    SerialPrintln(F("Reboot requested from web UI"));
    request->send(200, "text/plain",
                  "Reboot pending — this takes a few seconds. Reload the home "
                  "page afterwards.");
    WebStateLock lock;
    pendingReboot = true;
    rebootRequestedAtMs = millis();
  });

  // --- WiFi portal + credentials (#188) -------------------------------------
  // Handlers stage into WifiService; all radio/NVS work runs in netTask's
  // wifiServiceTick(). The portal page itself is served unconditionally —
  // from the LAN it doubles as a "move to another network" page.
  server.on("/wifi-setup", HTTP_GET, [](AsyncWebServerRequest* request) {
    serveGzipAsset(request, "text/html", PORTAL_HTML_GZ, PORTAL_HTML_GZ_LEN);
  });

  server.on("/wifi/scan", HTTP_POST, [](AsyncWebServerRequest* request) {
    wifiStageScan();
    request->send(200, "text/plain", F("scanning"));
  });
  server.on("/wifi/scan", HTTP_GET, [](AsyncWebServerRequest* request) {
    String json = wifiScanResultJson();
    if (json.length() == 0) {
      request->send(202, "text/plain", F("pending"));
    } else {
      request->send(200, "application/json", json);
    }
  });

  server.on("/wifi/config", HTTP_POST, [](AsyncWebServerRequest* request) {
    SerialPrintln(F("WiFi credentials submitted from web"));
    if (!request->hasParam("ssid", true)) {
      request->send(400, "text/plain", F("invalid"));
      return;
    }
    String ssid = request->getParam("ssid", true)->value();
    String pass = request->hasParam("pass", true)
                      ? request->getParam("pass", true)->value()
                      : String();
    if (!isValidWifiSsidValue(ssid, LEN_WIFI_SSID) ||
        !isValidWifiPasswordValue(pass, LEN_WIFI_PASSWORD)) {
      SerialPrintln(F("WiFi config rejected: invalid ssid/password"));
      request->send(400, "text/plain", F("invalid"));
      return;
    }
    wifiStagePortalConfig(ssid, pass);
    request->send(200, "text/plain", F("ok-reboot"));
  });

  // POST, not GET (v1 #145): erasing WiFi credentials is the sharpest edge.
  server.on("/reset-wifi", HTTP_POST, [](AsyncWebServerRequest* request) {
    SerialPrintln(F("WiFi credential erase requested from web UI"));
    request->send(200, "text/plain",
                  "WiFi credentials erased. The display is rebooting into its "
                  "setup portal — reconnect to the device's setup AP to "
                  "configure a network.");
    wifiStageReset();
  });

  // Captive-portal hook: while the setup portal is up, the DNS catch-all
  // funnels every hostname here and this redirect pops the OS sign-in sheet
  // (/generate_204, /hotspot-detect.html, /connecttest.txt all land in
  // onNotFound). Outside portal mode: a plain 404, as v1.
  server.onNotFound([](AsyncWebServerRequest* request) {
    String redirectUrl = wifiPortalRedirectUrl();  // "" = portal not up
    if (redirectUrl.length() > 0) {
      request->redirect(redirectUrl);
    } else {
      request->send(404, "text/plain", F("Not found"));
    }
  });

  // --- master OTA (#190) -----------------------------------------------------
  // v1 wire contract: POST multipart field "firmware" + mandatory ?md5=
  // (v1 #144) + optional ?v= intended-version diagnostic. Update targets
  // the inactive A/B slot; the image boots PENDING_VERIFY and OtaService
  // confirms it once a netif is up (bootloader reverts otherwise).
  //
  // Async-context exception (deliberate, v1 precedent): Update.write() runs
  // right here in the async_tcp task — a firmware stream cannot be staged
  // through a queue. The handler still never touches settings/NVS/radio;
  // the ?v= write and the reboot are staged for the netTask drain.
  server.on(
      "/firmware/master", HTTP_POST,
      [](AsyncWebServerRequest* request) {
        // Overlap-rejected upload (#191), or a bodyless POST racing a live
        // session: answer 409 without touching the owner's shared state,
        // MQTT freeze or Update session.
        if (request->_tempObject != nullptr ||
            (otaOwnerRequest != nullptr && otaOwnerRequest != request)) {
          request->send(409, "text/plain",
                        F("Another master OTA upload is already in progress "
                          "— retry when it finishes"));
          return;
        }
        otaOwnerRequest = nullptr;  // session concluded, whatever the verdict
        if (otaRejectionStatus != 0) {
          request->send(otaRejectionStatus, "text/plain", otaRejectionReason);
          return;
        }
        if (Update.hasError()) {
          mqttResumeAfterOta();  // no reboot coming — thaw the session (#116)
          request->send(500, "text/plain", String("Master OTA failed: ") +
                                               Update.errorString());
          return;
        }
        if (!Update.isFinished()) {
          mqttResumeAfterOta();  // no reboot coming — thaw the session (#116)
          request->send(500, "text/plain",
                        F("Master OTA incomplete: upload ended before the "
                          "image was complete."));
          return;
        }
        request->send(200, "text/plain",
                      F("Master firmware flashed; rebooting…"));
        WebStateLock lock;
        pendingReboot = true;
        rebootRequestedAtMs = millis();
      },
      [](AsyncWebServerRequest* request, String filename, size_t index,
         uint8_t* data, size_t len, bool final) {
        if (index == 0) {
          // Concurrent-upload guard (#191): a live session owns the Update
          // singleton and the shared rejection state — mark this request
          // rejected (per-request _tempObject; malloc pairs with the free
          // in the request destructor) and leave both alone.
          if (otaOwnerRequest != nullptr && otaOwnerRequest != request) {
            request->_tempObject = malloc(1);
            return;
          }
          otaRejectionStatus = 0;
          otaRejectionReason = "";

          // Reflash gate (#205): a master OTA reboots the S3 mid-unit-flash
          // and strands the in-flight unit in twiboot for no reason.
          if (reflashInProgress(displaySnapshotGet().reflash)) {
            otaRejectionStatus = 409;
            otaRejectionReason =
                "Unit reflash in progress — retry when it finishes";
            return;
          }

          String md5 = request->hasParam("md5")
                           ? request->getParam("md5")->value()
                           : String();
          if (md5.length() == 0) {
            otaRejectionStatus = 400;
            otaRejectionReason =
                "md5 query parameter is required (compute it over the .bin "
                "and pass ?md5=...)";
            return;
          }
          if (!normalizeOtaMd5(md5)) {
            otaRejectionStatus = 400;
            otaRejectionReason = "md5 must be exactly 32 hex characters";
            return;
          }

          SerialPrintln("Master OTA upload started (md5 " + md5 + ")");
          if (Update.isRunning()) {
            Update.abort();  // stale aborted upload must not wedge this one
                             // (v1 #162 re-entry class)
          }
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            otaRejectionStatus = 500;
            otaRejectionReason =
                String("Master OTA could not start: ") + Update.errorString();
            return;
          }
          Update.setMD5(md5.c_str());
          // Freeze MQTT for the flash writes (v1 #116): staged — mqttTask
          // force-closes the session, the broker fires the "offline" LWT.
          mqttStopForOta();

          // ?v= staged unconditionally (empty if absent) so a stale value
          // from an earlier flash can't outlive this one (v1 #52 rationale),
          // through the same validator as every other settings write (#191).
          String intended = sanitizeIntendedVersion(
              request->hasParam("v") ? request->getParam("v")->value()
                                     : String());
          {
            WebStateLock lock;
            pendingIntendedVersion = intended;
            pendingIntendedVersionProvided = true;
          }

          // Session is live from here (#191). onDisconnect is the backstop
          // for a client that dies mid-upload: free the slot; the stale
          // Update session is aborted by the next upload's begin path above.
          otaOwnerRequest = request;
          otaUploadStartMs = millis();
          request->onDisconnect([request]() {
            if (otaOwnerRequest == request) otaOwnerRequest = nullptr;
          });
        }

        // Not (or no longer) the live owner (#191): covers overlap-rejected
        // requests AND stragglers rejected via the shared flag above whose
        // client keeps streaming — a later legitimate owner resets that
        // flag, and without this gate their leftover chunks would write
        // into the new owner's session.
        if (otaOwnerRequest != request) return;
        if (otaRejectionStatus != 0) return;

        if (len > 0 && Update.write(data, len) != len) {
          // Error is latched inside Update; the completion callback
          // reports it. Stop consuming flash time on further chunks.
          return;
        }
        if (final) {
          if (Update.end(true)) {
            uint32_t elapsedMs = millis() - otaUploadStartMs;
            uint32_t totalBytes = (uint32_t)(index + len);
            if (elapsedMs == 0) elapsedMs = 1;
            SerialPrintf(
                "Master OTA received %u bytes in %u ms (%u KB/s)\n",
                (unsigned)totalBytes, (unsigned)elapsedMs,
                (unsigned)(((uint64_t)totalBytes * 1000ULL / 1024ULL) /
                           elapsedMs));
            SerialPrintln(F("Master OTA image verified and armed — reboot "
                            "boots it PENDING_VERIFY"));
          } else {
            SerialPrintln(String("Master OTA failed at end: ") +
                          Update.errorString());
          }
        }
      });

  server.on("/debug/ota", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", otaDebugJson());
  });

  // --- factory rescue slot (#195) --------------------------------------------
  // Install/refresh the rescue image over WiFi: same wire contract as
  // /firmware/master (multipart field "firmware" + mandatory ?md5=), target
  // = the factory partition via FactorySlot's raw esp_partition writes.
  // Never touches otadata — installing a rescue image does not change what
  // boots next. Same async-context exception as the master OTA above: the
  // firmware stream is written right here in the async_tcp task.
  server.on(
      "/firmware/rescue", HTTP_POST,
      [](AsyncWebServerRequest* request) {
        if (rescueRejectionStatus != 0) {
          request->send(rescueRejectionStatus, "text/plain",
                        rescueRejectionReason);
          return;
        }
        String err = factoryWriteError();
        if (err.length() > 0) {
          request->send(500, "text/plain", "Rescue install failed: " + err);
          return;
        }
        request->send(200, "text/plain",
                      F("Rescue image installed into the factory slot. No "
                        "reboot — POST /firmware/rescue-boot to test it."));
      },
      [](AsyncWebServerRequest* request, String filename, size_t index,
         uint8_t* data, size_t len, bool final) {
        if (index == 0) {
          // An install is already streaming: 409 without touching its state.
          // (The shared rejection flag stalls the in-flight upload too — the
          // erased header keeps that safe; clean per-request verdicts are
          // #191 territory.)
          if (factoryInstallInProgress()) {
            rescueRejectionStatus = 409;
            rescueRejectionReason =
                "another rescue install is in flight — let it finish (a "
                "dropped one expires after ~30 s) and retry";
            return;
          }
          rescueRejectionStatus = 0;
          rescueRejectionReason = "";

          String md5 = request->hasParam("md5")
                           ? request->getParam("md5")->value()
                           : String();
          if (md5.length() == 0) {
            rescueRejectionStatus = 400;
            rescueRejectionReason =
                "md5 query parameter is required (compute it over the .bin "
                "and pass ?md5=...)";
            return;
          }
          if (!normalizeOtaMd5(md5)) {
            rescueRejectionStatus = 400;
            rescueRejectionReason = "md5 must be exactly 32 hex characters";
            return;
          }
          if (!factoryWriteBegin(md5)) {
            rescueRejectionStatus = 500;
            rescueRejectionReason =
                "Rescue install could not start: " + factoryWriteError();
            return;
          }
        }

        if (rescueRejectionStatus != 0) return;

        if (len > 0 && !factoryWriteChunk(data, len, index)) {
          return;  // error latched in FactorySlot; completion reports it
        }
        if (final) {
          factoryWriteEnd();  // completion callback reads factoryWriteError()
        }
      });

  // Software entry into the rescue image — and the periodic "prove the
  // rescue image still boots" test (#195 spec). Guarded so a wall-mounted
  // device can't be pointed at an empty slot (the bootloader would fall
  // back to an OTA slot anyway, but the endpoint must stay honest as a
  // boot test). otadata erase + staged reboot; NVS untouched, so WiFi
  // credentials survive into rescue (#193 invariant).
  server.on("/firmware/rescue-boot", HTTP_POST,
            [](AsyncWebServerRequest* request) {
              if (factoryInstallInProgress()) {
                request->send(409, "text/plain",
                              F("A rescue image install is in flight — let "
                                "it finish, then retry."));
                return;
              }
              if (!factorySlotImageValid()) {
                request->send(409, "text/plain",
                              F("Factory slot holds no valid rescue image — "
                                "POST it to /firmware/rescue first."));
                return;
              }
              if (!rescueBootArm()) {
                request->send(500, "text/plain",
                              F("otadata erase failed — rescue boot not "
                                "armed."));
                return;
              }
              request->send(200, "text/plain",
                            F("Rebooting into the rescue image… it joins "
                              "WiFi (or opens <name>-rescue) and serves the "
                              "recovery page."));
              WebStateLock lock;
              pendingReboot = true;
              rebootRequestedAtMs = millis();
            });

  // --- unit health (#203, v1 #45 wire contract) -----------------------------
  // GET renders JSON from the snapshot copy — never touches the bus from
  // async context (the async rule is structural here: only displayTask
  // holds Wire).
  // System tab (#245): current vitals + ~10 min sparkline history in one
  // JSON. History is server-side (netTask's sample ring) so a freshly
  // opened tab has depth immediately; the browser polls at 2 s.
  server.on("/system/stats", HTTP_GET, [](AsyncWebServerRequest* request) {
    std::unique_ptr<char[]> buf(new char[SYSTEM_STATS_JSON_CAP]);
    size_t n = systemStatsJson(buf.get(), SYSTEM_STATS_JSON_CAP);
    if (n == 0 || n >= SYSTEM_STATS_JSON_CAP) {
      request->send(500, "text/plain", F("stats unavailable"));
      return;
    }
    request->send(200, "application/json", buf.get());
  });

  server.on("/units/health", HTTP_GET, [](AsyncWebServerRequest* request) {
    DisplaySnapshot snap = displaySnapshotGet();
    // Heap, not stack: ~2 KB doesn't belong on the async_tcp task stack,
    // and a static buffer would race concurrent requests.
    std::unique_ptr<char[]> buf(new char[UNIT_HEALTH_JSON_CAP]);
    size_t n =
        buildUnitHealthJson(buf.get(), UNIT_HEALTH_JSON_CAP, snap.units,
                            snap.displayWidth, snap.faultyUnitCount,
                            SFP_I2C_ADDRESS_BASE);
    if (n == 0 || n >= UNIT_HEALTH_JSON_CAP) {
      // Would-be-truncated payload: fall back to a valid headline-only JSON
      // rather than shipping a cut object (v1 truncation discipline).
      n = (size_t)snprintf(buf.get(), UNIT_HEALTH_JSON_CAP,
                           "{\"width\":%d,\"faulty\":%d,\"units\":[]}",
                           snap.displayWidth, snap.faultyUnitCount);
    }
    // Wear assessment rides the same payload (#231, additive key), spliced
    // before the closing brace like the reflash object below.
    WearAssessment wear;
    assessWear(snap.units, snap.displayWidth, wear);
    char wearJson[96];
    size_t wearLen = buildWearJson(wear, wearJson, sizeof(wearJson));
    if (n > 0 && wearLen < sizeof(wearJson) &&
        n + wearLen + 2 < UNIT_HEALTH_JSON_CAP) {
      n += (size_t)snprintf(buf.get() + n - 1, UNIT_HEALTH_JSON_CAP - n + 1,
                            ",%s}", wearJson) - 1;
    }
    // Reflash progress rides the same payload (#205, additive key — the
    // Maintenance tab already polls this endpoint). Spliced before the
    // closing brace; the ~70 B worst case fits the cap's slack by design.
    char reflashJson[80];
    buildReflashJson(reflashJson, sizeof(reflashJson), snap.reflash);
    if (n > 0 && n + strlen(reflashJson) + 13 < UNIT_HEALTH_JSON_CAP) {
      snprintf(buf.get() + n - 1, UNIT_HEALTH_JSON_CAP - n + 1,
               ",\"reflash\":%s}", reflashJson);
    }
    request->send(200, "application/json", buf.get());
  });
  // POST enqueues a Probe — displayTask re-scans the bus AND re-polls
  // health in one pass (a probe subsumes v1's plain re-poll; ?probe=1 is
  // accepted for wire compat but changes nothing). 202 mirrors v1's
  // "pending"; a full queue reports busy like v1's flash-in-progress 503.
  server.on("/units/health/refresh", HTTP_POST,
            [](AsyncWebServerRequest* request) {
              if (rejectWhileReflashing(request)) return;
              if (!displayEnqueue(makeProbeCommand())) {
                request->send(503, "application/json",
                              F("{\"status\":\"busy\"}"));
                return;
              }
              request->send(202, "application/json",
                            F("{\"status\":\"pending\"}"));
            });

  // --- calibration + provisioning (#204, queue-native) ----------------------
  // Handlers validate against a snapshot copy, enqueue with a fresh seq and
  // answer {"seq":N}; execution truth arrives via GET /unit/op-result plus
  // the refreshed health facts. The async rule is upheld throughout: no
  // handler touches the bus or waits on display work.

  server.on("/unit/offset", HTTP_GET, [](AsyncWebServerRequest* request) {
    DisplaySnapshot snap = displaySnapshotGet();
    int addr = 0;
    if (!maintCheckAddress(request, snap, addr)) return;
    const UnitFacts& unit = snap.units[addr - SFP_I2C_ADDRESS_BASE];
    if (!unit.offsetValid) {
      // v1 wording (pre-#32 unit firmware has no GET_OFFSET); also covers
      // "reads invalidated until the next probe".
      request->send(502, "text/plain",
                    F("Unit did not return a valid offset (firmware may "
                      "predate #32)"));
      return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"offset\":%d}", (int)unit.offset);
    request->send(200, "application/json", buf);
  });

  server.on("/unit/offset", HTTP_POST, [](AsyncWebServerRequest* request) {
    DisplaySnapshot snap = displaySnapshotGet();
    int addr = 0;
    if (!maintCheckAddress(request, snap, addr)) return;
    long value = 0;
    if (!maintRequireLongParam(request, "value", value)) return;
    MaintVerdict verdict = maintValidateOffset(value);
    if (verdict.httpStatus != 200) {
      request->send(verdict.httpStatus, "text/plain", verdict.message);
      return;
    }
    maintEnqueue(request, makeWriteOffsetCommand(displayNextMaintSeq(),
                                                 (uint8_t)addr,
                                                 (int16_t)value));
  });

  server.on("/unit/jog", HTTP_POST, [](AsyncWebServerRequest* request) {
    DisplaySnapshot snap = displaySnapshotGet();
    int addr = 0;
    if (!maintCheckAddress(request, snap, addr)) return;
    long steps = 0;
    if (!maintRequireLongParam(request, "steps", steps)) return;
    MaintVerdict verdict = maintValidateJog(steps);
    if (verdict.httpStatus != 200) {
      request->send(verdict.httpStatus, "text/plain", verdict.message);
      return;
    }
    maintEnqueue(request, makeJogCommand(displayNextMaintSeq(), (uint8_t)addr,
                                         (int)steps));
  });

  server.on("/unit/home", HTTP_POST, [](AsyncWebServerRequest* request) {
    DisplaySnapshot snap = displaySnapshotGet();
    int addr = 0;
    if (!maintCheckAddress(request, snap, addr)) return;
    maintEnqueue(request,
                 makeHomeCommand(displayNextMaintSeq(), (uint8_t)addr));
  });

  server.on("/unit/identify", HTTP_POST, [](AsyncWebServerRequest* request) {
    DisplaySnapshot snap = displaySnapshotGet();
    int addr = 0;
    if (!maintCheckAddress(request, snap, addr)) return;
    maintEnqueue(request,
                 makeIdentifyCommand(displayNextMaintSeq(), (uint8_t)addr));
  });

  // Physical-rebuild bookkeeping (#231): zero the unit's wear odometer after
  // a flap swap or motor replacement. Same op contract as identify/home.
  server.on("/unit/reset-odometer", HTTP_POST,
            [](AsyncWebServerRequest* request) {
    DisplaySnapshot snap = displaySnapshotGet();
    int addr = 0;
    if (!maintCheckAddress(request, snap, addr)) return;
    maintEnqueue(request, makeResetOdometerCommand(displayNextMaintSeq(),
                                                   (uint8_t)addr));
  });

  // On-demand unit self-test (#265): the unit measures its own mechanics
  // (steps/rev, hall window, rev time) over ~2 revolutions. Same op
  // contract as identify/home; the measurements come back via
  // GET /unit/self-test-result.
  server.on("/unit/self-test", HTTP_POST, [](AsyncWebServerRequest* request) {
    DisplaySnapshot snap = displaySnapshotGet();
    int addr = 0;
    if (!maintCheckAddress(request, snap, addr)) return;
    maintEnqueue(request,
                 makeSelfTestCommand(displayNextMaintSeq(), (uint8_t)addr));
  });

  // Self-test execution feedback: pending / ok(+measurements) /
  // failed(+reason) / expired, rendered from the snapshot's single
  // SelfTestSlot — the self-test twin of /unit/op-result.
  server.on("/unit/self-test-result", HTTP_GET,
            [](AsyncWebServerRequest* request) {
    long seq = 0;
    if (!maintRequireLongParam(request, "seq", seq)) return;
    if (seq < 1) {
      request->send(400, "text/plain", F("seq must be >= 1"));
      return;
    }
    DisplaySnapshot snap = displaySnapshotGet();
    char buf[128];
    buildSelfTestJson(buf, sizeof(buf), snap.lastSelfTest, (uint32_t)seq);
    request->send(200, "application/json", buf);
  });

  // Debug endpoint, v1 semantics preserved: pushes the unit into twiboot
  // (~1 s on its DIP-derived address, then back to the sketch). v1 parity:
  // range check only, no sketch-state gate — it exists precisely for poking
  // at units the probe view might mislabel. displayTask deliberately does
  // NOT reprobe afterwards (v1 #88: probing the twiboot window pins the
  // bootloader alive).
  server.on("/unit/reboot", HTTP_POST, [](AsyncWebServerRequest* request) {
    long addr = 0;
    if (!maintRequireLongParam(request, "address", addr)) return;
    if (addr < 1 || addr > 126) {
      request->send(400, "text/plain", F("Address must be 1..126"));
      return;
    }
    maintEnqueue(request, makeRebootToBootloaderCommand(displayNextMaintSeq(),
                                                        (uint8_t)addr));
  });

  server.on("/unit/set-address", HTTP_POST,
            [](AsyncWebServerRequest* request) {
              DisplaySnapshot snap = displaySnapshotGet();
              int addr = 0;
              if (!maintCheckAddress(request, snap, addr)) return;
              long target = 0;
              if (!maintRequireLongParam(request, "value", target)) return;
              // Fast 409 from the copy; displayTask re-runs the same policy
              // against live facts right before the burn (authoritative).
              MaintVerdict verdict = maintValidateSetAddressTarget(
                  target, addr, snap.units, UNITS_AMOUNT);
              if (verdict.httpStatus != 200) {
                request->send(verdict.httpStatus, "text/plain",
                              verdict.message);
                return;
              }
              maintEnqueue(request,
                           makeSetAddressCommand(displayNextMaintSeq(),
                                                 (uint8_t)addr,
                                                 (uint8_t)target));
            });

  server.on("/unit/clear-address", HTTP_POST,
            [](AsyncWebServerRequest* request) {
              DisplaySnapshot snap = displaySnapshotGet();
              int addr = 0;
              if (!maintCheckAddress(request, snap, addr)) return;
              maintEnqueue(request, makeClearAddressCommand(
                                        displayNextMaintSeq(), (uint8_t)addr));
            });

  // Blank-out recalibration (v1 semantics: two full-row frames force the
  // wrap-around re-home, then the enqueue-time text returns). Text comes
  // from the display snapshot (what is actually showing), alignment/speed
  // from the settings of this moment — all baked into the command.
  server.on("/reset-units", HTTP_POST, [](AsyncWebServerRequest* request) {
    SerialPrintln(F("Units reset requested from web UI"));
    DisplaySnapshot snap = displaySnapshotGet();
    WebContentSnapshot content = webDisplayContentSnapshot();
    maintEnqueue(request, makeResetUnitsCommand(
                              displayNextMaintSeq(), String(snap.currentText),
                              content.alignment, content.flapSpeed));
  });

  // Kill switch (v1 #35). Order is load-bearing (review 2026-07-11): the
  // abort flag is set BEFORE the enqueue so the queue's happens-before
  // guarantees displayTask's Stop always finds it set — set-after-enqueue
  // races an idle displayTask clearing it first, stranding the flag ON for
  // every future wait. A 503 rolls the flag back (nothing queued to abort).
  server.on("/stop", HTTP_POST, [](AsyncWebServerRequest* request) {
    SerialPrintln(F("Stop requested from web UI"));
    DisplayCommand cmd = makeStopCommand(displayNextMaintSeq());
    unitBusRequestAbort();
    if (!displayEnqueue(cmd)) {
      unitBusClearAbort();
      request->send(503, "text/plain",
                    F("Display queue full — try again in a moment"));
      return;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "{\"seq\":%lu}", (unsigned long)cmd.seq);
    request->send(200, "application/json", buf);
  });

  // Execution feedback for the queued ops: pending / ok / failed(+reason) /
  // expired, rendered from the snapshot's single MaintResult slot.
  server.on("/unit/op-result", HTTP_GET, [](AsyncWebServerRequest* request) {
    long seq = 0;
    if (!maintRequireLongParam(request, "seq", seq)) return;
    if (seq < 1) {
      request->send(400, "text/plain", F("seq must be >= 1"));
      return;
    }
    DisplaySnapshot snap = displaySnapshotGet();
    char buf[96];
    buildOpResultJson(buf, sizeof(buf), snap.lastMaint, (uint32_t)seq);
    request->send(200, "application/json", buf);
  });

  // Bulk unit reflash (#205): pushes every unit not on the bundled rev
  // through twiboot with the PROGMEM-embedded image. Queue-native like all
  // maintenance ops — {"seq":N} now, job outcome via /unit/op-result, live
  // progress in /units/health's reflash object. Text/alignment/speed baked
  // at enqueue (the job re-shows them; reflashed units homed to blank).
  server.on("/reflash-units", HTTP_POST, [](AsyncWebServerRequest* request) {
    SerialPrintln(F("Unit reflash requested from web UI"));
    DisplaySnapshot snap = displaySnapshotGet();
    WebContentSnapshot content = webDisplayContentSnapshot();
    maintEnqueue(request, makeReflashUnitsCommand(
                              displayNextMaintSeq(), String(snap.currentText),
                              content.alignment, content.flapSpeed));
  });

  // --- MQTT broker mDNS discovery (#224, v1 /mqtt/discover contract) --------
  // POST arms the staged flag (re-POST while pending is a no-op — the flag
  // is the re-entry guard); the blocking MDNS.queryService pass runs from
  // netTask's drain, never here.
  server.on("/mqtt/discover", HTTP_POST, [](AsyncWebServerRequest* request) {
    {
      WebStateLock lock;
      if (!mqttDiscoverPending) {
        mqttDiscoverResultJson = "";
        mqttDiscoverPending = true;
      }
    }
    request->send(200, "text/plain", F("Broker discovery started"));
  });
  server.on("/mqtt/discover", HTTP_GET, [](AsyncWebServerRequest* request) {
    // Snapshot under the lock, send outside it (file convention — send()
    // schedules through AsyncTCP and its lock domain must not nest ours).
    bool pending;
    String json;
    {
      WebStateLock lock;
      pending = mqttDiscoverPending;
      json = mqttDiscoverResultJson;
    }
    if (pending) {
      request->send(202, "text/plain", F("Discovery running"));
      return;
    }
    if (json.length() == 0) {
      request->send(404, "text/plain", F("No discovery has run yet"));
      return;
    }
    request->send(200, "application/json", json);
  });

  // --- Cluster follower endpoints (#272, epic #270) --------------------------
  // The LAN wire protocol the leader drives (form-encoded requests, JSON
  // replies — same conventions as the rest of this API). Handlers stage
  // into ClusterFollower under its own mutex; NVS writes and the display
  // enqueue run in netTask's clusterFollowerServiceTick().
  server.on("/cluster/join", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("leaderHost", true) ||
        !request->hasParam("row", true) || !request->hasParam("epoch", true)) {
      request->send(400, "text/plain", F("Missing leaderHost/row/epoch"));
      return;
    }
    ClusterJoinRequest req;
    req.leaderHost = request->getParam("leaderHost", true)->value();
    req.leaderName = request->hasParam("leaderName", true)
                         ? request->getParam("leaderName", true)->value()
                         : req.leaderHost;
    long row = request->getParam("row", true)->value().toInt();
    req.epoch = (uint32_t)strtoul(
        request->getParam("epoch", true)->value().c_str(), nullptr, 10);
    if (row < 0 || row >= CLUSTER_MAX_MEMBERS) {
      request->send(400, "text/plain", F("Row out of range"));
      return;
    }
    req.row = (int)row;
    if (req.leaderHost.length() == 0 ||
        req.leaderHost.length() > CLUSTER_HOST_MAX_LEN ||
        !settingsIsPrintableAscii(req.leaderHost, 0x21)) {
      request->send(400, "text/plain", F("Invalid leaderHost"));
      return;
    }
    if (req.leaderName.length() > CLUSTER_HOST_MAX_LEN ||
        !settingsIsPrintableAscii(req.leaderName, 0x20)) {
      request->send(400, "text/plain", F("Invalid leaderName"));
      return;
    }
    clusterFollowerHandleJoin(req);
    // Handshake reply: identity, firmware rev, width. Width is the boot
    // probe's result today; #234 refines it — no protocol change.
    String out = "{\"name\":";
    appendJsonString(out, effectiveName);
    out += ",\"rev\":\"" GIT_REV "\",\"width\":";
    out += (int)displaySnapshotGet().displayWidth;
    out += ",\"protocol\":1}";
    request->send(200, "application/json", out);
  });

  server.on("/cluster/render", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("epoch", true) || !request->hasParam("seq", true) ||
        !request->hasParam("text", true)) {
      request->send(400, "text/plain", F("Missing epoch/seq/text"));
      return;
    }
    uint32_t epoch = (uint32_t)strtoul(
        request->getParam("epoch", true)->value().c_str(), nullptr, 10);
    uint32_t seq = (uint32_t)strtoul(
        request->getParam("seq", true)->value().c_str(), nullptr, 10);
    String text = request->getParam("text", true)->value();
    int speed;
    if (request->hasParam("speed", true)) {
      speed = request->getParam("speed", true)->value().toInt();
      if (speed < 1 || speed > 100) {
        request->send(400, "text/plain", F("Speed must be 1..100"));
        return;
      }
    } else {
      WebStateLock lock;
      speed = liveSettings->flapSpeed;
    }
    uint64_t commitAtMs =
        request->hasParam("commitAtMs", true)
            ? strtoull(request->getParam("commitAtMs", true)->value().c_str(),
                       nullptr, 10)
            : 0ULL;
    ClusterRenderVerdict v =
        clusterFollowerHandleRender(epoch, seq, text, speed, commitAtMs);
    if (v == ClusterRenderVerdict::NotClustered) {
      request->send(409, "application/json",
                    F("{\"error\":\"not clustered\"}"));
      return;
    }
    String out = "{\"applied\":";
    out += (v == ClusterRenderVerdict::Apply) ? "true" : "false";
    out += ",\"seq\":";
    out += String((unsigned long)seq);
    out += '}';
    request->send(200, "application/json", out);
  });

  server.on("/cluster/ping", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!clusterFollowerHandlePing()) {
      request->send(409, "application/json",
                    F("{\"error\":\"not clustered\"}"));
      return;
    }
    ClusterFollowerView cv = clusterFollowerViewGet();
    String out = "{\"state\":";
    appendJsonString(out, clusterFollowerPhaseName(cv.phase));
    out += ",\"epoch\":";
    out += String((unsigned long)cv.epoch);
    out += ",\"seq\":";
    out += String((unsigned long)cv.lastSeq);
    out += '}';
    request->send(200, "application/json", out);
  });

  server.on("/cluster/leave", HTTP_POST, [](AsyncWebServerRequest* request) {
    clusterFollowerHandleLeave();  // idempotent
    request->send(200, "text/plain", F("ok"));
  });

  server.on("/cluster/health", HTTP_GET, [](AsyncWebServerRequest* request) {
    ClusterFollowerView cv = clusterFollowerViewGet();
    DisplaySnapshot snap = displaySnapshotGet();
    String out = "{\"state\":";
    appendJsonString(out, clusterFollowerPhaseName(cv.phase));
    out += ",\"leaderName\":";
    appendJsonString(out, cv.leaderName);
    out += ",\"leaderHost\":";
    appendJsonString(out, cv.leaderHost);
    out += ",\"row\":";
    out += cv.row;
    out += ",\"epoch\":";
    out += String((unsigned long)cv.epoch);
    out += ",\"seq\":";
    out += String((unsigned long)cv.lastSeq);
    out += ",\"segment\":";
    appendJsonString(out, cv.heldSegment);
    out += ",\"rev\":\"" GIT_REV "\",\"width\":";
    out += (int)snap.displayWidth;
    out += ",\"detected\":";
    out += (int)snap.detectedUnitCount;
    out += ",\"faulty\":";
    out += (int)snap.faultyUnitCount;
    out += '}';
    request->send(200, "application/json", out);
  });

  // --- Cluster leader endpoints (#273) ----------------------------------------
  // `members` uses the ClusterLeaderPolicy wire format
  // (`host|row|col|width;…`, empty host = this master's own row; "" =
  // disable). Validation runs here for the 400; the swap itself (leave
  // fan-out, NVS persist, runtime reset) runs in clusterTask.
  server.on("/cluster/config", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("members", true)) {
      request->send(400, "text/plain", F("Missing members"));
      return;
    }
    ClusterConfigVerdict v = clusterLeaderStageConfig(
        request->getParam("members", true)->value());
    request->send(v.httpStatus, "text/plain", v.message);
  });

  server.on("/cluster/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    ClusterLeaderStatus st = clusterLeaderStatusGet();
    String out = "{\"enabled\":";
    out += st.enabled ? "true" : "false";
    out += ",\"epoch\":";
    out += String((unsigned long)st.epoch);
    out += ",\"seq\":";
    out += String((unsigned long)st.seq);
    out += ",\"members\":[";
    for (int i = 0; i < st.memberCount; i++) {
      const ClusterLeaderMemberStatus& m = st.members[i];
      if (i) out += ',';
      out += "{\"host\":";
      appendJsonString(out, m.host);
      out += ",\"self\":";
      out += m.host.length() == 0 ? "true" : "false";
      out += ",\"row\":";
      out += m.row;
      out += ",\"col\":";
      out += m.col;
      out += ",\"width\":";
      out += m.width;
      out += ",\"joined\":";
      out += m.joined ? "true" : "false";
      out += ",\"degraded\":";
      out += m.degraded ? "true" : "false";
      out += ",\"failures\":";
      out += m.failures;
      out += ",\"rev\":";
      appendJsonString(out, m.rev);
      out += ",\"reportedWidth\":";
      out += m.reportedWidth;
      out += '}';
    }
    out += "]}";
    request->send(200, "application/json", out);
  });
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

void webDisplayEventsTick() {
  // 100 ms check cadence: imperceptible latency, and the snapshot copy
  // stays off netTask's 10 ms hot loop.
  static uint32_t nextCheckMs = 0;
  static DisplayEventTracker tracker;
  uint32_t nowMs = millis();
  if ((int32_t)(nowMs - nextCheckMs) < 0) return;
  nextCheckMs = nowMs + 100;

  // count() before consuming the change: the library runs onConnect before
  // inserting the client into its list, so a change landing in that window
  // must stay unconsumed for the next tick. The resulting duplicate push of
  // an already-shown text is deduped by the mirror's frame compare.
  if (sseEvents.count() == 0) return;
  DisplaySnapshot snap = displaySnapshotGet();
  if (!displayEventDue(tracker, snap.currentText)) return;
  sseEvents.send(buildDisplayEventJson(snap.currentText).c_str(), "display",
                 millis());
}

void webEndpointsLoop(MasterSettings& settings, SettingsStore& store) {
  if (webStateMutex == nullptr) return;  // init hasn't run

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
      applySettingsPost(pendingPost, settings, store);
      timezoneChanged = settings.timezonePosix != timezoneBefore;

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
      // display. Ordering matters twice: after applySettingsPost so an
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

  // Outside the lock: configTzTime takes the LWIP core lock — keep the two
  // lock domains from ever nesting (v1 #48 parity: TZ applies rebootless).
  if (timezoneChanged) clockServiceApplyTz(settings);

  // MQTT broker discovery (#224): the blocking MDNS.queryService pass runs
  // out here for the same lock-domain reason — mDNS takes LWIP locks. The
  // ~1-2 s stall of netTask while it runs is the v1 loop() behavior.
  bool discoverDue;
  {
    WebStateLock lock;
    discoverDue = mqttDiscoverPending;
  }
  if (discoverDue) {
    MdnsBrokerCandidate candidates[4];
    size_t count = 0;
    int n = MDNS.queryService("mqtt", "tcp");
    for (int i = 0; i < n && count < 4; i++) {
      MdnsBrokerCandidate& c = candidates[count++];
      c.name = normalizeMdnsHostname(MDNS.hostname(i));
      IPAddress a = MDNS.address(i);
      c.ip = a == IPAddress() ? String() : a.toString();
      c.advertisedPort = MDNS.port(i);
      c.fromHomeAssistant = false;
    }
    if (count == 0) {
      // No broker advertised itself — Home Assistant's zeroconf record
      // locates the host running the Mosquitto add-on (v1 fallback).
      n = MDNS.queryService("home-assistant", "tcp");
      for (int i = 0; i < n && count < 4; i++) {
        MdnsBrokerCandidate& c = candidates[count++];
        c.name = normalizeMdnsHostname(MDNS.hostname(i));
        IPAddress a = MDNS.address(i);
        c.ip = a == IPAddress() ? String() : a.toString();
        c.advertisedPort = MDNS.port(i);
        c.fromHomeAssistant = true;
      }
    }
    String json = buildDiscoverJson(candidates, count);
    SerialPrintf("MQTT discover: %u candidate(s)\n", (unsigned)count);
    WebStateLock lock;
    mqttDiscoverResultJson = json;
    mqttDiscoverPending = false;
  }

  // Flash-log drain (#206): netTask is the single flash writer.
  flashLogTick(rebootDue);  // force on reboot so the last lines land

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

// Bundled unit firmware accessors (#205) — see WebEndpoints.h for why this
// TU is the only WebAssets.h includer. On the S3, PROGMEM is flash-mapped
// and directly readable, so the pointer works as a plain byte buffer.
const uint8_t* webUnitFirmwareBin() { return UNIT_FIRMWARE_BIN; }
size_t webUnitFirmwareBinLen() { return UNIT_FIRMWARE_BIN_LEN; }
