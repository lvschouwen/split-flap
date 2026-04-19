#pragma once

#include <Arduino.h>
#include <FS.h>

// Forward declarations for functions the Arduino sketch preprocessor
// can't auto-prototype (parameters with namespace-qualified reference
// types like `fs::FS&`). Kept minimal on purpose — regular functions
// continue to rely on Arduino's auto-prototyping.

String readFile(fs::FS& fs, const char* path, String defaultValue);
void writeFile(fs::FS& fs, const char* path, const char* message);

// Populated by probeI2cBus() in ServiceFlapFunctions.ino and read by
// getCurrentSettingValues() in ESPMaster.ino (which comes earlier in the
// alphabetical concatenation order, so needs the forward declaration).
extern int detectedUnitCount;
extern int detectedUnitAddresses[];

// Defined in ServiceFlapFunctions.ino; called from the /unit/reboot endpoint
// handler registered in ESPMaster.ino (earlier in the concat order).
int rebootUnitToBootloader(int i2cAddress);

// Firmware flashing (ServiceFirmwareFunctions.ino). `firmwareFlashInProgress`
// is checked by the main loop so we don't step on the Wire bus while a
// flash is active. The begin/feed/finish/abort helpers drive the upload
// state machine from the /firmware/unit HTTP handler.
extern volatile bool firmwareFlashInProgress;
bool beginFirmwareFlash(uint8_t i2cAddress, String& error);
bool feedFirmwareChunk(const uint8_t* data, size_t len);
bool finishFirmwareFlash(String& resultMsg);
void abortFirmwareFlash(const String& reason);
