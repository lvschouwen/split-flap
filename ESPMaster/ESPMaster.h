#pragma once

#include <Arduino.h>
#include <FS.h>

// Forward declarations for functions the Arduino sketch preprocessor
// can't auto-prototype (parameters with namespace-qualified reference
// types like `fs::FS&`). Kept minimal on purpose — regular functions
// continue to rely on Arduino's auto-prototyping.

String readFile(fs::FS& fs, const char* path, String defaultValue);
void writeFile(fs::FS& fs, const char* path, const char* message);
