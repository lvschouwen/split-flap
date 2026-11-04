// FollowerBusOps.cpp — single-unit operations on the unit bus. Shared state
// is FollowerBusInternal.h.

#include "FollowerBus.h"
#include "FollowerBusInternal.h"

#include "FollowerConfig.h"
#include "HomeWait.h"  // what a row sent home says of it (#605)
#include "UnitBusCore.h"

// --- single-unit ops ---------------------------------------------------------------

// The frames, verifies and settle times are shared/UnitBusCore.h; an op that
// starts motion first waits for a quiet radio (#505).
int busWriteOffset(uint8_t i2cAddress, int16_t value) {
  return unitWriteOffset(unitBus, i2cAddress, value);
}

int busJog(uint8_t i2cAddress, int steps) {
  admitMotion();
  return unitJog(unitBus, i2cAddress, steps);
}

int busHome(uint8_t i2cAddress) {
  admitMotion();
  return unitHome(unitBus, i2cAddress);
}

int busIdentify(uint8_t i2cAddress) { return unitIdentify(unitBus, i2cAddress); }

int busSetAddress(uint8_t i2cAddress, uint8_t newAddress) {
  return unitSetAddress(unitBus, i2cAddress, newAddress);
}

int busClearAddress(uint8_t i2cAddress) { return unitClearAddress(unitBus, i2cAddress); }

// The speed a row shows at before the master has named one.
void busReshowLastFrame() { reshowPending = lastFrameValid; }

#define HOME_ALL_SPEED 80

void busHomeAll() {
  const bool hadText = lastFrameValid;
  const String text = lastFrame;
  // A row that has shown nothing yet has no speed of its own.
  const int speed = hadText ? lastFrameSpeed : HOME_ALL_SPEED;
  String row;
  for (int i = 0; i < displayWidth; i++) row += '-';
  busShowSegment(row, speed);
  delay(2000);
  for (int i = 0; i < displayWidth; i++) row[i] = '.';
  busShowSegment(row, speed);
  if (hadText) {
    busShowSegment(text, speed);
  } else {
    lastFrameValid = false;  // the dots are not this row's text
  }
}

bool busReadStatus(uint8_t i2cAddress, UnitStatus& out) {
  return unitReadStatus(unitBus, i2cAddress, out);
}

MaintGrade busGradeHomeAll() {
  HomeAllTally tally;
  for (int i = 0; i < displayWidth; i++) {
    if (!unitDrivable(unitFacts[i])) continue;
    UnitStatus status;
    const bool readOk = busReadStatus((uint8_t)toI2cAddress(i), status);
    homeAllTallyAdd(tally, readOk, status.flags);
  }
  return maintGradeHomeAll(tally);
}

int busResetOdometer(uint8_t i2cAddress) {
  return unitResetOdometer(unitBus, i2cAddress);
}

int busSetGates(uint8_t i2cAddress, uint8_t gates) {
  return unitSetGates(unitBus, i2cAddress, gates);
}

int busRebootToBootloader(uint8_t i2cAddress) {
  return unitEnterBootloader(unitBus, i2cAddress);
}

int busStartSelfTest(uint8_t i2cAddress) {
  admitMotion();
  return unitStartSelfTest(unitBus, i2cAddress);
}

bool busReadSelfTest(uint8_t i2cAddress, UnitSelfTestReading& out) {
  return unitReadSelfTest(unitBus, i2cAddress, out);
}

bool busReadBootInfo(uint8_t i2cAddress, BootUpdateReport& out) {
  return unitReadBootInfo(unitBus, i2cAddress, out);
}

int busBootUpdate(uint8_t i2cAddress, uint8_t stage) {
  return unitSendBootUpdate(unitBus, i2cAddress, stage);
}

void busInvalidateUnitReads(uint8_t i2cAddress) {
  int idx = i2cAddress - SFP_I2C_ADDRESS_BASE;
  if (idx < 0 || idx >= UNITS_AMOUNT) return;
  unitFactsInvalidateReads(unitFacts[idx]);
}
