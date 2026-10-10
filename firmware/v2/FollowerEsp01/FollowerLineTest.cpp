// FollowerLineTest glue (#496). Header owns the rationale. Bench-tier: GPIO
// registers and the CPU's cycle counter.

#include "FollowerLineTest.h"

#include <Arduino.h>

#include "FollowerBusDeath.h"
#include "FollowerConfig.h"

#if SERIAL_ENABLE == false
namespace {
// The pins of Wire.begin(1, 3) in FollowerBus.cpp.
constexpr uint8_t SDA_PIN = 1;
constexpr uint8_t SCL_PIN = 3;
// How long a line may take before it counts as not rising at all.
constexpr uint32_t NO_RISE_US = 1000;

// The core's bit-banged bus leaves the pins as inputs with the chip's own
// pull-up on and pulls a line low by enabling its output (the latch is 0).
// The pull-up is switched off for the measurement and back on after it.
uint16_t riseOf(uint8_t pin) {
  const uint32_t mask = 1UL << pin;
  if (!(GPI & mask)) return FOLLOWER_LINE_NOT_MEASURED;  // held low: nothing to time
  const uint32_t mhz = ESP.getCpuFreqMHz();
  const uint32_t cap = mhz * NO_RISE_US;
  GPF(pin) &= ~(1 << GPFPU);
  GPES = mask;  // low
  delayMicroseconds(50);
  noInterrupts();
  const uint32_t t0 = esp_get_cycle_count();
  GPEC = mask;  // let go
  uint32_t dt = 0;
  while (!(GPI & mask) && dt < cap) dt = esp_get_cycle_count() - t0;
  dt = esp_get_cycle_count() - t0;
  interrupts();
  const bool rose = (GPI & mask) != 0;
  GPF(pin) |= (1 << GPFPU);
  return rose ? followerLineTenths(dt, mhz) : FOLLOWER_LINE_NO_RISE;
}
}  // namespace
#endif

FollowerLineReading followerLineTest() {
  FollowerLineReading r;
#if SERIAL_ENABLE == false
  const uint32_t both = (1UL << SDA_PIN) | (1UL << SCL_PIN);
  if ((GPI & both) != both) return r;  // a held line: not an idle bus
  r.scl = riseOf(SCL_PIN);
  delayMicroseconds(20);
  r.sda = riseOf(SDA_PIN);
  delayMicroseconds(20);
#endif
  return r;
}
