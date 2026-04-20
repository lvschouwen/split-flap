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

// Defined in ServiceFlapFunctions.ino; called from the /unit/reboot endpoint
// handler registered in ESPMaster.ino (earlier in the concat order).
int rebootUnitToBootloader(int i2cAddress);

// Firmware flashing (ServiceFirmwareFunctions.ino). `firmwareFlashInProgress`
// is checked by the main loop so we don't step on the Wire bus while a
// flash is active. flashUnitFromProgmem() is the only caller of the
// begin/finish helpers now that the HEX upload path is gone.
extern volatile bool firmwareFlashInProgress;
bool beginFirmwareFlash(uint8_t i2cAddress, String& error);
bool finishFirmwareFlash(String& resultMsg);
void abortFirmwareFlash(const String& reason);
