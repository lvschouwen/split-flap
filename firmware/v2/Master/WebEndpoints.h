#pragma once

#include <Arduino.h>

#include "Settings.h"
#include "SettingsStore.h"

class AsyncWebServer;

// Web endpoint layer for the v2 master (#186) — the v1 endpoint surface on
// the ESP32Async server stack. This slice registers the full route set: the
// PROGMEM-served UI, the settings read/write pair, health/log/reboot, and
// explicit 501 stubs for every endpoint whose backing service hasn't been
// ported yet (units/I2C, firmware/OTA, MQTT discovery, WiFi reset).
//
// Lifecycle: webEndpointsInit() registers routes only. webEndpointsStart()
// calls server.begin() and belongs to the WiFi slice — the LWIP stack isn't
// up until a network interface exists, so this slice never starts listening.
// webEndpointsLoop() drains the staged settings post and the pending reboot
// from loop() context (v1 async-context rule #150: handlers stage, the loop
// mutates).
void webEndpointsInit(AsyncWebServer& server, MasterSettings& settings,
                      SettingsStore& store, const String& effectiveDeviceName);
void webEndpointsStart(AsyncWebServer& server);
void webEndpointsLoop(MasterSettings& settings, SettingsStore& store);
