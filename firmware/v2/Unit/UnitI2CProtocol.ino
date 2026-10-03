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
  // Every master write — a bare bus-scan probe included — supersedes whatever
  // reply the previous write asked for (#502).
  pendingReply = REPLY_NONE;
  linkRxFrames++;  // probes count too: any addressed write proves we heard it
  if (numBytes <= 0) return;

  // Any I2C receive — even a bus-scan probe — proves a master is present, so
  // the boot-home fallback waits the hard cap instead of self-homing at the
  // 30 s standalone deadline (#309). Single byte, ISR-atomic.
  masterEverContacted = true;

  int firstByte = Wire.read();
  int remaining = numBytes - 1;

  // First byte >= AMOUNTFLAPS is a command opcode, not a letter index.
  if (firstByte >= AMOUNTFLAPS) {
    uint8_t opcode = (uint8_t)firstByte;
    // #512. The gate byte is written by loop() in a single store — safe to
    // read here.
    bool strict = unitGateEnabled(lifetime.featureGates, UNIT_GATE_STRICT_OPCODES);
    if (sfpIsNoArgMutation(opcode)) {
      // These carry no payload, so the guard byte is the only thing that tells
      // a real command from a corrupted letter or poll.
      uint8_t extraLen = (remaining > 0xFF) ? 0xFF : (uint8_t)remaining;
      uint8_t guard = 0;
      if (remaining > 0) {
        guard = (uint8_t)Wire.read();
        remaining--;
      }
      if (!noArgMutationAccepted(opcode, extraLen, guard, strict)) {
        if (badCommandCount < 0xFF) badCommandCount++;
        while (remaining-- > 0) Wire.read();
        return;
      }
    }
    switch (opcode) {
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
        pendingReply = REPLY_VERSION;
        break;
      case SFP_CMD_GET_OFFSET:
        pendingReply = REPLY_OFFSET;
        break;
      case SFP_CMD_GET_STATUS:
        pendingReply = REPLY_STATUS;
        break;
      case SFP_CMD_GET_LETTER:
        pendingReply = REPLY_LETTER;
        break;
      case SFP_CMD_GET_ODOMETER:
        pendingReply = REPLY_ODOMETER;
        break;
      case SFP_CMD_GET_DIAG:
        pendingReply = REPLY_DIAG;
        break;
      case SFP_CMD_GET_SELF_TEST:
        pendingReply = REPLY_SELF_TEST;
        break;
      case SFP_CMD_GET_VITALS:
        pendingReply = REPLY_VITALS;
        break;
      case SFP_CMD_GET_EXT_DIAG:
        pendingReply = REPLY_EXT_DIAG;
        break;
      case SFP_CMD_GET_LIFETIME:
        pendingReply = REPLY_LIFETIME;
        break;
      case SFP_CMD_GET_BOOT_INFO:
        pendingReply = REPLY_BOOT_INFO;  // #499; cached reply streamed by requestEvent
        break;
      case SFP_CMD_START_SELF_TEST:
        pendingSelfTest = true;
        break;
      case SFP_CMD_SET_OFFSET:
        if (remaining >= SET_OFFSET_PAYLOAD_LEN) {
          uint8_t pay[SET_OFFSET_PAYLOAD_LEN];
          for (uint8_t k = 0; k < SET_OFFSET_PAYLOAD_LEN; k++) {
            pay[k] = (uint8_t)Wire.read();
          }
          remaining -= SET_OFFSET_PAYLOAD_LEN;
          // Value + bitwise complement (#405): a pair that disagrees is a
          // corrupted write, not a calibration. Previously fire-and-forget.
          int16_t requestedOffset = 0;
          if (!setOffsetDecode(pay, SET_OFFSET_PAYLOAD_LEN, requestedOffset)) {
            if (badCommandCount < 0xFF) badCommandCount++;
            break;
          }
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
      case SFP_CMD_JOG: {
        uint8_t extraLen = (remaining > 0xFF) ? 0xFF : (uint8_t)remaining;
        uint8_t pay[JOG_PAYLOAD_LEN] = {0, 0};
        for (uint8_t k = 0; k < JOG_PAYLOAD_LEN && remaining > 0; k++) {
          pay[k] = (uint8_t)Wire.read();
          remaining--;
        }
        int8_t steps = 0;
        if (jogDecode(pay, extraLen, strict, steps)) {
          pendingJogSteps = steps;
        } else if (badCommandCount < 0xFF) {
          badCommandCount++;
        }
        break;
      }
      case SFP_CMD_HOME:
        pendingHome = true;
        break;
      case SFP_CMD_SET_I2C_ADDRESS:
        if (remaining >= SET_ADDRESS_PAYLOAD_LEN) {
          uint8_t pay[SET_ADDRESS_PAYLOAD_LEN];
          for (uint8_t k = 0; k < SET_ADDRESS_PAYLOAD_LEN; k++) {
            pay[k] = (uint8_t)Wire.read();
          }
          remaining -= SET_ADDRESS_PAYLOAD_LEN;
          // Value + bitwise complement (#405). This was the worst hole in the
          // contract: one unprotected byte, after which the unit persists it
          // and reboots. A corruption landing inside 1..126 relocated the unit
          // to an address nobody was looking at, unverifiable after the fact
          // because the unit was gone from the address you were talking to —
          // recovery was a physical trip to re-DIP.
          uint8_t requestedAddress = 0;
          if (setAddressDecode(pay, SET_ADDRESS_PAYLOAD_LEN, requestedAddress)) {
            pendingAddressValue = requestedAddress;
            pendingSetAddress = true;
          } else if (badCommandCount < 0xFF) {
            badCommandCount++;
          }
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
      case SFP_CMD_RESET_ODOMETER:
        pendingOdometerReset = true;
        break;
      case SFP_CMD_SET_GATES:
        if (remaining >= SET_GATES_PAYLOAD_LEN) {
          uint8_t pay[SET_GATES_PAYLOAD_LEN];
          for (uint8_t k = 0; k < SET_GATES_PAYLOAD_LEN; k++) {
            pay[k] = (uint8_t)Wire.read();
          }
          remaining -= SET_GATES_PAYLOAD_LEN;
          // Value + bitwise complement (#405 discipline), then a bits check:
          // this unit persists only gates it has code for, so a newer master
          // cannot talk it into reporting a feature it will never run (#409).
          uint8_t requestedGates = 0;
          if (setGatesDecode(pay, SET_GATES_PAYLOAD_LEN, requestedGates) &&
              unitGateBitsKnown(requestedGates)) {
            pendingGatesValue = requestedGates;
            pendingGatesWrite = true;
          } else if (badCommandCount < 0xFF) {
            badCommandCount++;
          }
        } else if (badCommandCount < 0xFF) {
          badCommandCount++;
        }
        break;
      case SFP_CMD_BOOT_UPDATE:
        // In-system twiboot update (#499). 2-byte payload: stage + ~stage
        // (#405 complement discipline — this mutation rewrites the bootloader,
        // so a corrupted stage byte must never be acted on). loop() runs it.
        if (remaining >= 2) {
          uint8_t stage = (uint8_t)Wire.read();
          uint8_t comp = (uint8_t)Wire.read();
          remaining -= 2;
          if ((uint8_t)(stage ^ comp) == 0xFF && (stage == 1 || stage == 2)) {
            pendingBootUpdateStage = stage;
            pendingBootUpdate = true;
          } else if (badCommandCount < 0xFF) {
            badCommandCount++;
          }
        } else if (badCommandCount < 0xFF) {
          badCommandCount++;
        }
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
  // Clamped into the range a master sends (unitClampSpeed): zero would make
  // Stepper::setSpeed() divide by it, and a corrupted high byte would ask the
  // motor for a rate it only loses steps at.
  stepperSpeed = unitClampSpeed(Wire.read());
}

void requestEvent() {
  uint8_t reply = pendingReply;
  pendingReply = REPLY_NONE;
  linkTxReplies++;
  if (reply == REPLY_VERSION) {
    // 10 bytes: GIT_REV null-padded, SFP_PROTOCOL_VERSION, checksum (#405).
    // This reply's shape is FIXED FOREVER — it carries the version that gates
    // every other opcode, so a master must be able to parse it without
    // already knowing which contract this unit speaks.
    uint8_t buf[VERSION_REPLY_LEN];
    versionEncodeReply(GIT_REV, SFP_PROTOCOL_VERSION, buf);
    Wire.write(buf, VERSION_REPLY_LEN);
    return;
  }
  if (reply == REPLY_OFFSET) {
    // 3 bytes: int16 calOffset LE + checksum (#405).
    uint8_t buf[OFFSET_REPLY_LEN];
    offsetEncodeReply((int16_t)calOffset, buf);
    Wire.write(buf, OFFSET_REPLY_LEN);
    return;
  }
  if (reply == REPLY_LETTER) {
    // Issue #106. 2 bytes: displayed letter index + bitwise complement so
    // the master can reject a corrupted read instead of "verifying" noise.
    // A unit that is not homed reports SFP_LETTER_UNKNOWN (letterReplyIndex).
    uint8_t letter = letterReplyIndex((uint8_t)displayedLetter, homed);
    uint8_t buf[2] = { letter, (uint8_t)~letter };
    Wire.write(buf, 2);
    return;
  }
  if (reply == REPLY_ODOMETER) {
    // 5 bytes: uint32 LE revolutions + XOR checksum ^ 0xA5 (#231). Reading
    // the 4-byte mirror is safe here: this IS the TWI ISR, and loop-side
    // writers hold interrupts off (UnitMotion.ino stepCounted()).
    uint8_t buf[ODO_REPLY_LEN];
    odometerEncodeReply(odometerRevolutions, buf);
    Wire.write(buf, ODO_REPLY_LEN);
    return;
  }
  if (reply == REPLY_DIAG) {
    // 6 bytes, pre-encoded by driftRefreshReplyBuffers() under
    // noInterrupts() (#263/#264) — stream verbatim, nothing to compute in
    // ISR context. The volatile cast is safe: writers hold interrupts off.
    Wire.write((const uint8_t*)diagReplyBuf, DRIFT_REPLY_LEN);
    return;
  }
  if (reply == REPLY_SELF_TEST) {
    // 9 bytes, same pre-encoded-buffer contract as the diag reply (#265).
    Wire.write((const uint8_t*)selfTestReplyBuf, SELFTEST_REPLY_LEN);
    return;
  }
  if (reply == REPLY_VITALS) {
    // 8 bytes, pre-encoded by vitalsRefreshReplyBuffer() under noInterrupts()
    // (#306) — stream verbatim. Un-reflashed masters never send GET_VITALS.
    Wire.write((const uint8_t*)vitalsReplyBuf, VITALS_REPLY_LEN);
    return;
  }
  if (reply == REPLY_EXT_DIAG) {
    // 11-byte base packet (#365), pre-encoded by refreshExtDiagReply() under
    // noInterrupts(), followed by the 10-byte link extension (#502). The
    // extension is encoded here, not in loop(): its frame counters must keep
    // moving while a blocking move holds loop(), or a busy unit reads as a deaf
    // one. A master that reads only the base length NACKs after byte 10.
    uint8_t buf[EXT_DIAG_LINK_REPLY_LEN];
    for (uint8_t i = 0; i < EXT_DIAG_REPLY_LEN; i++) buf[i] = extDiagReplyBuf[i];
    UnitLinkStats link;
    link.uptimeSeconds = uptimeSecondsFull;
    link.rxFrames      = linkRxFrames;
    link.txReplies     = linkTxReplies;
    link.deafHeals     = twiDeaf.resets;
    extDiagLinkEncode(link, buf + EXT_DIAG_REPLY_LEN);
    Wire.write(buf, EXT_DIAG_LINK_REPLY_LEN);
    return;
  }
  if (reply == REPLY_LIFETIME) {
    // 15 bytes, pre-encoded by refreshLifetimeReply() under noInterrupts()
    // (#406) — stream verbatim. A master predating the opcode never sends it;
    // the masked checksum plus the reply-length check on the master side
    // handle a stray probe hitting the unknown opcode.
    Wire.write((const uint8_t*)lifetimeReplyBuf, LIFETIME_REPLY_LEN);
    return;
  }
  if (reply == REPLY_BOOT_INFO) {
    // 11 bytes (#499), cached by refreshBootInfoReply() at boot + after each
    // update — streamed verbatim. A master predating the opcode never sends it;
    // the masked checksum + range checks on the master side reject a stray probe
    // hitting the unknown opcode.
    Wire.write((const uint8_t*)bootInfoReplyBuf, BOOT_INFO_REPLY_LEN);
    return;
  }
  if (reply == REPLY_STATUS) {
    // Issue #47. 8-byte health/diag payload + a #405 checksum byte. Master
    // parses it into UnitStatus.
    //
    //   byte 0   status flag bitfield
    //             bit 0  currentlyrotating
    //             bit 1  last home FAILED (hit 3*STEPS without marker)
    //             bit 2  hall never triggered during last home
    //             bit 3  reserved (stuck drum, future)
    //             bit 4  I2C address source is EEPROM, not DIP (#215) —
    //                    twiboot still listens on DIP, so a mismatch means
    //                    over-I2C reflash can't reach this unit
    //             bit 5  homed since boot (#309) — with bit 0 gives the
    //                    master unhomed/homing/homed (hs2)
    //             bits 6-7 reserved
    //   byte 1   reset cause of this boot: MCUSR bits 0-3, bit 7 = the
    //            sketch asked for it (UnitResetCause.h)
    //   byte 2   lifetime brownout reset count (EEPROM, saturating)
    //   byte 3   lifetime watchdog reset count (EEPROM, saturating)
    //   byte 4-5 uptime in seconds (uint16 big-endian, saturating)
    //   byte 6   bad I2C command count since boot (saturating)
    //   byte 7   last homing step count / 16 (saturating uint8)
    uint8_t flags = 0;
    if (currentlyrotating)          flags |= (1 << 0);
    if (statusLastHomeFailed)       flags |= (1 << 1);
    if (statusHallNeverTriggered)   flags |= (1 << 2);
    if (addressFromEeprom)          flags |= (1 << 4);
    // Bit 5 = homed since boot (#309). With bit 0 (moving) the master derives
    // the boot-home state: !homed & !moving = unhomed, !homed & moving = homing,
    // homed = homed. Lets a curl see a row still waiting out its stagger.
    if (homed)                      flags |= UNIT_STATUS_FLAG_HOMED;
    uint16_t lastHomeScaled16 = (lastHomingStepCount >> 4);
    uint8_t lastHomeScaled = (lastHomeScaled16 > 0xFF) ? 0xFF : (uint8_t)lastHomeScaled16;
    uint8_t payload[STATUS_PAYLOAD_LEN] = {
      flags,
      resetStatusByte,
      lifetimeBrownoutCount,
      lifetimeWatchdogCount,
      (uint8_t)((uptimeSeconds >> 8) & 0xFF),
      (uint8_t)(uptimeSeconds & 0xFF),
      badCommandCount,
      lastHomeScaled,
    };
    // 9th byte is the #405 checksum. This is the highest-frequency read on the
    // bus and the one with the widest blast radius — a corrupted flags byte
    // used to be able to invent a fault or mask a real one silently.
    uint8_t buf[STATUS_REPLY_LEN];
    statusEncodeReply(payload, buf);
    Wire.write(buf, STATUS_REPLY_LEN);
    return;
  }
  Wire.write(currentlyrotating); //send unit status to master
}

//TWI self-heal (#489, policy in UnitTwiHeal.h). Samples SDA (PC4) and SCL
//(PC5) straight from PINC — valid while the TWI owns the pins. Either held low
//kills the bus (SDA: no START; SCL: an endless stretch). Dropping TWEN hands
//the pins back to the port: inputs with the pull-ups twi_init() left set, so
//lines that read high a moment later were ours. Wire.begin() re-runs twi_init (twi_state back
//to READY) and keeps the onReceive/onRequest hooks; TWGCE must be re-armed
//after it, same as setup(). Runs from loop() only — a wedge during a blocking
//move clears when the move returns to loop().
void twiHealTick() {
  uint32_t now = millis();
  const uint8_t lines = _BV(PC4) | _BV(PC5);
  bool held = (PINC & lines) != lines;
  if (!twiHealShouldReset(twiHeal, held, now)) return;
  // TWEN off; writing TWINT=1 also clears a stale flag that would otherwise
  // vector a bogus ISR the moment Wire.begin() re-enables TWIE.
  TWCR = _BV(TWINT);
  pendingReply = REPLY_NONE;  // whatever was asked for died with the transfer
  delayMicroseconds(20);
  bool releasedByUs = (PINC & lines) == lines;
  Wire.begin(i2cAddress);
  TWAR |= (1 << TWGCE);
  twiHealNoteReset(twiHeal, now, releasedByUs);
}

//Deaf-slave check (#502, policy in UnitTwiHeal.h). Compares TWCR/TWAR against
//what a listening slave needs; a config that stays wrong past the hold window
//gets the same TWEN-off -> Wire.begin() re-init as the heal above, deferred
//while a line reads low. PINC still reads the line levels if TWEN was lost:
//the pins fall back to inputs with twi_init()'s pull-ups.
static_assert(TWI_LISTEN_TWCR_MASK == (_BV(TWEA) | _BV(TWEN) | _BV(TWIE)),
              "UnitTwiHeal.h TWCR mask does not match the AVR bit positions");
static_assert(TWI_LISTEN_TWAR_GCE == _BV(TWGCE),
              "UnitTwiHeal.h TWAR general-call bit does not match the AVR bit");
void twiDeafTick() {
  uint32_t now = millis();
  const uint8_t lines = _BV(PC4) | _BV(PC5);
  bool linesFree = (PINC & lines) == lines;
  bool intact = twiListenConfigIntact(TWCR, TWAR, (uint8_t)i2cAddress);
  if (!twiDeafShouldReset(twiDeaf, intact, linesFree, now)) return;
  TWCR = _BV(TWINT);  // TWEN off, stale flag cleared — see twiHealTick()
  pendingReply = REPLY_NONE;
  Wire.begin(i2cAddress);
  TWAR |= (1 << TWGCE);
  twiDeafNoteReset(twiDeaf, now);
}

//Returns the I2C address of the unit. EEPROM takes precedence (set by the
//position wizard, once implemented) so the physical DIP switches don't need
//to be unique after first setup; empty/invalid EEPROM falls back to
//SFP_I2C_ADDRESS_BASE + DIP.
int getaddress() {
  //Blank, unprovisioned, checksum-failed and out-of-range identity blocks all
  //resolve to 0 = fall back to DIP (#406). That direction is the safe one:
  //twiboot listens on the DIP-derived address regardless, so an address we
  //refuse to adopt is always recoverable, while an address we wrongly adopt
  //strands the unit somewhere nobody is looking and takes a physical trip.
  uint8_t block[EE_ID_BLOCK_LEN];
  for (uint8_t i = 0; i < EE_ID_BLOCK_LEN; i++) {
    block[i] = EEPROM.read(EE_LAYOUT_VERSION + i);
  }
  uint8_t stored = unitEeIdentityAddress(block);
  if (stored != 0) {
    addressFromEeprom = true;  //GET_STATUS flags bit 4 (#215)
    return stored;
  }
  int dipValue = !digitalRead(ADRESSSW4) + (!digitalRead(ADRESSSW3) * 2) + (!digitalRead(ADRESSSW2) * 4) + (!digitalRead(ADRESSSW1) * 8);
  return SFP_I2C_ADDRESS_BASE + dipValue;
}
