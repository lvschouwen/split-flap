#pragma once
// StatusLed — glue between StatusLedPolicy.h (pure, natively tested) and
// the onboard WS2812 (#199). The pin is the HW-678 / YD-ESP32-S3 family's
// GPIO 48 (DevKitC-1 v1.1 boards route it to 38 — override STATUS_LED_PIN
// in build_flags if the boot-white blink stays dark). Driven by the
// Arduino core's rgbLedWrite() over the RMT peripheral; no library.

#include <Arduino.h>

#include "Settings.h"

// setup() context, after loadSettings(): stores the settings reference and
// immediately shows the boot white — doubling as the GPIO sanity check.
void statusLedInit(const MasterSettings& settings);

// netTask context only (reads WifiService's task-private phase). Cheap:
// self-limits to a 10 Hz evaluation and writes the LED only on change.
void statusLedTick();
