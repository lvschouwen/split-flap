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
#include <Update.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "BuildVersion.h"
#include "ClockPolicy.h"
#include "ClockService.h"
#include "HelpersSerialHandling.h"
#include "OtaService.h"
#include "PendingSettingsPost.h"
#include "SettingsJson.h"
#include "Tasks.h"
#include "WebAssets.h"
#include "WebLog.h"
#include "WifiService.h"

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

// Live state the read handlers render. Set once in webEndpointsInit();
// handlers only ever read (async-context rule). Held as TU-local statics
// rather than lambda-captured references so a handler can never outlive
// what it captured.
static MasterSettings* liveSettings = nullptr;
static String effectiveName;

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

static const char* NOT_PORTED_MSG =
    "Not available on the v2 master yet (#58): this endpoint's backing "
    "service hasn't been ported.";

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

static void registerNotPortedStub(AsyncWebServer& server, const char* path,
                                  WebRequestMethod method) {
  server.on(path, method, [](AsyncWebServerRequest* request) {
    request->send(501, "text/plain", NOT_PORTED_MSG);
  });
}

static const char* resetReasonString() {
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
  (void)store;  // handlers never write the store; the loop drain does
  webStateMutex = xSemaphoreCreateMutex();
  if (webStateMutex == nullptr) {
    // Boot-time OOM: WebStateLock on a null handle is UB, so fail loudly
    // instead — abort() panics into the coredump partition.
    Serial.println(F("FATAL: webStateMutex allocation failed"));
    abort();
  }
  liveSettings = &settings;
  effectiveName = effectiveDeviceName;

  // --- static assets, all from PROGMEM (generated by build_assets.py) -----
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    SerialPrintln(F("Request Home Page Received"));
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

  // --- reads ---------------------------------------------------------------
  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest* request) {
    SerialPrintln(F("Request for Settings Received"));
    SettingsJsonFields f;
    f.unitsAmount = UNITS_AMOUNT;  // arrays default per-slot until I2C slice
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
    f.lastWrittenText = String(displaySnapshotGet().currentText);
    f.mqttConnected = false;  // MQTT runtime is a later slice
    f.version = GIT_REV;
    f.sketchMd5 = ESP.getSketchMD5();
    // Verdict synthesized from esp_ota partition state (#190) — v1 wire
    // vocabulary plus "pending" while the health confirm hasn't run yet.
    OtaVerdict verdict = otaVerdictSnapshot();
    f.lastFlashResult = verdict.lastFlashResult;
    f.otaReverted = verdict.otaReverted;
    f.lastResetReason = resetReasonString();
    request->send(200, "application/json", buildSettingsJson(f));
  });

  server.on("/health", HTTP_GET, [](AsyncWebServerRequest* request) {
    SerialPrintln(F("Request for Health Check Received"));
    request->send(200, "text/plain", "Healthy");
  });

  server.on("/log", HTTP_GET, [](AsyncWebServerRequest* request) {
    // Don't SerialPrintln here; every log request would otherwise stamp
    // itself into the buffer on every poll and drown out real activity.
    request->send(200, "text/plain", webLogRead());
  });

  // --- settings/message form (v1 wire contract) ----------------------------
  server.on("/", HTTP_POST, [](AsyncWebServerRequest* request) {
    SerialPrintln(F("Request Post of Form Received"));

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
      SerialPrintln(F("Finished Processing Request with Error"));
      if (isAjax) request->send(400, "text/plain", F("invalid"));
      else request->redirect("/?invalid-submission=true");
      return;
    }

    // Message sends become display commands at drain time; report a full
    // queue now instead of accepting a message that would be dropped. (The
    // stub worker drains instantly, so this only fires if something wedges.)
    if (local.inputTextProvided && displayQueueFull()) {
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

    SerialPrintln(F("Finished Processing Request Successfully"));
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
    SerialPrintln(F("Request to Reboot Received"));
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
    SerialPrintln(F("Request to Scan WiFi Received"));
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
    SerialPrintln(F("Request to Configure WiFi Received"));
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
    SerialPrintln(F("Request to Reset WiFi Received"));
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
        if (otaRejectionStatus != 0) {
          request->send(otaRejectionStatus, "text/plain", otaRejectionReason);
          return;
        }
        if (Update.hasError()) {
          request->send(500, "text/plain", String("Master OTA failed: ") +
                                               Update.errorString());
          return;
        }
        if (!Update.isFinished()) {
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
          otaRejectionStatus = 0;
          otaRejectionReason = "";

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

          // ?v= staged unconditionally (empty if absent) so a stale value
          // from an earlier flash can't outlive this one (v1 #52 rationale).
          String intended = request->hasParam("v")
                                ? request->getParam("v")->value()
                                : String();
          if ((int)intended.length() >= LEN_INTENDED_VERSION) {
            intended = intended.substring(0, LEN_INTENDED_VERSION - 1);
          }
          WebStateLock lock;
          pendingIntendedVersion = intended;
          pendingIntendedVersionProvided = true;
        }

        if (otaRejectionStatus != 0) return;

        if (len > 0 && Update.write(data, len) != len) {
          // Error is latched inside Update; the completion callback
          // reports it. Stop consuming flash time on further chunks.
          return;
        }
        if (final) {
          if (Update.end(true)) {
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

  // --- v1 endpoints whose services aren't ported yet (#58 slices) ----------
  // Explicit 501s so a bench click yields a clear message instead of a
  // silent 404. Each stub is retired by the slice that ports its service.
  registerNotPortedStub(server, "/units/health", HTTP_GET);
  registerNotPortedStub(server, "/units/health/refresh", HTTP_POST);
  registerNotPortedStub(server, "/unit/offset", HTTP_GET);
  registerNotPortedStub(server, "/unit/offset", HTTP_POST);
  registerNotPortedStub(server, "/unit/jog", HTTP_POST);
  registerNotPortedStub(server, "/unit/home", HTTP_POST);
  registerNotPortedStub(server, "/unit/reboot", HTTP_POST);
  registerNotPortedStub(server, "/unit/identify", HTTP_POST);
  registerNotPortedStub(server, "/unit/set-address", HTTP_POST);
  registerNotPortedStub(server, "/unit/clear-address", HTTP_POST);
  registerNotPortedStub(server, "/stop", HTTP_POST);
  registerNotPortedStub(server, "/reflash-units", HTTP_POST);
  registerNotPortedStub(server, "/reset-units", HTTP_POST);
  registerNotPortedStub(server, "/firmware/recover-mark", HTTP_POST);
  registerNotPortedStub(server, "/firmware/ota-mode", HTTP_POST);
  registerNotPortedStub(server, "/mqtt/discover", HTTP_GET);
  registerNotPortedStub(server, "/mqtt/discover", HTTP_POST);
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
      if (pendingPost.transientTextProvided) {
        SerialPrintln("Transient text dropped (mode service not ported yet, "
                      "#58): " + pendingPost.transientText);
      }

      // "Last Received" tracks messages/mode switches, not settings saves —
      // per-card posts (#128) mean only those submissions stamp it.
      if (messageProvided || pendingPost.deviceModeProvided) {
        lastMessageStamp = formatDateTime(time(nullptr), CLOCK_STAMP_FORMAT);
      }

      // Settings first, command second: a speed/alignment change riding the
      // same POST as a message must apply to that message (v1 ordering).
      String timezoneBefore = settings.timezonePosix;
      applySettingsPost(pendingPost, settings, store);
      timezoneChanged = settings.timezonePosix != timezoneBefore;

      // v1 parity: a posted message only takes effect in text mode, checked
      // AFTER any mode field riding the same POST. In clock mode it is
      // silently ignored — never shown, never retained.
      if (messageProvided && settings.deviceMode == "text") {
        // Retained in the display domain: ClockPolicy's dedup compares this
        // against snapshot text, which makeShowTextCommand truncates.
        currentInputText = truncateForDisplay(messageText);
        DisplayCommand cmd = makeShowTextCommand(
            messageText, settings.alignment, settings.flapSpeed);
        if (displayEnqueue(cmd)) {
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

  if (rebootDue) {
    SerialPrintln(F("Rebooting..."));
    Serial.flush();
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
