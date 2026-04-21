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

// Unit health / diagnostics (issue #47). Populated by readUnitStatus() from
// the 8-byte CMD_GET_STATUS reply. Mirrors the layout documented in
// Unit.ino's requestEvent().
struct UnitStatus {
  uint8_t  flags;                    // bit0 moving, bit1 last-home-failed, bit2 hall-never-triggered
  uint8_t  mcusrAtBoot;              // BORF / WDRF / EXTRF / PORF / JTRF snapshot
  uint8_t  lifetimeBrownoutCount;    // saturating
  uint8_t  lifetimeWatchdogCount;    // saturating
  uint16_t uptimeSeconds;            // saturating
  uint8_t  badCommandCount;          // saturating
  uint16_t lastHomingStepCount;      // decoded from byte 7 * 16
};
// Reads the 8-byte status payload from a sketch-running unit. Returns true
// on success. On short reply (old firmware predating CMD_GET_STATUS) or
// Wire error, returns false and `out` is untouched.
bool readUnitStatus(int i2cAddress, UnitStatus& out);

// Pretty-prints UnitStatus for every sketch-mode unit to Serial (which
// also lands in the MQTT log topic via the web log ring + sink). Called
// at boot, on POST /stop, and on a long-interval tick from loop().
// Defined in ServiceFlapFunctions.ino. Issue #50.
void dumpAllUnitStatusSerial(bool compact);

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

// MQTT service (ServiceMqttFunctions.ino, issue #50). All functions no-op if
// mqttHostSetting is empty (MQTT disabled) — safe to call unconditionally.
//
// mqttInit():          called once from setup() after settings load. Registers
//                      event handlers and kicks off the first connect attempt.
// mqttServiceLoop():   pumped from loop(); handles reconnect backoff and the
//                      isPendingMqttReconfig flag.
// mqttIsConnected():   true only after onConnect fires.
void mqttInit();
void mqttServiceLoop();
bool mqttIsConnected();

// Pre-connect log ring flush. Called from mqttInit() after connect completes;
// drains the WebLog ring into the `log` topic, then resets it. Defined in
// ServiceMqttFunctions.ino. Safe no-op when MQTT is disabled or disconnected.
void mqttFlushLogRing();
