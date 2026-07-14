#pragma once
// FollowerBus.h — the follower's I2C unit bus (#298): trimmed port of v1's
// ServiceFlapFunctions.ino + ServiceFirmwareFunctions.ino onto the v2
// UnitFacts model. The superloop (loop() in main.cpp) is the ONLY caller —
// async web handlers never touch Wire; they stage work the loop drains
// (v1's context rule verbatim).
//
// Twiboot quirks honored (v1 #88): the boot probe waits out the 1500 ms
// twiboot window (main.cpp), and busProbeInhibitedUntilMs() arms after a
// reboot-to-bootloader op — every loop-drained probe waits it out. The
// reflash job's internal probes are the documented exception (#205
// semantics: pinned units are immediately flashed + exited).

#include <Arduino.h>

#include "FollowerConfig.h"  // UNITS_AMOUNT
#include "FollowerOps.h"
#include "UnitHealth.h"
#include "UnitProtocolHelpers.h"  // UnitSelfTestReading

// Per-slot facts (probe + health poll truth) and the derived row width.
extern UnitFacts unitFacts[UNITS_AMOUNT];
extern int displayWidth;
extern int detectedUnitCount;
extern ReflashProgress reflashProgress;

void busInit();

// Full bus scan: state, version (vs the bundled rev), offset, odometer per
// slot; recomputes displayWidth. Blocking (~2 ms/unit) — loop() only.
void busProbe();

// CMD_GET_STATUS + odometer refresh for every sketch-mode unit. loop() only.
void busPollHealth();

// Renders one pre-positioned segment verbatim: pad/truncate to the probed
// width, write per-unit letter indexes, wait, verify + resend (v1 #106).
// Blocking for the whole flap time — loop() only.
void busShowSegment(const String& segment, int webSpeed);

// Probe-inhibit deadline (v1 #88): armed by the reboot-to-bootloader op so
// runtime probes never hit the twiboot window and pin it alive.
uint32_t busProbeInhibitedUntilMs();
void busArmProbeInhibit(uint32_t untilMs);

// Short single-unit ops (loop() executes the staged {"seq":N} op).
int busWriteOffset(uint8_t i2cAddress, int16_t value);
int busJog(uint8_t i2cAddress, int steps);
int busHome(uint8_t i2cAddress);
int busIdentify(uint8_t i2cAddress);
int busResetOdometer(uint8_t i2cAddress);
int busRebootToBootloader(uint8_t i2cAddress);
int busStartSelfTest(uint8_t i2cAddress);
bool busReadSelfTest(uint8_t i2cAddress, UnitSelfTestReading& out);

// The full bundled-hex reflash job (v1 #138 flow: enter-bootloader sweep,
// throttled PROGMEM flash, batch settle). Blocking for many seconds —
// loop() only; progress lands in reflashProgress.
void busRunReflashJob();

// Boot-time provisioning/auto-update (v1 semantics): flash every unit the
// probe found sitting in twiboot; push provably-outdated sketch units
// through the same path.
void busAutoInstallBootloaderUnits();
void busAutoUpdateOutdatedUnits();
