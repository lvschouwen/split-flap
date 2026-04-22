// MasterH2 — ESP32-H2 firmware, Phase 2 scaffold (#63 + polish #65).
//
// Phase 2 responsibilities:
//   - Boot, blink an LED + UART heartbeat (placeholder until LED-bar rev #64
//     opens copper escape on H2 IO11/12/13 for heartbeat/Zigbee/BLE LEDs)
//   - Init LittleFS to exercise the partition mount
//   - Init Preferences (NVS) to exercise the settings store
//   - Print boot count so we can verify both subsystems
//
// UART link to S3 (UART1 on IO4/IO5, COBS+CRC framing) lands Phase 3.

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>

#include "config.h"  // splitflap::kMaxRows etc. from firmware/lib/common/

// GPIO8 = onboard LED on the ESP32-H2-DevKitM-1 reference board.
// On the real Master v2 PCB the heartbeat LED moves to H2 IO11 (LED-bar #64).
constexpr uint8_t kLedPin = 8;

namespace {

void initLittleFs() {
  if (LittleFS.begin(true)) {
    Serial.printf("[H2] LittleFS OK (%u/%u bytes used)\n",
                  static_cast<unsigned>(LittleFS.usedBytes()),
                  static_cast<unsigned>(LittleFS.totalBytes()));
  } else {
    Serial.println("[H2] LittleFS FAIL");
  }
}

uint32_t bumpBootCounter() {
  Preferences prefs;
  if (!prefs.begin("smoke", false)) {
    Serial.println("[H2] Preferences FAIL");
    return 0;
  }
  uint32_t count = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", count);
  prefs.end();
  return count;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(kLedPin, OUTPUT);
  delay(500);

  Serial.println("[H2] alive");
  Serial.printf("[H2] config: kMaxRows=%u, kMaxUnitsPerRow=%u, kMaxUnits=%u\n",
                splitflap::kMaxRows, splitflap::kMaxUnitsPerRow,
                splitflap::kMaxUnits);

  initLittleFs();
  Serial.printf("[H2] Preferences OK (boot count = %u)\n",
                static_cast<unsigned>(bumpBootCounter()));
}

void loop() {
  static uint32_t tick = 0;
  digitalWrite(kLedPin, (tick & 1) ? HIGH : LOW);
  Serial.printf("[H2] tick=%lu\n", static_cast<unsigned long>(tick));
  tick++;
  delay(500);
}
