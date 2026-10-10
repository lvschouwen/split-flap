#pragma once

#include <Arduino.h>

#include "Settings.h"
#include "SettingsStore.h"

class AsyncWebServer;

// Web endpoint layer for the v2 master (#186) on the ESP32Async server
// stack: the PROGMEM-served page, /api/v2, the firmware uploads, the WiFi
// portal routes (#188) and the diagnostics reads. GET /api lists them.
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

// netTask only: pushes what changed to the readers of GET /api/v2/stream.
void webStreamTick();

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
bool webMqttApplyQuiet(bool quiet);  // #227
bool webMqttApplySpeed(int speed);
bool webMqttApplyAlignment(const String& alignment);

// Stage the standard graceful reboot (drain flushes logs, then restarts) —
// the HA restart button rides the same dispatcher as the restart action. The cause
// is stamped to NVS at the drain and served as lastRebootCause next boot
// (#432).
void webRequestReboot(const char* cause);

// An update from a release (#583) put an image where an upload would have:
// the same notes, staged for the drain like an upload's ?v=.
void webNoteRescueInstalled(const char* rev);
void webNoteIntendedVersion(const char* rev);

// Mutex-guarded reads for MQTT's retained diagnostics.
String webTimezoneSnapshot();
const char* webResetReasonString();
// Same names for a stored esp_reset_reason_t (boot trace, #504).
const char* webResetReasonName(int reason);

// Bundled unit firmware (#205): the generated WebAssets.h arrays have
// internal linkage, so only WebContent.cpp includes that header (#338) — a
// second include would duplicate every PROGMEM blob into another TU.
// displayTask reaches the image through these accessors instead.
const uint8_t* webUnitFirmwareBin();
size_t webUnitFirmwareBinLen();
