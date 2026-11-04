// FollowerBusRender.cpp — the row's render path: the settle read, motion
// admission (#505), the stop wait and busShowSegment. Shared state is
// FollowerBusInternal.h.

#include "FollowerBus.h"
#include "FollowerBusInternal.h"

#include "FlapLetters.h"  // shared character/speed mapping
#include "FollowerConfig.h"
#include "FollowerWifi.h"  // followerRadioBusy (#505)
#include "MotionBudget.h"  // motion admission (#505)
#include "RenderStagger.h"  // sub-frame inrush stagger (#324)
#include "UnitBusCore.h"
#include "UnitTimings.h"

static uint32_t rendersSettled = 0;
static bool settleReadPending = false;

uint32_t busRendersSettled() { return rendersSettled; }

#if SERIAL_ENABLE == false
// A settled render's re-read of every unit, and only then the count that
// sends the unit facts (#640): never inside a twiboot window or a reflash.
void settleReadTick() {
  if (!settleReadPending) return;
  if ((int32_t)(millis() - busProbeInhibitedUntilMs()) < 0) return;
  if (reflashInProgress(reflashProgress)) return;
  settleReadPending = false;
  busPollHealth();
  rendersSettled++;
}
#endif

// A render's write; a failure is charged to its unit (#367).
static int writeToUnit(int unitIndex, int letter, int speed) {
  int status = unitWriteLetter(unitBus, (uint8_t)toI2cAddress(unitIndex),
                               (uint8_t)letter, (uint8_t)speed);
  if (status != 0) unitErrors.note(unitIndex, millis());
  return status;
}

// 0 idle, 1 rotating, -1 offline.
static int checkIfMoving(int unitIndex) {
  return unitMovingStatus(unitBus, (uint8_t)toI2cAddress(unitIndex));
}

static bool isRowMoving() {
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    if (!unitDrivable(unitFacts[i])) continue;  // #405
    if (checkIfMoving(i) == 1) return true;
  }
  return false;
}

bool busRowMoving() {
#if SERIAL_ENABLE == false
  // A unit in its twiboot window (v1 #88) or a streaming reflash must not be
  // read: report "moving" so the caller waits instead.
  if ((int32_t)(millis() - busProbeInhibitedUntilMs()) < 0) return true;
  if (reflashInProgress(reflashProgress)) return true;
  return isRowMoving();
#else
  return false;
#endif
}

// --- motion admission (#505, MotionBudget.h) ---------------------------------
static MotionRadioGate radioGate;
static MotionBudgetState motionBudget;

// No motion while the radio is in a high-draw phase, bounded.
void admitMotion() {
  // Every move this row starts passes here: what a bus death is dated against.
  lastMoveMs = millis();
  everMoved = true;
  uint32_t holdStart = millis();
  bool logged = false;
  for (;;) {
    motionRadioObserve(radioGate, followerRadioBusy(), millis());
    if (motionRadioQuiet(radioGate, millis())) return;
    if (motionRadioHoldExpired(holdStart, millis())) {
      SerialPrintln(F("motion: radio still busy after the hold cap — moving anyway"));
      return;
    }
    if (!logged) {
      SerialPrintln(F("motion: holding unit moves while the radio is busy"));
      logged = true;
    }
    delay(100);
  }
}

// Blocks until fewer than the budget's cap of tracked units still move.
static void waitForMotionSlot(MotionTracker& movers) {
  while (motionTrackerFull(movers, motionBudget.cap)) {
    for (int k = movers.count - 1; k >= 0; k--) {
      motionTrackerObserve(movers, k, checkIfMoving(movers.unit[k]), millis());
    }
    if (motionTrackerFull(movers, motionBudget.cap)) delay(50);
  }
}

// Folds unit i's since-boot supply minimum into the cap (heartbeat cadence).
void motionBudgetFold(int i) {
  uint32_t now = millis();
  motionRadioObserve(radioGate, followerRadioBusy(), now);
  const UnitFacts& u = unitFacts[i];
  if (u.vitalsValid &&
      motionBudgetObserveVmin(motionBudget, i, u.vitals.vccMin_mV, now)) {
    SerialPrint(F("motion: rail sag "));
    SerialPrint(u.vitals.vccMin_mV);
    SerialPrint(F(" mV at unit "));
    SerialPrint(toI2cAddress(i));
    SerialPrint(F(" — at most "));
    SerialPrint(motionBudget.cap);
    SerialPrintln(F(" unit(s) move at once"));
  } else if (motionBudgetTick(motionBudget, now)) {
    SerialPrint(F("motion: rail quiet — at most "));
    SerialPrint(motionBudget.cap);
    SerialPrintln(F(" unit(s) move at once"));
  }
}

// v1 waitForDisplayToStop, minus the /stop abort (this firmware has no
// local producers and serves no /stop — the stuck timeout is the bound).
static void waitForRowToStop() {
  uint32_t waitStart = millis();
  while (isRowMoving()) {
    if (millis() - waitStart > UNIT_SHOW_STUCK_TIMEOUT_MS) {
      SerialPrintln(F("Row-stop wait timed out — a unit may be stuck"));
      break;
    }
    delay(100);
  }
}


void busShowSegment(const String& segment, int webSpeed) {
#if SERIAL_ENABLE == false
  const int width = displayWidth;
  const int speed = convertSpeedToUnit(webSpeed);
  // Segments arrive pre-positioned from the leader: pad/truncate to the
  // probed width, no alignment pass.
  String frame = segment;
  while ((int)frame.length() < width) frame += ' ';
  lastFrame = segment;
  lastFrameSpeed = webSpeed;
  lastFrameValid = true;

  waitForRowToStop();
  admitMotion();

  MotionTracker movers;
  int commanded[UNITS_AMOUNT];
  for (int i = 0; i < UNITS_AMOUNT; i++) commanded[i] = -1;

  int commandedCount = 0;
  for (int i = 0; i < width; i++) {
    if (!unitDrivable(unitFacts[i])) continue;  // #405
    int letter = flapLetterOrBlank(frame[i]);
    // #505: at most the budget's cap moving at once — the steady draw of
    // every energised stepper, not just the start spike, sags the rail.
    waitForMotionSlot(movers);
    // #324: spread the flap inrush — pause before opening each new group so a
    // full row's steppers don't spin up at once and brown out the rail.
    if (renderStaggerShouldSettle(commandedCount, RENDER_STAGGER_BATCH)) {
      delay(RENDER_STAGGER_SETTLE_MS);
    }
    writeToUnit(i, letter, speed);
    motionTrackerAdd(movers, i, millis());
    commanded[i] = letter;
    commandedCount++;
  }

  waitForRowToStop();

  // Closed-loop verification (v1 #106): read back, re-send once on mismatch.
  int resent = 0;
  MotionTracker resendMovers;
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    if (commanded[i] < 0 || !unitDrivable(unitFacts[i])) continue;  // #405
    int shown;
    if (!unitReadDisplayedLetter(unitBus, (uint8_t)toI2cAddress(i), shown)) {
      continue;
    }
    if (shown == commanded[i]) continue;
    waitForMotionSlot(resendMovers);  // resends are moves too (#505)
    writeToUnit(i, commanded[i], speed);
    motionTrackerAdd(resendMovers, i, millis());
    resent++;
  }
  if (resent > 0) waitForRowToStop();

  // #640: what the drums now read, for the master at once — the heartbeat
  // would take a round of the row. In a twiboot window or a reflash the
  // heartbeat reads them once the bus is free again.
  settleReadPending = true;
  settleReadTick();
#else
  SerialPrint(F("Row shows: \""));
  SerialPrint(segment);
  SerialPrint(F("\" speed "));
  SerialPrintln(webSpeed);
#endif
}
