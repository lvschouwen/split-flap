#pragma once

// Internal seam of the web endpoint family (#338): shared state + per-module
// registrar/loop hooks for the Web*.cpp translation units split out of the
// once-monolithic WebEndpoints.cpp. Include from Web*.cpp ONLY — the names
// below have external linkage across this family (definitions live in
// WebEndpoints.cpp) and are deliberately NOT part of the public
// WebEndpoints.h surface. Other modules (e.g. WifiService.cpp) keep
// same-named file-statics; they must never see these declarations.
//
// The async-context rule (WebEndpoints.cpp header) binds every module here:
// handlers stage, netTask's webEndpointsLoop() mutates.

#include <ESPAsyncWebServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "PendingSettingsPost.h"
#include "Settings.h"
#include "SettingsStore.h"

// Cross-task guard: handlers run in the async_tcp task, the drain in netTask —
// shared Strings need real synchronization. A mutex (not a spinlock) because
// the critical sections allocate Strings and write NVS.
extern SemaphoreHandle_t webStateMutex;

struct WebStateLock {
  WebStateLock() { xSemaphoreTake(webStateMutex, portMAX_DELAY); }
  ~WebStateLock() { xSemaphoreGive(webStateMutex); }
  WebStateLock(const WebStateLock&) = delete;
  WebStateLock& operator=(const WebStateLock&) = delete;
};

// Live state the read handlers render — set once in webEndpointsInit(),
// handlers only read (under WebStateLock where mutable).
extern MasterSettings* liveSettings;
extern SettingsStore* liveStore;
extern String effectiveName;

// Staged mutations, drained by webEndpointsLoop() (all under WebStateLock).
extern PendingSettingsPost pendingPost;
extern bool pendingReboot;
extern uint32_t rebootRequestedAtMs;
extern String pendingRebootCause;  // #432: stamped to NVS at the drain
extern String pendingIntendedVersion;  // ?v= from /firmware/master (#190)
extern bool pendingIntendedVersionProvided;
// ?v= from /firmware/rescue (#391) — staged like the above because writing
// the rescue-slot record is an NVS commit, not part of the image stream.
extern String pendingRescueRev;
extern bool pendingRescueRecord;

// Runtime-only message state (#192, never persisted) — under WebStateLock.
extern String currentInputText;
extern String lastMessageStamp;

// The /settings JSON gather.
String buildCurrentSettingsJson();

// #313 inline CSRF gate for routes that write flash inside onUpload (the
// middleware fires post-body, too late). True = forged cross-site POST.
bool webUploadCsrfRejected(AsyncWebServerRequest* request);

// #313 CSRF middleware, attached once in webEndpointsInit().
AsyncMiddlewareFunction& webCsrfMiddleware();

// The gates every /api/v2 action and settings change goes through.
enum class WebStage : uint8_t {
  Staged,
  UnitUpdate,  // text while a unit update runs (the producer gate, #205)
  QueueFull,   // text while the display queue is full
};
// Merges a fully validated post into the pending one. `needsReboot` and
// `deviceNameChanged` are judged against the live settings.
WebStage webStagePost(const PendingSettingsPost& local, bool& needsReboot,
                      bool& deviceNameChanged);
// Why this board must not restart now (a unit update or a firmware upload is
// running), nullptr when it may.
const char* webRestartRefusal();
// Restart this board once the answer has gone out. Returns why not
// (webRestartRefusal), nullptr when staged.
const char* webStageReboot(const char* cause);
// The kill switch: aborts what the units are doing and blanks every row.
// False when the display queue is full (nothing was stopped).
bool webStopWall(uint32_t& seq);
// Is the wall quiet now?
bool webQuietNow();
// Do the boards update their units when they start?
bool webUpdateUnitsAtStart();
// Why this board last restarted on purpose; "" when it did not (#432).
const String& webBootRebootCause();

// Per-module route registrars, called once from webEndpointsInit(). Routes
// are matched per path+method, so cross-module registration order is not
// semantic; same-path method pairs stay within one module.
void webContentRegister(AsyncWebServer& server);
void webSettingsRegister(AsyncWebServer& server);
void webSystemRegister(AsyncWebServer& server);
// #431: staged /coredump/erase drain (owned by WebSystem.cpp, called from
// webEndpointsLoop — netTask is the sole flash writer).
void webSystemCoredumpEraseTick();
// #395: live master-OTA upload session (owned by WebFirmware.cpp) — the
// restart gate consults it so a restart can't tear a mid-flight flash write.
bool webFirmwareOtaUploadActive();
void webFirmwareRegister(AsyncWebServer& server);
void webWallRegister(AsyncWebServer& server);
void webBoardRegister(AsyncWebServer& server);
void webStreamRegister(AsyncWebServer& server);

// Loop hooks drained by webEndpointsLoop() — call order is load-bearing,
// see the webEndpointsLoop() call site.
void webFirmwareLoop();
void webWallFindRowsLoop();
