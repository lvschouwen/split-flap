#pragma once

#include <Arduino.h>

#include "Settings.h"
#include "SettingsStore.h"

class AsyncWebServer;

// Web endpoint layer for the v2 master (#186) — the v1 endpoint surface on
// the ESP32Async server stack, plus the WiFi portal/credential routes
// (#188). Registers the PROGMEM-served UI, the settings read/write pair,
// health/log/reboot, /wifi-setup + /wifi/scan + /wifi/config + /reset-wifi,
// and explicit 501 stubs for every endpoint whose backing service hasn't
// been ported yet (units/I2C, firmware/OTA, MQTT discovery).
//
// Lifecycle: webEndpointsInit() registers routes only. webEndpointsStart()
// calls server.begin() — WifiService invokes it (idempotently) once a netif
// exists, portal AP or STA join, whichever comes first. webEndpointsLoop()
// drains the staged settings post and the pending reboot from netTask
// context (v1 async-context rule #150: handlers stage, the loop mutates).
void webEndpointsInit(AsyncWebServer& server, MasterSettings& settings,
                      SettingsStore& store, const String& effectiveDeviceName);
void webEndpointsStart(AsyncWebServer& server);
void webEndpointsLoop(MasterSettings& settings, SettingsStore& store);

// SSE display push (#251), netTask only: sends a "display" event when the
// snapshot's text changes. Change detection pure in DisplayEvents.h.
void webDisplayEventsTick();

// What the 1 Hz mode ticker needs from the web domain (#192): the active
// mode plus the parameters it bakes into DisplayCommands. inputText is the
// retained runtime message (never persisted, "" until the first POST) that
// a clock->text mode switch re-shows.
struct WebContentSnapshot {
  String deviceMode;
  String inputText;
  String alignment;
  int flapSpeed = 1;
};

// Mutex-guarded copy for clockTask (core 1) — same snapshot-copy rule as
// DisplaySnapshot: consumers render from the copy, never from live state.
WebContentSnapshot webDisplayContentSnapshot();

// MQTT command write path (#224): mqttTask applies validated HA commands
// under webStateMutex with NVS write-through — the same invariants as the
// settings drain. Each returns true when the value actually changed.
bool webMqttApplyMode(const String& mode);
bool webMqttApplySpeed(int speed);
bool webMqttApplyAlignment(const String& alignment);

// Stage the standard graceful reboot (drain flushes logs, then restarts) —
// the HA restart button rides the same dispatcher as POST /reboot.
void webRequestReboot();

// Mutex-guarded reads for MQTT's retained diagnostics.
String webTimezoneSnapshot();
const char* webResetReasonString();

// Bundled unit firmware (#205): the generated WebAssets.h arrays have
// internal linkage, so only WebContent.cpp includes that header (#338) — a
// second include would duplicate every PROGMEM blob into another TU.
// displayTask reaches the image through these accessors instead.
const uint8_t* webUnitFirmwareBin();
size_t webUnitFirmwareBinLen();
