#pragma once

#include <Arduino.h>

// Forward declarations for symbols referenced before the Arduino
// preprocessor can auto-prototype them (cross-TU references into files
// that appear later in alphabetical concatenation order, or templates).

// Populated by probeI2cBus() in ServiceFlapFunctions.ino and read by
// getCurrentSettingValues() in ESPMaster.ino (which comes earlier in the
// alphabetical concatenation order, so needs the forward declaration).
extern int detectedUnitCount;
extern int detectedUnitAddresses[];
// Per-unit state from probeI2cBus(): 0 = silent, 1 = sketch, 2 = bootloader.
extern int detectedUnitStates[];
// Per-unit firmware status, populated alongside detectedUnitAddresses.
//   0 = ok (unit reported the same rev the master was built with)
//   1 = outdated (unit reported a different rev)
//   2 = unknown (unit didn't reply with a valid 8-byte rev — older firmware)
extern int detectedUnitVersionStatus[];
// 8 chars + null terminator. Empty string when no valid version was returned.
extern char detectedUnitVersions[][9];

// Defined in ServiceFlapFunctions.ino; called from the /unit/reboot endpoint
// handler registered in ESPMaster.ino (earlier in the concat order).
int rebootUnitToBootloader(int i2cAddress);

// Interactive calibration helpers (issue #32). Defined in
// ServiceFlapFunctions.ino, called from HTTP endpoints in ESPMaster.ino.
bool readUnitOffset(int i2cAddress, int16_t &out);
int  writeUnitOffset(int i2cAddress, int16_t value);
int  jogUnit(int i2cAddress, int steps);
int  homeUnit(int i2cAddress);

// Unit health / diagnostics (issue #47). The UnitStatus struct + pure logic
// (faulty predicate, faulty count, JSON assembly) live in UnitHealth.h so they
// are natively testable; this header only declares the Wire-touching + poll
// pieces that can't be.
#include "UnitHealth.h"
// Reads the 8-byte status payload from a sketch-running unit. Returns true
// on success. On short reply (old firmware predating CMD_GET_STATUS) or
// Wire error, returns false and `out` is untouched.
bool readUnitStatus(int i2cAddress, UnitStatus& out);

// Poll cache for the unit-health web card (#45) + MQTT telemetry (#137).
// Populated by pollUnitHealth() in ServiceFlapFunctions.ino — blocking I2C, so
// loop() context only (piggybacks the 60 s MQTT telemetry tick, plus an
// on-demand refresh armed by POST /units/health/refresh). unitHealthJson is the
// shared payload served verbatim by GET /units/health AND published to the MQTT
// json_attributes_topic; faultyUnitCount feeds the integer HA alerting sensor.
extern UnitStatus unitHealth[];
extern bool       unitHealthValid[];
extern char       unitHealthJson[];
extern int        faultyUnitCount;
extern volatile bool unitHealthRefreshPending;
void pollUnitHealth();

// Broadcasts CMD_HOME to the I2C general-call address (0x00). Every unit
// with TWGCE enabled will run calibrate(true). Replaces N-unit sequential
// home loops used by the Stop button and the reset-calibration flow.
int  broadcastHome();
int  rebootUnit(int i2cAddress);     // soft WDT reset, stays in sketch mode

// Firmware flashing (ServiceFirmwareFunctions.ino). `firmwareFlashInProgress`
// is checked by the main loop so we don't step on the Wire bus while a
// flash is active. flashUnitFromProgmem() is the only caller of the
// begin/finish helpers now that the HEX upload path is gone.
extern volatile bool firmwareFlashInProgress;
// Deferred + throttled unit reflash (#138). POST /reflash-units only arms
// `reflashUnitsPending`; the blocking bootloader-entry + PROGMEM flash runs in
// loop() via runPendingUnitReflash(), never in the async handler, and paces the
// flash in small batches so post-flash homing spikes can't brown out a supply
// shared with the steppers.
extern volatile bool reflashUnitsPending;
// Persistent flag: true for the whole duration of runPendingUnitReflash() so
// every Wire-touching async endpoint can refuse to inject I2C mid-flash (#138).
extern volatile bool unitReflashRunning;
void runPendingUnitReflash();
// Abort flag for the showMessage wait loop. Set by POST /stop, consumed
// (and cleared) by showMessage(). Defined in ESPMaster.ino. Issue #35.
extern volatile bool abortCurrentShow;
bool beginFirmwareFlash(uint8_t i2cAddress, String& error);
bool finishFirmwareFlash(String& resultMsg);
void abortFirmwareFlash(const String& reason);
void autoInstallFirmwareToBootloaderUnits();
// Reboots OUTDATED sketch-running units into twiboot, re-probes, and runs
// autoInstallFirmwareToBootloaderUnits() to push the bundled PROGMEM hex.
// Defined in ServiceFirmwareFunctions.ino; called from setup().
void autoUpdateOutdatedUnits();
int enterBootloaderAllDetected(bool reprobeAfter);

// Wire-level write failures from the last showMessage() pass. Defined in
// ESPMaster.ino, updated by ServiceFlapFunctions.ino, published by the MQTT
// telemetry in ServiceMqttFunctions.ino (#121).
extern int lastShowUnitWriteErrors;

// MQTT / Home Assistant integration (issue #121, runtime config #57).
// Defined in ServiceMqttFunctions.ino; everything is inert until a broker
// host is configured via the web UI (EEPROM, applied on reboot).
void initMqtt();
void loopMqtt();
bool mqttNotificationTick();
void mqttStopForOta();
void mqttResumeAfterOta();
bool mqttIsConnected();
