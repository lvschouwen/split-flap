// MasterS3 — ESP32-S3 firmware, Phase 2 scaffold (#63).
// Blinks the three status LEDs (WiFi/Activity/Zigbee) in a binary count
// and prints a heartbeat to USB-CDC so we can verify the chip is alive
// in Wokwi or on real hardware.

#include <Arduino.h>

namespace pins {
constexpr uint8_t led_wifi = 4;  // PCB net LED_WIFI_DRV
constexpr uint8_t led_act  = 5;  // PCB net LED_ACT_DRV
constexpr uint8_t led_zb   = 6;  // PCB net LED_ZB_DRV
}

void setup() {
  Serial.begin(115200);
  pinMode(pins::led_wifi, OUTPUT);
  pinMode(pins::led_act, OUTPUT);
  pinMode(pins::led_zb, OUTPUT);
  delay(500);
  Serial.println("[S3] alive");
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
