#pragma once

// UnitBus — the v2 master's I2C access to the v1 units (#203, slice A of
// the I2C port). The ONLY Wire toucher in the firmware, and displayTask is
// its only caller (#187 rule: bus exclusivity is structural — no busy
// flags, the command queue is the staging mechanism). Blocking by design:
// a straight port of v1's bench-proven transactions and timing; commands
// queue behind long operations.
//
// All functions here have hardware side effects and are bench-tested (the
// pure seams they lean on — FlapFrame, UnitHealth, DisplayWidth — are the
// natively-tested tier).

#include <stddef.h>
#include <stdint.h>

#include "UnitHealth.h"
#include "UnitProtocolHelpers.h"

// Wire init on the unit bus pins. SDA=8 / SCL=9 (Arduino-ESP32 S3 defaults),
// 100 kHz, 3.3 V — electrically a drop-in for the ESP-01. Clear of the
// reserved pins (4 button, 19/20 USB, 35-37 PSRAM, 48 LED).
void unitBusInit();

// Scans I2C addresses base..base+maxUnits-1 and fills facts[i].state
// (0 silent / 1 sketch / 2 bootloader), plus version + fwStatus for
// sketch-mode units. Resets every slot first — a re-probe forgets units
// that dropped off the bus.
void unitBusProbe(UnitFacts* facts, int maxUnits);

// Reads CMD_GET_STATUS from every sketch-mode unit into facts[i].status /
// statusValid. Bootloader/silent slots stay invalid so they render as gaps
// and never count toward the faulty total.
void unitBusPollHealth(UnitFacts* facts, int maxUnits);

// Reads one unit's full health block (status + odometer + diag + vitals) into
// facts[i], same validity semantics as unitBusPollHealth. Returns whether the
// CMD_GET_STATUS read succeeded — the scheduled-heartbeat liveness signal
// (#310). A bootloader/silent slot returns false without bus traffic.
bool unitBusPollHealthOne(UnitFacts* facts, int i);

// Drives one frame onto the flaps: waits for the display to stop, sends
// letters[0..width-1] to every sketch-mode unit, waits again, then verifies
// each unit via CMD_GET_LETTER with one resend round (v1 #106 closed loop).
// `unitSpeed` is the wire speed (MIN_SPEED..MAX_SPEED, already converted).
// Returns the number of failed unit writes (v1's writeErrors tally).
int unitBusShowFrame(const UnitFacts* facts, int width,
                     const uint8_t* letters, int unitSpeed);

// --- calibration + provisioning (#204) — straight v1 ports -------------------
// All return Wire.endTransmission() status (0 = success) so displayTask can
// grade the op's MaintResult.

// unitBusWriteOffset additionally reads the offset back (#405) and returns
// UNIT_BUS_OFFSET_UNVERIFIED / _MISMATCH (UnitWireContract.h) instead of 0
// when the write did not demonstrably land.
int unitBusWriteOffset(int i2cAddress, int16_t value);  // persists, verifies
int unitBusJog(int i2cAddress, int steps);              // ±127, not persisted
int unitBusHome(int i2cAddress);                        // full calibrate(true)
int unitBusIdentify(int i2cAddress);                    // ~3 s LED blink
int unitBusResetOdometer(int i2cAddress);               // zero wear odometer (#231)

// Persists the unit's UNIT_GATE_* byte (#409) and VERIFIES it by reading
// GET_LIFETIME back — returns UNIT_BUS_GATES_UNVERIFIED / _MISMATCH
// (UnitWireContract.h) instead of 0 when the write did not demonstrably land.
// The unit rejects gate bits it has no code for, so a refused write surfaces
// as _MISMATCH rather than as a silent success.
int unitBusSetGates(int i2cAddress, uint8_t gates);
int unitBusStartSelfTest(int i2cAddress);               // ~15 s diagnostic rev (#265)

// Reads the unit's self-test state/result via CMD_GET_SELF_TEST (#265).
// False on wire failure or a checksum-rejected reply (old firmware).
bool unitBusReadSelfTest(int i2cAddress, UnitSelfTestReading& out);

// Bus transaction counters (#245): written only by displayTask, safe to
// read cross-task (aligned 32-bit). Idle rotation polls are not counted.
uint32_t unitBusTxCount();
uint32_t unitBusErrCount();
int unitBusRebootToBootloader(int i2cAddress);          // twiboot @DIP, ~1 s
int unitBusSetAddress(int i2cAddress, uint8_t newAddress);  // burn + reboot
int unitBusClearAddress(int i2cAddress);                    // EEPROM → DIP
int unitBusBroadcastHome();  // general-call CMD_HOME, one transaction (v1 #47)

// --- unit reflash over twiboot (#205) — straight v1 ports ---------------------

// Outcome of one unit's flash attempt. Any failure after Ok's page stream
// began leaves the unit sitting in twiboot — boot auto-install or a retry
// recovers it (v1 failure story); it is never exited onto a torn image.
enum class UnitFlashResult : uint8_t {
  Ok = 0,
  BootloaderSilent,  // twiboot never ACKed a ping at this address
  ChipMismatch,      // chipinfo signature / page size not an ATmega328P
  PageFailed,        // write / readback-verify failed after one rewrite
  ExitFailed,        // SWITCH_APPLICATION not ACKed
  PostBootSilent,    // sketch did not answer after the exit
  Aborted,           // /stop during the page stream — unit left in twiboot
};
const char* unitFlashResultName(UnitFlashResult r);

// Streams `image` (page-padded, TWIBOOT_PAGE_SIZE multiple) to the unit at
// `i2cAddress`, which must already be in twiboot (the orchestration's
// enter-bootloader sweep + rescan guarantees it): ping retry → chipinfo
// verify → write+readback per page (one rewrite, v1 #110) → exit → post-boot
// ACK + CMD_REBOOT for a clean watchdog restart (v1 #113).
UnitFlashResult unitBusFlashUnit(int i2cAddress, const uint8_t* image,
                                 size_t len);

// Polls a just-flashed batch until every unit answers AND reports
// not-rotating (post-flash homing finished), or the timeout elapses —
// bounds how many units draw homing current at once (v1 #138). Bus-safety
// pacing: deliberately NOT abort-shortened.
void unitBusWaitBatchIdle(const uint8_t* addrs, int count,
                          uint32_t timeoutMs);

// Stop-abort signal (#204): the ONE cross-task entry into this module — an
// atomic flag, not bus state. The /stop handler sets it BEFORE enqueuing the
// Stop command and rolls it back if the enqueue 503s (set-after-enqueue races
// an idle displayTask clearing it first, stranding the flag ON); every wait
// loop polls it and returns early; displayTask clears it when Stop executes.
// The bus itself stays displayTask-exclusive. The reflash orchestration polls
// it between units and batches via unitBusAbortRequested() (#205).
void unitBusRequestAbort();
void unitBusClearAbort();
bool unitBusAbortRequested();
