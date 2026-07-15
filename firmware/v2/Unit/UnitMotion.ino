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
  // Drift position tracking (#263): drumPosition advances along the drum's
  // physical rotation direction. Callers already bake ROTATIONDIRECTION
  // into `steps`, so multiplying again deliberately CANCELS it (d² = 1) —
  // physical-forward motion always advances the position regardless of the
  // wiring direction. Not a bug; do not "fix" to a single multiply.
  driftAdvance(drift, (long)steps * ROTATIONDIRECTION, STEPS);
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
//
//Steps one at a time with a per-step hall watch (#263): a forward move only
//sweeps the hall marker when the drum physically slipped (letter 0 sits at
//the marker and forward moves never wrap past it — any backward target goes
//through calibrate instead), so an entering edge here is a direct drift
//observation. Two consecutive low samples debounce the edge; the ~4 us
//digitalRead per >=2 ms step is noise. Stepper::step(1) keeps its inter-step
//pacing across calls (last_step_time is a member). driftObserveEdge resyncs
//the position to the edge (truth for the #264 readback) and past-threshold
//deviations arm the idle re-home in loop().
void stepFlaps(int flaps) {
  bool inWindow = digitalRead(HALLPIN) == 0;  // no re-fire when starting inside
  uint8_t lowStreak = 0;
  for (int i = 0; i < flaps; i++) {
    wdt_reset(); //a many-flap move at low speed exceeds the 8 s window (#107)
    int roundedStep = STEPS_PER_FLAP_WHOLE;
    missedSteps += STEPS_PER_FLAP_FRAC;
    if (missedSteps > 1) {
      roundedStep++;
      missedSteps--;
    }
    for (int s = 0; s < roundedStep; s++) {
      stepCounted(ROTATIONDIRECTION * 1);
      if (digitalRead(HALLPIN) == 0) {
        if (lowStreak < 2) lowStreak++;
        if (lowStreak == 2 && !inWindow) {
          inWindow = true;
          if (driftObserveEdge(drift, STEPS, DRIFT_THRESHOLD_STEPS)) {
            drift.driftPending = true;
          }
        }
      } else {
        lowStreak = 0;
        inWindow = false;
      }
    }
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

  // Trigger 2 (#309): while UNHOMED, this call will force a full calibrate below
  // (the drum position is unknown). If homing keeps FAILING (dead hall), that
  // full seek would otherwise re-run on every letter and cook the motor —
  // calibrate() has no overheat gate. Rate-limit the retries after the first
  // attempt; a success sets `homed` and skips this path entirely next time.
  if (!homed) {
    if (lastUnhomedCalibrateMs != 0 &&
        millis() - lastUnhomedCalibrateMs < UNHOMED_CALIBRATE_COOLDOWN_MS) {
      return;  // still cooling down after a failed home — leave the drum idle
    }
    lastUnhomedCalibrateMs = millis();
  }

  lastRotation = millis();
  int posCurrentLetter = displayedLetter;
#ifdef SERIAL_ENABLE
  Serial.print("go to letter: ");
  Serial.println((char)pgm_read_byte(&LETTER_CHARS[toLetter]));
#endif
  //letter on a higher-or-equal index: no full rotation needed, step directly.
  //But while UNHOMED (#309 boot-home trigger 2) the drum position is unknown —
  //force the calibrate branch so a driving master that skips an explicit HOME
  //still gets a homed unit before the first move.
  if (homed && toLetter >= posCurrentLetter) {
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
  //Loaded Vcc sample (#306): the coils are still energised here, so the rail
  //is at its steady loaded level — the sag the #305 brownout saga chased.
  //Taken once per move, before the motor de-energises below.
  vitalsSample(true);
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
      //Drift measurement (#263): the search stepping above flowed through
      //stepCounted, so drumPosition says where the belief expected this
      //edge — a past-threshold deviation is a measured drift event. The
      //homing itself IS the correction, so only count; pending clears.
      driftObserveEdge(drift, STEPS, DRIFT_THRESHOLD_STEPS);
      drift.driftPending = false;
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
      homed = true;  // boot-home satisfied (#309); disables the self-home drain
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
      //No marker found: the position estimate is meaningless and an auto
      //re-home would just fail the same way — drop both (#263). The master
      //sees the fault via statusLastHomeFailed / hallNeverTriggered.
      drift.positionKnown = false;
      drift.driftPending = false;
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

//Re-encodes the ISR-visible GET_DIAG / GET_SELF_TEST reply buffers from the
//loop-context drift/selfTest state (#263/#265). The interrupt-guarded copy
//is the only ISR handshake — requestEvent() streams the buffers verbatim,
//so it can never see a torn multi-byte value (#96 class). Called once per
//loop() pass, from setup() before the Wire handlers register (#173), and at
//runSelfTest's state transitions.
void driftRefreshReplyBuffers() {
  uint8_t diag[DRIFT_REPLY_LEN];
  uint8_t flags = 0;
  if (drift.driftPending) flags |= DRIFT_FLAG_PENDING;
  if (drift.positionKnown) flags |= DRIFT_FLAG_POSITION_KNOWN;
  int16_t offsetShadow = (int16_t)calOffset;  //volatile → local
  driftEncodeDiagReply(
      driftPhysicalLetter(drift, offsetShadow, STEPS, AMOUNTFLAPS), flags,
      drift.driftEvents, drift.lastDriftSteps, diag);
  uint8_t st[SELFTEST_REPLY_LEN];
  selfTestEncodeReply(selfTest, st);
  noInterrupts();
  for (uint8_t i = 0; i < DRIFT_REPLY_LEN; i++) diagReplyBuf[i] = diag[i];
  for (uint8_t i = 0; i < SELFTEST_REPLY_LEN; i++) selfTestReplyBuf[i] = st[i];
  interrupts();
}

//Supply-Vcc diagnostics (#306). Reads the AVR's own rail via the internal
//1.1V bandgap referenced to AVcc — no external divider, so it works on any
//unit. ADMUX 0b1110 selects the bandgap channel; the reference needs ~1 ms
//to settle after the mux switch before a valid conversion. Runs in loop
//context (never the Wire ISR) — the blocking conversion is ~0.1 ms.
uint16_t readVccMv() {
  ADMUX = (1 << REFS0) | 0b1110;  // AVcc ref, 1.1V bandgap as input
  delay(2);                        // let the bandgap settle
  ADCSRA |= (1 << ADSC);           // start conversion
  while (ADCSRA & (1 << ADSC)) {}  // wait (~13 ADC clocks)
  uint16_t adc = ADC;
  return unitVccFromAdc(adc);
}

//Free SRAM = gap between the heap top (__brkval, or __heap_start before any
//malloc) and the current stack pointer. The classic AVR idiom; a lower value
//means less headroom before a stack/heap collision.
uint16_t freeRamBytes() {
  extern int __heap_start, *__brkval;
  int v;
  int top = (__brkval == 0) ? (int)&__heap_start : (int)__brkval;
  int freeBytes = (int)&v - top;
  return (freeBytes < 0) ? 0 : (uint16_t)freeBytes;
}

//Take one Vcc + free-RAM sample and fold the minima. `loaded` distinguishes a
//mid-move sample (coils energised, the sag we care about) from an idle rail
//check; both feed the since-boot minimum, so the loaded samples drive vccMin
//down toward the brownout floor while idle samples keep vccNow fresh.
void vitalsSample(bool /*loaded*/) {
  vitalsVccNow = readVccMv();
  if (vitalsVccNow != 0 && vitalsVccNow < vitalsVccMin) vitalsVccMin = vitalsVccNow;
  uint16_t fr = freeRamBytes();
  if (fr < vitalsFreeRamMin) vitalsFreeRamMin = fr;
}

//Re-encodes the ISR-visible GET_VITALS reply from loop-context state (#306),
//same interrupt-guarded handshake as driftRefreshReplyBuffers(). Cheap (no
//ADC — the sampling is throttled separately), so it runs every loop pass and
//once in setup() before the Wire handlers register.
void vitalsRefreshReplyBuffer() {
  UnitVitals v;
  v.vccNow_mV = vitalsVccNow;
  v.vccMin_mV = (vitalsVccMin == 0xFFFF) ? vitalsVccNow : vitalsVccMin;
  v.cmdPos = (uint8_t)(receivedNumber & 0xFF);  // last commanded flap index
  v.freeRamMin = (vitalsFreeRamMin == 0xFFFF) ? 0 : vitalsFreeRamMin;
  uint8_t buf[VITALS_REPLY_LEN];
  vitalsEncodeReply(v, buf);
  noInterrupts();
  for (uint8_t i = 0; i < VITALS_REPLY_LEN; i++) vitalsReplyBuf[i] = buf[i];
  interrupts();
}

//On-demand diagnostic revolution (#265): sync to the hall entering edge
//(calibrate's approach rules), then measure one full revolution stepping 1
//at a time — actual steps/rev, hall window width in steps, wall time. All
//at HOMING_RPM. Parks at the calibrated zero like calibrate() so the
//letter-diff check in loop() restores the commanded letter; the 2 s
//overheat gate gives the motor a breather after the ~2 revolutions of
//continuous motion. A timeout in either phase reports FAILED and drops the
//position estimate (same reasoning as calibrate's failure path).
void runSelfTest() {
  selfTest.state = SELFTEST_STATE_RUNNING;
  driftRefreshReplyBuffers();  //publish RUNNING before the ~12 s of motion
  startMotor();
  stepper.setSpeed(HOMING_RPM);

  //Phase 1: approach the entering edge from outside the window.
  long guard = 0;
  bool failed = false;
  if (digitalRead(HALLPIN) == 0) {
    while (digitalRead(HALLPIN) == 0) {
      wdt_reset();
      stepCounted(ROTATIONDIRECTION * 1);
      if (++guard > 3L * STEPS) { failed = true; break; }
    }
    if (!failed) {
      stepCounted(ROTATIONDIRECTION * 50);  //clear the release edge
      guard += 50;
    }
  }
  while (!failed && digitalRead(HALLPIN) != 0) {
    wdt_reset();
    stepCounted(ROTATIONDIRECTION * 1);
    if (++guard > 3L * STEPS) failed = true;
  }

  //Phase 2: one measured revolution from the edge. The window width is the
  //initial hall-low stretch; the revolution completes when the hall goes
  //low again after leaving it (2-sample debounce on both transitions).
  if (!failed) {
    driftMarkSynced(drift);  //we are AT the entering edge
    uint16_t measuredSteps = 0;
    //Seed 1: the entering-edge position itself (where phase 1 stopped) is
    //inside the window but phase 2 only samples AFTER each step.
    uint16_t windowSteps = 1;
    unsigned long t0 = millis();
    bool leftWindow = false;
    uint8_t streak = 0;
    for (;;) {
      wdt_reset();
      stepCounted(ROTATIONDIRECTION * 1);
      measuredSteps++;
      if (measuredSteps > (uint16_t)(2 * STEPS)) {  //edge never came back
        failed = true;
        break;
      }
      int hall = digitalRead(HALLPIN);
      if (!leftWindow) {
        if (hall == 0) {
          windowSteps++;
          streak = 0;
        } else if (++streak >= 2) {
          leftWindow = true;
          streak = 0;
        }
      } else {
        if (hall == 0) {
          if (++streak >= 2) break;  //re-entered: revolution complete
        } else {
          streak = 0;
        }
      }
    }
    if (!failed) {
      unsigned long dt = millis() - t0;
      selfTest.revTimeMs = dt > 65535UL ? 65535 : (uint16_t)dt;
      //The break landed on the debounce CONFIRM sample — the true edge was
      //one step earlier (phase 1 stopped at the first low sample, so both
      //ends of the measurement use the same edge definition).
      selfTest.stepsPerRev = measuredSteps - 1;
      selfTest.hallWindowSteps = windowSteps;
      selfTest.state = SELFTEST_STATE_OK;
      //Parked <=2 steps past the entering edge (the debounce confirm) —
      //well under the drift threshold; re-park at the calibrated zero.
      driftMarkSynced(drift);
      stepCounted(ROTATIONDIRECTION * calOffset);
      displayedLetter = 0;
      missedSteps = 0;
      drift.driftPending = false;
    }
  }
  if (failed) {
    selfTest.state = SELFTEST_STATE_FAILED;
    selfTest.stepsPerRev = 0;
    selfTest.hallWindowSteps = 0;
    selfTest.revTimeMs = 0;
    //The hall edge was never found: the drum's position is unknowable, so
    //park instead of letting the letter-diff check "restore" the commanded
    //letter from a fake blank origin (codex review). The master's next
    //frame re-sends content deliberately; until then the unit stays put.
    displayedLetter = 0;
    receivedNumber = 0;
    drift.positionKnown = false;
    drift.driftPending = false;
  }
  lastRotation = millis();  //overheat gate before the restore move
  delay(100);
  stopMotor();
  driftRefreshReplyBuffers();  //publish the result before loop() resumes
}
