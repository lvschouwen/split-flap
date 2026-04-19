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
