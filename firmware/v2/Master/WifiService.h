#pragma once
// WifiService — the v2 master's WiFi bring-up (#188): stored-credential
// join, captive-portal fallback, mDNS, /reset-wifi. All radio work runs in
// netTask via wifiServiceTick(); the decision logic is WifiPolicy.h (pure,
// natively tested), this layer only executes its actions.
//
// Async-context rule (v1 #150): web handlers call ONLY the wifiStage*() /
// wifiScan*() / wifiPortalRedirectUrl() functions below — mutex-guarded
// staging and reads, never radio or NVS work.

#include <Arduino.h>

#include "Settings.h"
#include "SettingsStore.h"

class AsyncWebServer;

// Stores the wiring; the radio comes up on netTask's first tick. Call from
// setup() before tasksInit() so no tick runs uninitialized.
void wifiServiceInit(AsyncWebServer& server, MasterSettings& settings,
                     SettingsStore& store, const String& effectiveDeviceName);

// netTask context only: runs the join/portal policy, executes its actions,
// pumps the portal DNS catch-all and any in-flight scan.
void wifiServiceTick();

// --- staging + reads for async handlers -------------------------------------

// POST /wifi/config: pre-validated ssid/pass, copied under the mutex; the
// tick persists them and reboots (v1 portal-save semantics).
void wifiStagePortalConfig(const String& ssid, const String& pass);

// POST /reset-wifi: the tick deletes both NVS keys and reboots into the
// portal.
void wifiStageReset();

// POST /wifi/scan: the tick starts an async scan and caches the JSON.
void wifiStageScan();

// GET /wifi/scan: cached scan JSON; "" while no scan has completed yet.
String wifiScanResultJson();

// Captive-redirect target ("http://<ap-ip>/wifi-setup"), staged when the
// portal comes up; "" while it isn't. Non-empty switches the onNotFound
// handler into captive-redirect mode without it touching the WiFi class.
String wifiPortalRedirectUrl();
