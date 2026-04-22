// MasterS3 — ESP32-S3 firmware, Phase 2 scaffold (#63 + polish #65).
//
// Phase 2 responsibilities:
//   - Boot, blink the three status LEDs (placeholders for IO4/5/6 — these
//     pins move to TLC5947 SPI in the LED-bar PCB rev, see #64)
//   - Init LittleFS (so the partition is exercised at boot and we know the
//     filesystem mount works)
//   - Init Preferences (NVS-backed key-value, the canonical settings store
//     for ESP32 — clean break from ESP8266's flat-byte EEPROM)
//   - Print a heartbeat to USB-CDC with the boot count from Preferences,
//     so we can verify both subsystems work in Wokwi or on hardware.
//
// Real features (WiFi, web UI, OTA, I2C, S3↔H2 UART) land Phase 3+.

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>

#include "config.h"  // splitflap::kMaxRows etc. from firmware/lib/common/

namespace pins {
constexpr uint8_t led_wifi = 4;  // PCB net LED_WIFI_DRV (LED-bar rev: TLC5947)
constexpr uint8_t led_act  = 5;  // PCB net LED_ACT_DRV  (LED-bar rev: TLC5947)
constexpr uint8_t led_zb   = 6;  // PCB net LED_ZB_DRV   (LED-bar rev: H2 IO12)
}

namespace {

void initLittleFs() {
  if (LittleFS.begin(true)) {
    Serial.printf("[S3] LittleFS OK (%u/%u bytes used)\n",
                  static_cast<unsigned>(LittleFS.usedBytes()),
                  static_cast<unsigned>(LittleFS.totalBytes()));
  } else {
    Serial.println("[S3] LittleFS FAIL — mount + format both refused");
  }
}

uint32_t bumpBootCounter() {
  Preferences prefs;
  if (!prefs.begin("smoke", false)) {
    Serial.println("[S3] Preferences FAIL — could not open 'smoke' namespace");
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
  pinMode(pins::led_wifi, OUTPUT);
  pinMode(pins::led_act, OUTPUT);
  pinMode(pins::led_zb, OUTPUT);
  delay(500);

  Serial.println("[S3] alive");
  Serial.printf("[S3] config: kMaxRows=%u, kMaxUnitsPerRow=%u, kMaxUnits=%u\n",
                splitflap::kMaxRows, splitflap::kMaxUnitsPerRow,
                splitflap::kMaxUnits);

  initLittleFs();
  Serial.printf("[S3] Preferences OK (boot count = %u)\n",
                static_cast<unsigned>(bumpBootCounter()));
}

void loop() {
  static uint32_t tick = 0;
  digitalWrite(pins::led_wifi, (tick & 0b001) ? HIGH : LOW);
  digitalWrite(pins::led_act,  (tick & 0b010) ? HIGH : LOW);
  digitalWrite(pins::led_zb,   (tick & 0b100) ? HIGH : LOW);
  Serial.printf("[S3] tick=%lu\n", static_cast<unsigned long>(tick));
  tick++;
  delay(500);
}
