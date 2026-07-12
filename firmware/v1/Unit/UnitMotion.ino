// Stepper motion + hall-sensor calibration, extracted from Unit.ino (#175).
// Same single-translation-unit rules as UnitI2CProtocol.ino: Unit.ino's
// globals/#defines are visible here (main sketch concatenates first).
// The blocking step loops here kick the 8 s watchdog per iteration.

//Single funnel for every drum move (#231): steps the motor and folds the
//magnitude into the revolution odometer. All motion runs in loop context,
//so the plain-state update is safe; the ISR-visible mirror write must be
//interrupt-guarded (4 bytes — torn-read class #96) and only happens when a
//revolution actually completed.
void stepCounted(int steps) {
  stepper.step(steps);
  odometerAddSteps(odometer, steps, STEPS);
  if (odometer.revolutions != odometerRevolutions) {
    noInterrupts();
    odometerRevolutions = odometer.revolutions;
    interrupts();
  }
}

//Steps the drum forward by `flaps` flap-positions. Each flap is
//STEPS_PER_FLAP_WHOLE whole steps plus a fractional remainder accumulated in
//missedSteps — when it exceeds one step we add a step and subtract, keeping
//cumulative drift below one flap. Caller must have set the speed and started
//the motor. Extracted from the two identical loops in rotateToLetter (#136).
void stepFlaps(int flaps) {
  for (int i = 0; i < flaps; i++) {
    wdt_reset(); //a many-flap move at low speed exceeds the 8 s window (#107)
    int roundedStep = STEPS_PER_FLAP_WHOLE;
    missedSteps += STEPS_PER_FLAP_FRAC;
    if (missedSteps > 1) {
      roundedStep++;
      missedSteps--;
    }
    stepCounted(ROTATIONDIRECTION * roundedStep);
  }
}

//rotate to letter
void rotateToLetter(int toLetter) {
  // Defensive bounds check (#136): receiveLetter() already constrains letter
  // indices to 0..AMOUNTFLAPS-1, but a corrupted target must never drive the
  // step loop below into a runaway motor spin. Kept before the overheat gate so
  // an invalid target can never latch currentlyrotating = 1 below and leave the
  // unit reporting busy forever — it just no-ops, staying idle.
  if (toLetter < 0 || toLetter >= AMOUNTFLAPS) {
    return;
  }

  // Anti-overheat gate: after a rotation starts, the stepper won't move again
  // until OVERHEATINGTIMEOUT has passed. While a target is pending but the gate
  // hasn't cleared, report "busy" so the master's waitForDisplayToStop() waits
  // for the deferred move instead of running its #106 verify pass against the
  // stale displayedLetter. loop() keeps re-calling us (displayedLetter !=
  // receivedNumber) until the gate opens and the move actually runs (#135).
  if (!(lastRotation == 0 || (millis() - lastRotation > OVERHEATINGTIMEOUT * 1000UL))) {
    currentlyrotating = 1;
    return;
  }

  lastRotation = millis();
  int posCurrentLetter = displayedLetter;
#ifdef SERIAL_ENABLE
  Serial.print("go to letter: ");
  Serial.println((char)pgm_read_byte(&LETTER_CHARS[toLetter]));
#endif
  //letter on a higher-or-equal index: no full rotation needed, step directly
  if (toLetter >= posCurrentLetter) {
#ifdef SERIAL_ENABLE
    Serial.println("direct");
#endif
    startMotor();
    stepper.setSpeed(stepperSpeed);
    stepFlaps(toLetter - posCurrentLetter);
  }
  else {
    //full rotation is needed, good time for a calibration
#ifdef SERIAL_ENABLE
    Serial.println("full rotation incl. calibration");
#endif
    calibrate(false); //calibrate revolver and do not stop motor
    stepper.setSpeed(stepperSpeed);
    stepFlaps(toLetter);
  }
  //store new position
  displayedLetter = toLetter;
  //rotation is done, stop the motor
  delay(100); //important to stop rotation before shutting of the motor to avoid rotation after switching off current
  stopMotor();
}

//gets magnet sensor offset from EEPROM in steps
void getOffset() {
  int stored;  //shadow: EEPROM.get can't take the volatile directly
  EEPROM.get(eeAddress, stored);
  // Fresh EEPROM reads 0xFFFF (-1, harmless); corruption can read anything.
  // An offset beyond one full revolution is never a legitimate calibration
  // and would make every homing overshoot by whole turns — treat as unset.
  if (stored < -STEPS || stored > STEPS) {
    stored = 0;
  }
  calOffset = stored;
#ifdef SERIAL_ENABLE
  Serial.print("CalOffset from EEPROM: ");
  Serial.print(calOffset);
  Serial.println();
#endif
}

//doing a calibration of the revolver using the hall sensor
int calibrate(bool initialCalibration) {
#ifdef SERIAL_ENABLE
  Serial.println("calibrate revolver");
#endif
  currentlyrotating = 1; //set active state to active
  bool reachedMarker = false;
  // Track whether the hall sensor ever read 0 (magnet detected) during this
  // homing attempt. Stays true only if hall was 0 from step 0 OR we stepped
  // through to find it. If we time out with hall stuck at 1, the status bit
  // tells the master "magnet fell off / bad KY-003 / wiring issue".
  // Issue #47.
  bool hallSawMagnet = false;
  stepper.setSpeed(HOMING_RPM); //fixed speed — never the last message's speed (issue #108). rotateToLetter() re-sets the commanded speed after calibrate(false).
  int i = 0;
  while (!reachedMarker) {
    wdt_reset(); //a full homing at low speed legitimately exceeds the 8 s window (#107)
    int currentHallValue = digitalRead(HALLPIN);
    if (currentHallValue == 0) {
      hallSawMagnet = true;
    }
    if (currentHallValue == 0 && i == 0) {
      //Started inside the magnet window (e.g. SFP_CMD_HOME while parked at the
      //calibrated zero). Accepting this position as "marker found" would
      //make the zero point drift with wherever we happened to stop inside
      //the window — step out until the sensor releases, then clear the
      //edge, so the marker is always approached from the same side (#96).
      while (digitalRead(HALLPIN) == 0) {
        wdt_reset(); //stepping out of the magnet window can also run long (#107)
        stepCounted(ROTATIONDIRECTION * 1);
        i++;
        if (i > 3 * STEPS) break; //hall stuck at 0 — fall through to the failure check below
      }
      if (i <= 3 * STEPS) {
        stepCounted(ROTATIONDIRECTION * 50);
        i += 50;
      }
    }
    else if (currentHallValue == 1 && i == 0) { //already in zero position move out a bit and do the calibration {
      //not reached yet
      i = 50;
      stepCounted(ROTATIONDIRECTION * 50); //move 50 steps to get out of scope of hall
    }
    else if (currentHallValue == 1) {
      //not reached yet
      stepCounted(ROTATIONDIRECTION * 1);
    }
    else {
      //reached marker, go to calibrated offset position
      reachedMarker = true;
      stepCounted(ROTATIONDIRECTION * calOffset);
      displayedLetter = 0;
      missedSteps = 0;
#ifdef SERIAL_ENABLE
      Serial.println("revolver calibrated");
#endif
      noInterrupts();
      statusLastHomeFailed = false;
      statusHallNeverTriggered = !hallSawMagnet;
      lastHomingStepCount = (uint16_t)i;
      interrupts();
      //Only stop motor for initial calibration
      if (initialCalibration) {
        stopMotor();
      }
      return i;
    }
    if (i > 3 * STEPS) {
      //seems that there is a problem with the marker or the sensor. turn of the motor to avoid overheating.
      displayedLetter = 0;
      reachedMarker = true;
#ifdef SERIAL_ENABLE
      Serial.println("calibration revolver failed");
#endif
      noInterrupts();
      statusLastHomeFailed = true;
      statusHallNeverTriggered = !hallSawMagnet;
      lastHomingStepCount = (uint16_t)i;
      interrupts();
      stopMotor();
      return -1;
    }
    i++;
  }
  return i;
}

//switching off the motor driver
void stopMotor() {
  lastInd1 = digitalRead(STEPPERPIN1);
  lastInd2 = digitalRead(STEPPERPIN2);
  lastInd3 = digitalRead(STEPPERPIN3);
  lastInd4 = digitalRead(STEPPERPIN4);

  digitalWrite(STEPPERPIN1, LOW);
  digitalWrite(STEPPERPIN2, LOW);
  digitalWrite(STEPPERPIN3, LOW);
  digitalWrite(STEPPERPIN4, LOW);
#ifdef SERIAL_ENABLE
  Serial.println("Motor Stop");
#endif
  currentlyrotating = 0; //set active state to not active
  delay(100);
}

void startMotor() {
#ifdef SERIAL_ENABLE
  Serial.println("Motor Start");
#endif
  currentlyrotating = 1; //set active state to active
  digitalWrite(STEPPERPIN1, lastInd1);
  digitalWrite(STEPPERPIN2, lastInd2);
  digitalWrite(STEPPERPIN3, lastInd3);
  digitalWrite(STEPPERPIN4, lastInd4);
}
