// MasterH2 — ESP32-H2 firmware, Phase 2 scaffold (#63).
// Blinks an LED and prints a UART0 heartbeat so we can verify the chip
// boots in Wokwi or on real hardware. The real PCB doesn't carry an
// H2-side LED — this pin is for sim/dev only and will be removed once
// the UART link to S3 is alive (Phase 3).

#include <Arduino.h>

// GPIO8 = onboard LED on the ESP32-H2-DevKitM-1 reference board.
// On the real Master v2 PCB this pin is "no copper escape" (strap pin
// reserved for H2 ROM_MSG_PRINT) — see PINOUT.md. Sim/dev only.
constexpr uint8_t kLedPin = 8;

void setup() {
  Serial.begin(115200);
  pinMode(kLedPin, OUTPUT);
  delay(500);
  Serial.println("[H2] alive");
}

void loop() {
  static uint32_t tick = 0;
  digitalWrite(kLedPin, (tick & 1) ? HIGH : LOW);
  Serial.printf("[H2] tick=%lu\n", static_cast<unsigned long>(tick));
  tick++;
  delay(500);
}
