#pragma once

#include <Arduino.h>

#include "DisplayCommand.h"  // DisplaySource on the content snapshot (#403)
#include "Settings.h"
#include "SettingsStore.h"

// What the board is currently told to show, and who is allowed to change it.
//
// This used to live inside the web endpoint layer, which meant the HTTP
// surface OWNED the display's runtime state: clockTask and mqttTask both
// reached into WebEndpoints.cpp for the retained message, the mode, the
// alignment and the speed. That is backwards — the state is the product,
// a protocol is just one way to reach it — and it is why replacing the API
// meant touching the display domain. Any new API sits ON this module.
//
// Threading: the same rule the web drain used. Every mutable String is
// guarded by one mutex (not a spinlock — the critical sections allocate
// Strings and write NVS). Consumers render from a snapshot copy, never from
// live state, exactly like DisplaySnapshot.

// The retained runtime message plus the parameters producers bake into
// DisplayCommands. inputText is never persisted ("" until something sets
// it); a clock->text mode switch re-shows it, which is why its ORIGINAL
// producer travels with it (#403) rather than being reattributed.
struct DisplayContent {
  String deviceMode;
  String inputText;
  DisplaySource inputTextSource = DisplaySource::Unknown;
  String alignment;
  int flapSpeed = 1;
};

// Binds the live settings + store. Call once, before any task starts.
void contentStateInit(MasterSettings& settings, SettingsStore& store);

// Mutex-guarded copy for clockTask (core 1) and anything else that renders.
DisplayContent contentSnapshot();

// Writers. NVS write-through; each returns true when the value actually
// changed, so callers can log an edge rather than every echo of the current
// value. Rejecting empty input is belt-and-braces — callers validate first.
bool contentSetMode(const String& mode);
bool contentSetSpeed(int speed);
bool contentSetAlignment(const String& alignment);

// The retained message and its producer, set together so a text can never
// carry the wrong attribution.
void contentSetText(const String& text, DisplaySource source);

// Staged graceful reboot: a caller asks, netTask's contentStateTick()
// performs it once the log has been flushed. Nothing reboots from a
// protocol handler's own context.
void contentRequestReboot();
void contentStateTick();

// Mutex-guarded reads for diagnostics.
String contentTimezone();
const char* contentResetReasonString();

// Bundled unit firmware (#205): the generated WebAssets.h arrays have
// internal linkage, so only UnitFirmwareAsset.cpp includes that header — a
// second include would duplicate the whole image into another TU.
// displayTask reaches the image through these accessors instead.
const uint8_t* unitFirmwareBin();
size_t unitFirmwareBinLen();
