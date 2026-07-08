// I2C slave protocol handlers, extracted from Unit.ino (#175).
// One translation unit: PlatformIO concatenates the main sketch FIRST
// (Unit.ino — "main" is detected by containing setup()/loop(), per the
// InoToCPPConverter source), then sibling .ino files — so every
// #define/global from Unit.ino is visible here without declarations
// (empirically verified before the split; function prototypes are hoisted
// globally across all .ino files anyway). receiveLetter()/requestEvent()
// run in the TWI ISR: no blocking work, defer mutations to loop() via the
// pending* flags declared in Unit.ino.

void receiveLetter(int numBytes) {
  if (numBytes <= 0) return;

  int firstByte = Wire.read();
  int remaining = numBytes - 1;

  // First byte >= AMOUNTFLAPS is a command opcode, not a letter index.
  if (firstByte >= AMOUNTFLAPS) {
    switch ((uint8_t)firstByte) {
      case SFP_CMD_ENTER_BOOTLOADER:
      case SFP_CMD_REBOOT:
        // Both trigger a watchdog reset. SFP_CMD_ENTER_BOOTLOADER is the
        // semantic "hand off to twiboot so the master can push firmware"
        // (master keeps twiboot alive with continuous pings); SFP_CMD_REBOOT
        // is "just restart the sketch" (master does nothing, twiboot
        // times out after ~250 ms, sketch runs). Mechanically identical
        // from the unit's side — only master follow-up behavior differs.
        pendingBootloader = true;
        break;
      case SFP_CMD_GET_VERSION:
        pendingVersionResponse = true;
        break;
      case SFP_CMD_GET_OFFSET:
        pendingOffsetResponse = true;
        break;
      case SFP_CMD_GET_STATUS:
        pendingStatusResponse = true;
        break;
      case SFP_CMD_GET_LETTER:
        pendingLetterResponse = true;
        break;
      case SFP_CMD_SET_OFFSET:
        if (remaining >= 2) {
          uint8_t lo = (uint8_t)Wire.read();
          uint8_t hi = (uint8_t)Wire.read();
          remaining -= 2;
          int16_t requestedOffset = (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
          // Drop out-of-range offsets instead of persisting them (#171):
          // past ±STEPS the post-homing stepper.step(calOffset) — one
          // blocking library call — outruns the 8 s watchdog window and
          // resets the Nano mid-rotation. Mirrors getOffset()'s boot check.
          if (requestedOffset >= -STEPS && requestedOffset <= STEPS) {
            pendingOffsetValue = requestedOffset;
            pendingOffsetWrite = true;
          } else if (badCommandCount < 0xFF) {
            badCommandCount++;
          }
        } else if (badCommandCount < 0xFF) {
          badCommandCount++;
        }
        break;
      case SFP_CMD_JOG:
        if (remaining >= 1) {
          pendingJogSteps = (int8_t)Wire.read();
          remaining--;
        } else if (badCommandCount < 0xFF) {
          badCommandCount++;
        }
        break;
      case SFP_CMD_HOME:
        pendingHome = true;
        break;
      case SFP_CMD_SET_I2C_ADDRESS:
        if (remaining >= 1) {
          pendingAddressValue = (uint8_t)Wire.read();
          remaining--;
          pendingSetAddress = true;
        } else if (badCommandCount < 0xFF) {
          badCommandCount++;
        }
        break;
      case SFP_CMD_CLEAR_I2C_ADDRESS:
        pendingClearAddress = true;
        break;
      case SFP_CMD_IDENTIFY:
        pendingIdentify = true;
        break;
      default:
        if (badCommandCount < 0xFF) badCommandCount++;
        break;  // unknown opcode -> ignore
    }
    while (remaining-- > 0) Wire.read();  // drain any extra args
    return;
  }

  // Legacy letter+speed protocol is exactly 2 bytes. Anything else is a
  // probe (the master's bootloader-detection write hits this code path)
  // or a malformed command — drain and ignore so we don't accidentally
  // rotate the drum to a random letter when probed. Single-byte empty
  // transmissions are the master's standard bus-scan probe, not errors.
  if (numBytes != 2) {
    if (numBytes > 2 && badCommandCount < 0xFF) badCommandCount++;
    while (remaining-- > 0) Wire.read();
    return;
  }

  receivedNumber = firstByte;
  // Stepper::setSpeed() divides by the speed — a zero byte (bus noise or a
  // buggy master) would produce a garbage step delay. Clamp to >= 1.
  int requestedSpeed = Wire.read();
  stepperSpeed = (requestedSpeed < 1) ? 1 : requestedSpeed;
}

void requestEvent() {
  if (pendingVersionResponse) {
    // Send exactly 8 bytes: GIT_REV, null-padded. Master reads 8 and compares.
    uint8_t buf[8] = {0};
    for (uint8_t i = 0; i < 8 && GIT_REV[i] != '\0'; i++) buf[i] = GIT_REV[i];
    Wire.write(buf, 8);
    pendingVersionResponse = false;
    return;
  }
  if (pendingOffsetResponse) {
    // Send exactly 2 bytes: int16 calOffset, little-endian.
    uint16_t raw = (uint16_t)calOffset;
    uint8_t buf[2] = { (uint8_t)(raw & 0xFF), (uint8_t)((raw >> 8) & 0xFF) };
    Wire.write(buf, 2);
    pendingOffsetResponse = false;
    return;
  }
  if (pendingLetterResponse) {
    // Issue #106. 2 bytes: displayed letter index + bitwise complement so
    // the master can reject a corrupted read instead of "verifying" noise.
    uint8_t letter = (uint8_t)displayedLetter;
    uint8_t buf[2] = { letter, (uint8_t)~letter };
    Wire.write(buf, 2);
    pendingLetterResponse = false;
    return;
  }
  if (pendingStatusResponse) {
    // Issue #47. 8-byte health/diag payload. Master parses into UnitStatus;
    // old masters that don't know SFP_CMD_GET_STATUS never send it, so this
    // branch won't fire for them.
    //
    //   byte 0   status flag bitfield
    //             bit 0  currentlyrotating
    //             bit 1  last home FAILED (hit 3*STEPS without marker)
    //             bit 2  hall never triggered during last home
    //             bit 3  reserved (stuck drum, future)
    //             bits 4-7 reserved
    //   byte 1   savedMcusr — current-boot reset-cause snapshot
    //   byte 2   lifetime brownout reset count (EEPROM, saturating)
    //   byte 3   lifetime watchdog reset count (EEPROM, saturating)
    //   byte 4-5 uptime in seconds (uint16 big-endian, saturating)
    //   byte 6   bad I2C command count since boot (saturating)
    //   byte 7   last homing step count / 16 (saturating uint8)
    uint8_t flags = 0;
    if (currentlyrotating)          flags |= (1 << 0);
    if (statusLastHomeFailed)       flags |= (1 << 1);
    if (statusHallNeverTriggered)   flags |= (1 << 2);
    uint16_t lastHomeScaled16 = (lastHomingStepCount >> 4);
    uint8_t lastHomeScaled = (lastHomeScaled16 > 0xFF) ? 0xFF : (uint8_t)lastHomeScaled16;
    uint8_t buf[8] = {
      flags,
      savedMcusr,
      lifetimeBrownoutCount,
      lifetimeWatchdogCount,
      (uint8_t)((uptimeSeconds >> 8) & 0xFF),
      (uint8_t)(uptimeSeconds & 0xFF),
      badCommandCount,
      lastHomeScaled,
    };
    Wire.write(buf, 8);
    pendingStatusResponse = false;
    return;
  }
  Wire.write(currentlyrotating); //send unit status to master
}

//Returns the I2C address of the unit. EEPROM takes precedence (set by the
//position wizard, once implemented) so the physical DIP switches don't need
//to be unique after first setup; empty/invalid EEPROM falls back to
//SFP_I2C_ADDRESS_BASE + DIP.
int getaddress() {
  uint8_t magic = EEPROM.read(EEPROM_ADDR_ID_MAGIC);
  if (magic == EEPROM_ID_MAGIC_VALUE) {
    uint8_t stored = EEPROM.read(EEPROM_ADDR_I2C_ADDR);
    //Reject obviously bad values. Address 0 is reserved general-call, 127 is
    //reserved too; anything else in 1..126 is plausible.
    if (stored >= 1 && stored <= 126) {
      return stored;
    }
  }
  int dipValue = !digitalRead(ADRESSSW4) + (!digitalRead(ADRESSSW3) * 2) + (!digitalRead(ADRESSSW2) * 4) + (!digitalRead(ADRESSSW1) * 8);
  return SFP_I2C_ADDRESS_BASE + dipValue;
}
