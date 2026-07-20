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
extern String pendingIntendedVersion;  // ?v= from /firmware/master (#190)
extern bool pendingIntendedVersionProvided;

// Runtime-only message state (#192, never persisted) — under WebStateLock.
extern String currentInputText;
extern String lastMessageStamp;

// Full /settings JSON gather — GET /settings and the /status aggregate (#307)
// render the identical object through this.
String buildCurrentSettingsJson();

// #313 inline CSRF gate for routes that write flash inside onUpload (the
// middleware fires post-body, too late). True = forged cross-site POST.
bool webUploadCsrfRejected(AsyncWebServerRequest* request);

// #294/#313 CORS+CSRF middleware (owned by WebCluster.cpp), attached once in
// webEndpointsInit().
AsyncMiddlewareFunction& webClusterCorsMiddleware();

// Per-module route registrars, called once from webEndpointsInit(). Routes
// are matched per path+method, so cross-module registration order is not
// semantic; same-path method pairs stay within one module.
void webContentRegister(AsyncWebServer& server);
void webSettingsRegister(AsyncWebServer& server);
void webSystemRegister(AsyncWebServer& server);
void webFirmwareRegister(AsyncWebServer& server);
void webMaintenanceRegister(AsyncWebServer& server);
void webClusterRegister(AsyncWebServer& server);

// Loop hooks drained by webEndpointsLoop() — call order is load-bearing,
// see the webEndpointsLoop() call site.
void webFirmwareLoop();
void webSettingsDiscoverLoop();
void webClusterDiscoverLoop();
