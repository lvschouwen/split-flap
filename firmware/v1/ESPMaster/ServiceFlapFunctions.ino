#include "UnitProtocolHelpers.h"
#include "DisplayWidth.h"

// I2C address 0x00 is reserved (general call); offset every unit's address so
// the master's 0-based unit index maps to 0x01..0x10. Must match
// I2C_ADDRESS_BASE in Unit/Unit.ino.
#define I2C_ADDRESS_BASE 1

// Command opcodes understood by Unit.ino's receiveLetter(). Values >=
// FLAP_AMOUNT are reserved for commands (valid letter indices are 0..44).
// Must stay in sync with Unit/Unit.ino's CMD_* constants.
//
// Opcodes are organized into semantic bands (issue #47):
//   0x80..0x8F  queries — write opcode, follow with requestFrom
//   0x90..0x9F  mutations — write opcode + args, no read follow-up
//
// 0x80 (ENTER_BOOTLOADER) and 0x81 (GET_VERSION) are fixed forever across
// protocol bumps because they are the cross-generation recovery path.
#define UNIT_CMD_ENTER_BOOTLOADER 0x80
#define UNIT_CMD_GET_VERSION      0x81
#define UNIT_CMD_GET_OFFSET       0x82
#define UNIT_CMD_GET_STATUS       0x83
#define UNIT_CMD_GET_LETTER       0x84
#define UNIT_CMD_HOME             0x90
#define UNIT_CMD_JOG              0x91
#define UNIT_CMD_REBOOT           0x92
#define UNIT_CMD_SET_OFFSET       0x93

// General-call broadcast address — a write to 0x00 reaches every unit with
// TWGCE enabled. By convention the master only ever broadcasts CMD_HOME
// (see broadcastHome()); units treat received opcodes the same whether
// addressed individually or via general call, so other opcodes on broadcast
// would produce unintended side effects.
#define I2C_GENERAL_CALL_ADDRESS  0x00

static int toI2cAddress(int unitIndex) {
  return I2C_ADDRESS_BASE + unitIndex;
}

// Defined later in this file but used inside showMessage(); explicit
// prototypes because the Arduino preprocessor's auto-prototyping is not
// relied on for statics (see CLAUDE.md gotchas).
static bool waitForDisplayToStop();
static void verifyAndResendLetters(const int* commandedLetters, int flapSpeed);

//Send the enter-bootloader opcode to a specific unit by I2C address. The
//unit triggers a watchdog reset and comes up in twiboot listening on 0x29
//for ~1 s. Returns Wire.endTransmission()'s status (0 = success).
int rebootUnitToBootloader(int i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write(UNIT_CMD_ENTER_BOOTLOADER);
  return Wire.endTransmission();
}

//Interactive calibration helpers (issue #32). All four talk to opcodes that
//only post-#28 firmware understands; older units silently drop the write
//(opcode namespace is reserved) but return their 1-byte status on read, so
//readUnitOffset() flags that as failure via `got != 2`.

//Reads the unit's current calOffset (int16 LE). Returns true on success;
//out is untouched on failure.
bool readUnitOffset(int i2cAddress, int16_t &out) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)UNIT_CMD_GET_OFFSET);
  if (Wire.endTransmission() != 0) return false;
  delay(2);  //give the slave time to flip pendingOffsetResponse before clocking
  uint8_t got = Wire.requestFrom((uint8_t)i2cAddress, (uint8_t)2);
  if (got != 2) {
    while (Wire.available()) Wire.read();
    return false;
  }
  uint8_t lo = Wire.read();
  uint8_t hi = Wire.read();
  out = (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
  return true;
}

//Writes a new calOffset to the unit's EEPROM. Does NOT re-home. Returns
//Wire.endTransmission() status (0 = success).
int writeUnitOffset(int i2cAddress, int16_t value) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)UNIT_CMD_SET_OFFSET);
  Wire.write((uint8_t)((uint16_t)value & 0xFF));
  Wire.write((uint8_t)(((uint16_t)value >> 8) & 0xFF));
  return Wire.endTransmission();
}

//Nudges the drum without re-homing. Steps clamped to int8 range.
int jogUnit(int i2cAddress, int steps) {
  if (steps > 127) steps = 127;
  if (steps < -127) steps = -127;
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)UNIT_CMD_JOG);
  Wire.write((uint8_t)(int8_t)steps);
  return Wire.endTransmission();
}

//Triggers a full calibrate(true) on the unit and parks it at blank.
int homeUnit(int i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)UNIT_CMD_HOME);
  return Wire.endTransmission();
}

//Triggers a soft watchdog reset on the unit (stays in sketch mode; twiboot
//times out with no master activity and jumps to the sketch). Use to recover
//a unit that looks wedged without a full reflash. Issue #47.
int rebootUnit(int i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)UNIT_CMD_REBOOT);
  return Wire.endTransmission();
}

//Broadcasts CMD_HOME to every unit on the bus in a single transaction via
//the I2C general-call address. Replaces loops that previously sent HOME to
//each detected unit one at a time. Issue #47.
int broadcastHome() {
  Wire.beginTransmission((uint8_t)I2C_GENERAL_CALL_ADDRESS);
  Wire.write((uint8_t)UNIT_CMD_HOME);
  return Wire.endTransmission();
}

//Reads the 8-byte health/status payload via CMD_GET_STATUS (issue #47).
//Returns true on success. Short replies (old firmware predating this
//opcode) or Wire failures return false without touching `out`.
bool readUnitStatus(int i2cAddress, UnitStatus& out) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)UNIT_CMD_GET_STATUS);
  if (Wire.endTransmission() != 0) return false;
  delay(2);  //give the slave time to flip pendingStatusResponse before clocking
  uint8_t got = Wire.requestFrom((uint8_t)i2cAddress, (uint8_t)8);
  if (got != 8) {
    while (Wire.available()) Wire.read();
    return false;
  }
  uint8_t buf[8];
  for (uint8_t i = 0; i < 8; i++) buf[i] = Wire.read();
  out.flags                 = buf[0];
  out.mcusrAtBoot           = buf[1];
  out.lifetimeBrownoutCount = buf[2];
  out.lifetimeWatchdogCount = buf[3];
  out.uptimeSeconds         = ((uint16_t)buf[4] << 8) | (uint16_t)buf[5];
  out.badCommandCount       = buf[6];
  //Byte 7 is last-homing-step / 16 (saturating); decode by reversing.
  out.lastHomingStepCount   = (uint16_t)buf[7] << 4;
  return true;
}

//Reads back the unit's currently displayed letter index via CMD_GET_LETTER
//(issue #106). New firmware replies 2 bytes: index + bitwise complement;
//old firmware replies with the 1-byte status fallback, so `got != 2` (or a
//failed complement check) returns false and `out` is untouched.
bool readUnitDisplayedLetter(int i2cAddress, int &out) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)UNIT_CMD_GET_LETTER);
  if (Wire.endTransmission() != 0) return false;
  delay(2);  //give the slave time to flip pendingLetterResponse before clocking
  uint8_t got = Wire.requestFrom((uint8_t)i2cAddress, (uint8_t)2);
  if (got != 2) {
    while (Wire.available()) Wire.read();
    return false;
  }
  uint8_t value = Wire.read();
  uint8_t complement = Wire.read();
  if (!letterReadbackValid(value, complement, FLAP_AMOUNT)) return false;
  out = value;
  return true;
}

//Shows a new message on the display
void showText(String message) {  
  showText(message, 0);
}

void showText(String message, int delayMillis) {  
  if (lastWrittenText != message || alignmentUpdated) { 
    String messageDisplay = message == "" ? "<Blank>" : message;
    String alignmentUpdatedDisplay = alignmentUpdated ? "Yes" : "No";

    SerialPrintln(F("Showing new Message"));
    SerialPrintln("New Message: " + messageDisplay);
    SerialPrintln("Alignment Updated: " + alignmentUpdatedDisplay);
  
    std::vector<String> messageLines = processSentenceToLines(message);

    if (messageLines.size() > 1) {
      SerialPrintln(F("Showing a split down message"));
    
      //Iterate over all the message lines we've got
      for (int linesIndex = 0; linesIndex < messageLines.size(); linesIndex++) {
        String line = messageLines[linesIndex];

        SerialPrint(F("-- Message Line: "));
        SerialPrintln(line);

        showMessage(line, convertSpeed(flapSpeed));
    
        //If the lines index isn't the last, delay showing the next message to give time to read
        if (linesIndex < (int)messageLines.size() - 1) {
          delay(3000);
        }
      }  
    }
    else {
      SerialPrintln("Showing a simple message: " + message);
      
      showMessage(message, convertSpeed(flapSpeed));
    }  

    //If the device wasn't previously in text mode, delay for a short time so can read!
    if (delayMillis != 0) {
      SerialPrintln("Pausing for a small duration. Delay: " + String(delayMillis));
      delay(delayMillis);
    }

    //Save what we last did
    lastWrittenText = message;

    //Alignment definitely has not changed now
    alignmentUpdated = false;
    
    SerialPrintln(F("Done showing message"));
  }
}

//Pushes message to units
void showMessage(String message, int flapSpeed) {
  //Snapshot the width for this whole call (#123). waitForDisplayToStop()'s
  //delay() (and even Wire clock-stretch waits) yield to the async web
  //server, and /reflash-units re-runs probeI2cBus() — which mutates
  //displayWidth — from its handler. One value for both the alignment pad
  //and the write-loop bound keeps the call self-consistent.
  const int width = displayWidth;

  //Format string per alignment choice
  if (alignment == ALIGNMENT_MODE_LEFT) {
    message = leftString(message);
  } 
  else if (alignment == ALIGNMENT_MODE_RIGHT) {
    message = rightString(message);
  } 
  else if (alignment == ALIGNMENT_MODE_CENTER) {
    message = centerString(message);
  }

  SerialPrint(F("Showing Aligned Message: \""));
  SerialPrint(message);
  SerialPrintln(F("\""));

  //Wait while display is still moving, with a hard timeout so a physically
  //stuck unit (status byte pegged at 1) doesn't deadlock the event loop.
  //Abortable via /stop (issue #35).
  abortCurrentShow = false;
  SerialPrintln(F("Unit calls are enabled. Will display message"));
  if (!waitForDisplayToStop()) {
    SerialPrintln(F("Show aborted by user (entry wait)"));
    return;
  }

  //Track what was actually commanded per unit so the post-show verification
  //pass can read it back and re-send on mismatch (issue #106). -1 = slot
  //not written (absent unit or char missing from the alphabet).
  int commandedLetters[UNITS_AMOUNT];
  for (int unitIndex = 0; unitIndex < UNITS_AMOUNT; unitIndex++) {
    commandedLetters[unitIndex] = -1;
  }

  int writeErrors = 0;
  //Aligned messages are padded to `width`, so slots beyond it hold
  //nothing to show; they are all probe-silent anyway.
  for (int unitIndex = 0; unitIndex < width; unitIndex++) {
    //Skip slots the boot-time bus probe did not find a sketch-running unit on.
    //Writing to absent addresses causes isDisplayMoving() to stall and, more
    //importantly, means users with dead or missing units mid-display
    //no longer deadlock the event loop.
    if (detectedUnitStates[unitIndex] != 1) {
      continue;
    }

    char currentLetter = message[unitIndex];
    int currentLetterPosition = translateLettertoInt(currentLetter);

    SerialPrint(F("Unit Nr.: "));
    SerialPrint(unitIndex);
    SerialPrint(F(" Letter: "));
    SerialPrint(message[unitIndex]);
    SerialPrint(F(" Letter position: "));
    SerialPrintln(currentLetterPosition);

    //only write to unit if char exists in letter array
    if (currentLetterPosition != -1) {
      if (writeToUnit(unitIndex, currentLetterPosition, flapSpeed) != 0) {
        writeErrors++;
      }
      commandedLetters[unitIndex] = currentLetterPosition;
    }
  }

  lastShowUnitWriteErrors = writeErrors;

  //Wait for the display to stop moving, then verify each unit actually
  //shows what was commanded (issue #106).
  if (!waitForDisplayToStop()) {
    SerialPrintln(F("Show aborted by user (exit wait)"));
    return;
  }
  verifyAndResendLetters(commandedLetters, flapSpeed);
}

//Waits until no unit reports rotation, with the /stop abort and a 30 s
//stuck-unit timeout. Returns false when aborted. Extracted from
//showMessage() — the same loop now runs at entry, exit, and after the
//verification re-send (issue #106). Log line rate-limited to once per
//5 s — this used to spam /log at ~10 Hz (issue #35).
static bool waitForDisplayToStop() {
  unsigned long waitStart = millis();
  unsigned long lastWaitLog = 0;
  while (isDisplayMoving()) {
    if (abortCurrentShow) {
      return false;
    }
    if (millis() - waitStart > 30000) {
      SerialPrintln(F("Wait timed out after 30s — assuming a unit is stuck, continuing anyway"));
      break;
    }
    if (millis() - lastWaitLog > 5000) {
      SerialPrintln(F("Waiting for display to stop"));
      lastWaitLog = millis();
    }
    delay(100);
  }
  return true;
}

//Closed-loop letter verification (issue #106). Reads back each written
//unit's displayed letter and re-sends once on mismatch — a corrupted or
//dropped I2C write no longer leaves the wrong character standing until the
//next message. Units on pre-#106 firmware reply with the 1-byte status
//fallback; readUnitDisplayedLetter() returns false and they are skipped.
static void verifyAndResendLetters(const int* commandedLetters, int flapSpeed) {
  int resent = 0;
  for (int unitIndex = 0; unitIndex < UNITS_AMOUNT; unitIndex++) {
    if (commandedLetters[unitIndex] < 0) continue;
    if (detectedUnitStates[unitIndex] != 1) continue;
    if (abortCurrentShow) return;
    int shown;
    if (!readUnitDisplayedLetter(toI2cAddress(unitIndex), shown)) continue;
    if (shown == commandedLetters[unitIndex]) continue;
    SerialPrint(F("Unit "));
    SerialPrint(unitIndex);
    SerialPrint(F(" shows letter index "));
    SerialPrint(shown);
    SerialPrint(F(" instead of "));
    SerialPrint(commandedLetters[unitIndex]);
    SerialPrintln(F(" — re-sending"));
    writeToUnit(unitIndex, commandedLetters[unitIndex], flapSpeed);
    resent++;
  }
  if (resent > 0) {
    SerialPrint(F("Letter verification re-sent "));
    SerialPrint(resent);
    SerialPrintln(F(" unit(s)"));
    waitForDisplayToStop();
  }
}

//Translates char to letter position
int translateLettertoInt(char letterchar) {
  for (int flapIndex = 0; flapIndex < FLAP_AMOUNT; flapIndex++) {
    if (letterchar == letters[flapIndex]) {
      return flapIndex;
    }
  }

  return -1;
}

//Write letter position and speed in rpm to single unit (by 0-based unit
//index). Returns Wire.endTransmission() status (0 = success) so callers
//can tally bus-level failures (#121).
int writeToUnit(int unitIndex, int letter, int flapSpeed) {
  int sendArray[2] = {letter, flapSpeed}; //Array with values to send to unit

  Wire.beginTransmission(toI2cAddress(unitIndex));

  //Write values to send to slave in buffer
  for (unsigned int index = 0; index < sizeof sendArray / sizeof sendArray[0]; index++) {
    SerialPrint(F("sendArray: "));
    SerialPrintln(sendArray[index]);

    Wire.write(sendArray[index]);
  }
  return Wire.endTransmission(); //send values to unit
}

//Checks if unit in display is currently moving. Only queries units the boot
//probe confirmed are running the sketch; a silent read (-1) from such a unit
//is treated as "idle" rather than "sleeping" so the master never deadlocks
//on a transiently unresponsive (or physically absent) unit.
bool isDisplayMoving() {
  for (int unitIndex = 0; unitIndex < UNITS_AMOUNT; unitIndex++) {
    if (detectedUnitStates[unitIndex] != 1) {
      displayState[unitIndex] = 0;
      continue;
    }
    displayState[unitIndex] = checkIfMoving(unitIndex);
    if (displayState[unitIndex] == 1) {
      //Don't log per-iteration — the caller's rate-limited wait loop
      //already reports "waiting for display to stop" every 5s. This used
      //to fire ~10×/s and drowned out everything else (issue #35).
      return true;
    }
  }

  return false;
}

//Checks if single unit is moving (by 0-based unit index). Called ~10×/s
//from isDisplayMoving() — must be quiet on /log when nothing is wrong.
int checkIfMoving(int unitIndex) {
  int i2cAddress = toI2cAddress(unitIndex);
  int active;
  Wire.requestFrom(i2cAddress, ANSWER_SIZE, 1);
  active = Wire.read();

  if (active == -1) {
    //Wake-up ping: empty transmission pulses the TWI peripheral. Log once
    //(the caller throttles its own wait-log), not per iteration.
    Wire.beginTransmission(i2cAddress);
    Wire.endTransmission();
  }

  return active;
}

//Scans the I2C bus once for responders in the range reserved for units.
//Populates detectedUnitCount, detectedUnitAddresses, and detectedUnitStates
//(0 = silent, 1 = sketch, 2 = bootloader).
int detectedUnitCount = 0;
int detectedUnitAddresses[UNITS_AMOUNT];
int detectedUnitStates[UNITS_AMOUNT];
// Version-check state, populated after each successful sketch-mode detection.
// Status codes match the comment in ESPMaster.h: 0=ok, 1=outdated, 2=unknown.
int detectedUnitVersionStatus[UNITS_AMOUNT];
char detectedUnitVersions[UNITS_AMOUNT][9];

// Asks a sketch-mode unit for its firmware GIT_REV via CMD_GET_VERSION. Writes
// up to 8 printable ASCII bytes into `out` (null-terminated). Returns true iff
// the unit returned 8 bytes that look like a valid rev (all printable).
// Old firmware that doesn't know the opcode will still ACK the write (the
// opcode namespace silently drops unknowns) but its requestEvent returns only
// the 1-byte rotation status, so `got != 8` — we flag that as "unknown".
static bool readUnitVersion(int i2cAddress, char *out) {
  out[0] = '\0';
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)UNIT_CMD_GET_VERSION);
  if (Wire.endTransmission() != 0) return false;
  // Give the slave a moment to flip its pending-version flag before we clock.
  delay(2);
  uint8_t got = Wire.requestFrom((uint8_t)i2cAddress, (uint8_t)8);
  if (got != 8) {
    while (Wire.available()) Wire.read();
    return false;
  }
  uint8_t buf[8];
  for (uint8_t i = 0; i < 8; i++) buf[i] = Wire.read();
  // Copy to out, stopping at first null; reject if any non-printable non-null.
  uint8_t len = 0;
  for (; len < 8; len++) {
    if (buf[len] == 0) break;
    if (buf[len] < 32 || buf[len] > 126) return false;
  }
  if (len == 0) return false;
  for (uint8_t i = 0; i < len; i++) out[i] = (char)buf[i];
  out[len] = '\0';
  return true;
}

// Writes twiboot's CMD_ACCESS_MEMORY + CHIPINFO request, reads the 8-byte
// chipinfo response and checks whether the signature matches the ATmega328P.
// Safe to call against a sketch-running unit: the patched Unit.ino ignores
// writes of length != 2, so probing doesn't rotate the drum as a side effect.
bool isUnitInBootloader(int i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)0x02);  // CMD_ACCESS_MEMORY
  Wire.write((uint8_t)0x00);  // MEMTYPE_CHIPINFO
  Wire.write((uint8_t)0x00);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t got = Wire.requestFrom((uint8_t)i2cAddress, (uint8_t)8);
  if (got < 3) {
    while (Wire.available()) Wire.read();
    return false;
  }
  uint8_t sig0 = Wire.read();
  uint8_t sig1 = Wire.read();
  uint8_t sig2 = Wire.read();
  while (Wire.available()) Wire.read();
  return (sig0 == 0x1E && sig1 == 0x95 && sig2 == 0x0F);
}

void probeI2cBus() {
#if SERIAL_ENABLE == false
  SerialPrintln(F("Scanning I2C bus for units..."));
  detectedUnitCount = 0;
  for (int unitIndex = 0; unitIndex < UNITS_AMOUNT; unitIndex++) {
    detectedUnitStates[unitIndex] = 0;  // silent by default
    detectedUnitVersionStatus[unitIndex] = 2;  // unknown by default
    detectedUnitVersions[unitIndex][0] = '\0';
    int i2cAddress = toI2cAddress(unitIndex);
    Wire.beginTransmission(i2cAddress);
    if (Wire.endTransmission() != 0) continue;

    bool inBootloader = isUnitInBootloader(i2cAddress);
    detectedUnitStates[unitIndex] = inBootloader ? 2 : 1;
    detectedUnitAddresses[detectedUnitCount++] = i2cAddress;

    SerialPrint(F("- unit at 0x"));
    SerialPrint(String(i2cAddress, HEX));
    if (inBootloader) {
      SerialPrintln(F(" is in BOOTLOADER mode"));
      continue;
    }
    SerialPrint(F(" is running sketch"));

    // Compare against the bundled unit firmware's rev (BUNDLED_UNIT_REV),
    // NOT the master's own GIT_REV. Because unit-firmware.hex is checked in,
    // the commit that updates it also bumps the master's rev — so using
    // master's rev would always flag fresh installs as OUTDATED. See #31.
    // Old firmware that predates CMD_GET_VERSION returns a short response
    // and is flagged "unknown".
    if (readUnitVersion(i2cAddress, detectedUnitVersions[unitIndex])) {
      // BUNDLED_UNIT_REV may be longer than 8 chars; unit truncates to 8,
      // so compare only the leading 8 chars.
      bool match = (strncmp(detectedUnitVersions[unitIndex], BUNDLED_UNIT_REV, 8) == 0);
      detectedUnitVersionStatus[unitIndex] = match ? 0 : 1;
      SerialPrint(match ? " (fw " : " (fw OUTDATED ");
      SerialPrint(detectedUnitVersions[unitIndex]);
      SerialPrintln(F(")"));
    } else {
      detectedUnitVersionStatus[unitIndex] = 2;
      SerialPrintln(F(" (fw UNKNOWN — likely predates version opcode)"));
    }
  }
  //Derive the effective display width (#123): highest responder + 1, so a
  //dead unit mid-display keeps its slot; falls back to the ceiling when
  //nothing responds so a unit-less bench ESP still lays out full-width.
  displayWidth = computeDisplayWidth(detectedUnitStates, UNITS_AMOUNT);
  SerialPrint(F("I2C scan complete. Detected "));
  SerialPrint(detectedUnitCount);
  SerialPrint(F("/"));
  SerialPrint(UNITS_AMOUNT);
  SerialPrint(F(" possible units. Display width: "));
  SerialPrintln(displayWidth);
#endif
}

//Shows the device's IPv4 address on the flaps at boot, octet by octet
//(issue #111): "192." -> "168." -> "1." -> "123", 3 s apart, last chunk
//held 5 s. A trailing dot marks continuation. Chunks are left-aligned so
//the octets read from a fixed anchor. leftString() pads to displayWidth,
//which makes showMessage()'s own alignment pass a no-op.
void showIpAddressOnBoot() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  IPAddress ip = WiFi.localIP();
  SerialPrint(F("Showing IP on display: "));
  SerialPrintln(ip.toString());
  for (int octet = 0; octet < 4; octet++) {
    String chunk = String(ip[octet]);
    if (octet < 3) {
      chunk += ".";
    }
    showMessage(leftString(chunk), convertSpeed(flapSpeed));
    delay(octet < 3 ? 3000 : 5000);
  }
}
