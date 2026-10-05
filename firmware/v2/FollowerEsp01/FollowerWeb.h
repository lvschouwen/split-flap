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

#include "BootInfo.h"
#include "FollowerOps.h"

// Cross-context flags (v1 conventions).
extern volatile bool isPendingReboot;

void webEndpointsInit(AsyncWebServer& server);

// True while a master OTA upload is streaming in — loop() freezes all
// display/unit work (v1 #116). Owns the 30 s stalled-upload auto-thaw,
// including freeing the Update session slot (v1 #191: a dangling owner
// would 409 every later upload forever).
bool webOtaUploadFrozen();

// Running image + stored upload share [0, this) of the flash.
uint32_t appAreaBytes();

// loop() drain: staged unit op execution, self-test polling, unit-health
// refresh (+ probe-inhibit wait), the blocking reflash job.
void webLoopTick();

// --- unit jobs for a caller that is not a route (the wall link, #564) ----------------
// The same single staged-op slot and result slots the {"seq":N} routes use;
// loop() context only.

enum class UnitOpStaged : uint8_t { Yes, Rescue, Busy, NoMemory };

// Stages one op for webLoopTick(); `seq` names it in the result slots.
UnitOpStaged unitOpStage(FollowerOpKind kind, uint8_t addr, long arg,
                         uint32_t& seq);
// A unit job is queued, running or still being waited on.
bool unitOpsBusy();
const MaintResult& unitOpResult();
const SelfTestSlot& unitOpSelfTest();
const BootInfoSlot& unitOpBootInfo();
// The BOOT_SECTION_LEN bytes the dump op `seq` read; nullptr once they are
// no longer held.
const uint8_t* unitOpBootDumpBytes(uint32_t seq);

// The /units/health document into buf; returns its length. Size the buffer
// with followerHealthBufCap().
size_t unitsHealthJson(char* buf, size_t cap);
