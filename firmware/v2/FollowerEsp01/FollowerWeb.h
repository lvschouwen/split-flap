#pragma once
// FollowerWeb.h — the follower's complete endpoint surface (#298; the spec
// table is exhaustive — nothing else exists, no HTML): /cluster/{join,
// render,ping,leave,health}, /firmware/master (v1 OTA contract, ?md5=
// mandatory), /reflash-units, /settings, /units/health(+refresh), the
// {"seq":N} maintenance-op subset, /unit/op-result, /reboot. Async rule
// (v1 verbatim): handlers validate + stage; webLoopTick() executes staged
// ops / reflash / health refreshes from loop() — the only I2C context.

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// Cross-context flags (v1 conventions).
extern volatile bool isPendingReboot;

void webEndpointsInit(AsyncWebServer& server);

// True while a master OTA upload is streaming in — loop() freezes all
// display/unit work (v1 #116). Owns the 30 s stalled-upload auto-thaw,
// including freeing the Update session slot (v1 #191: a dangling owner
// would 409 every later upload forever).
bool webOtaUploadFrozen();

// loop() drain: staged unit op execution, self-test polling, unit-health
// refresh (+ probe-inhibit wait), the blocking reflash job.
void webLoopTick();
