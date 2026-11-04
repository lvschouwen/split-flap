#pragma once

// Internal seam of the FollowerBus*.cpp family: the Wire adapter and the
// state the bus files share. Include from FollowerBus*.cpp ONLY — these names
// have external linkage across the family (definitions in FollowerBus.cpp)
// and are not part of the public FollowerBus.h surface.
//
// The adapter stays at global scope: the shared unit-bus templates
// instantiated on it then fold into one copy across the family. Never include
// UnitAssets.h here — its PROGMEM image has internal linkage and would be
// copied into every file that includes this header.

#include <Arduino.h>
#include <Wire.h>

#include "FollowerBus.h"
#include "FollowerBusRecovery.h"
#include "FollowerConfig.h"
#include "SplitFlapProtocol.h"
#include "TwibootFlash.h"
#include "UnitBusCore.h"

// Bus health counters (FollowerBus.cpp).
extern uint32_t busTxCount;
extern uint32_t busErrCount;

// The unit protocol is shared/UnitBusCore.h and the twiboot protocol
// shared/TwibootFlash.h; this is the Wire adapter both run on. No bus rebuild
// after a short read here: that is an ESP32 I2C driver quirk (#207).
namespace {
struct WireTwibootBus {
  void beginTransmission(uint8_t addr) { Wire.beginTransmission(addr); }
  int endTransmission(bool stop) { return Wire.endTransmission(stop); }
  size_t write(uint8_t b) { return Wire.write(b); }
  uint8_t requestFrom(uint8_t addr, uint8_t qty) {
    return Wire.requestFrom(addr, qty);
  }
  int read() { return Wire.read(); }
  int available() { return Wire.available(); }
  uint32_t nowMs() { return millis(); }
  void sleepMs(uint32_t ms) { delay(ms); }
  void readFailed() {}
  int endCounted() {
    int status = Wire.endTransmission();
    busTxCount++;
    if (status != 0) busErrCount++;
    return status;
  }
  void noteReadError() { busErrCount++; }
  void mark(UnitBusAct, uint8_t) {}
};
WireTwibootBus unitBus;

bool isUnitInBootloader(int i2cAddress) {
  return twibootIsBootloader(unitBus, (uint8_t)i2cAddress);
}
}  // namespace

extern UnitErrorLedger<UNITS_AMOUNT> unitErrors;

// The last frame written, re-shown by the recovery tick when staged.
extern bool reshowPending;
extern String lastFrame;
extern int lastFrameSpeed;
extern bool lastFrameValid;

// When this row last started a move: what a bus death is dated against.
extern uint32_t lastMoveMs;
extern bool everMoved;

// Row-wide bus-death recovery (FollowerBus.cpp writes it; the rescue tick
// stands down while the bus is dead).
extern BusRecoveryState busRecovery;

inline int toI2cAddress(int unitIndex) {
  return SFP_I2C_ADDRESS_BASE + unitIndex;
}

// No motion while the radio is in a high-draw phase, bounded
// (FollowerBusRender.cpp).
void admitMotion();

#if SERIAL_ENABLE == false
void pollHealthAll();          // FollowerBus.cpp
void settleReadTick();         // FollowerBusRender.cpp
void motionBudgetFold(int i);  // FollowerBusRender.cpp
void rescueTick(int i);        // FollowerBusFlash.cpp
#endif
