// I2C address 0x00 is reserved (general call); offset every unit's address so
// the master's 0-based unit index maps to 0x01..0x10. Must match
// I2C_ADDRESS_BASE in Unit/Unit.ino.
#define I2C_ADDRESS_BASE 1

// Command opcodes understood by Unit.ino's receiveLetter(). Values >=
// FLAP_AMOUNT are reserved for commands (valid letter indices are 0..44).
// Must stay in sync with Unit/Unit.ino's CMD_* constants.
#define UNIT_CMD_ENTER_BOOTLOADER 0x80

static int toI2cAddress(int unitIndex) {
  return I2C_ADDRESS_BASE + unitIndex;
}

//Send the enter-bootloader opcode to a specific unit by I2C address. The
//unit triggers a watchdog reset and comes up in twiboot listening on 0x29
//for ~1 s. Returns Wire.endTransmission()'s status (0 = success).
int rebootUnitToBootloader(int i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write(UNIT_CMD_ENTER_BOOTLOADER);
  return Wire.endTransmission();
}

//Shows a new message on the display
void showText(String message) {  
  showText(message, 0);
}

void showText(String message, int delayMillis) {  
  if (lastWrittenText != message || alignmentUpdated) { 
    String messageDisplay = message == "" ? "<Blank>" : message;
    String alignmentUpdatedDisplay = alignmentUpdated ? "Yes" : "No";

    SerialPrintln("Showing new Message");
    SerialPrintln("New Message: " + messageDisplay);
    SerialPrintln("Alignment Updated: " + alignmentUpdatedDisplay);
  
    std::vector<String> messageLines = processSentenceToLines(message);

    if (messageLines.size() > 1) {
      SerialPrintln("Showing a split down message");
    
      //Iterate over all the message lines we've got
      for (int linesIndex = 0; linesIndex < messageLines.size(); linesIndex++) {
        String line = messageLines[linesIndex];

        SerialPrint("-- Message Line: ");
        SerialPrintln(line);

        showMessage(line, convertSpeed(flapSpeed));
    
        //If the lines index isn't the last, delay showing the next message to give time to read
        if (linesIndex <= messageLines.size()) {
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
    
    SerialPrintln("Done showing message");
  }
}

//Pushes message to units
void showMessage(String message, int flapSpeed) {
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

  SerialPrint("Showing Aligned Message: \"");
  SerialPrint(message);
  SerialPrintln("\"");

#if UNIT_CALLS_DISABLE == true
  SerialPrintln("Unit Calls are disabled for debugging. Will delay to simulate calls...");
  delay(2000);
#else
  //Wait while display is still moving
  SerialPrintln("Unit calls are enabled. Will display message");
  while (isDisplayMoving()) {
    SerialPrintln("Waiting for display to stop");
    delay(500);
  }

  for (int unitIndex = 0; unitIndex < UNITS_AMOUNT; unitIndex++) {
    //Skip slots the boot-time bus probe did not find a sketch-running unit on.
    //Writing to absent addresses causes isDisplayMoving() to stall and, more
    //importantly, means users with fewer than UNITS_AMOUNT physical units
    //no longer deadlock the event loop.
    if (detectedUnitStates[unitIndex] != 1) {
      continue;
    }

    char currentLetter = message[unitIndex];
    int currentLetterPosition = translateLettertoInt(currentLetter);

    SerialPrint("Unit Nr.: ");
    SerialPrint(unitIndex);
    SerialPrint(" Letter: ");
    SerialPrint(message[unitIndex]);
    SerialPrint(" Letter position: ");
    SerialPrintln(currentLetterPosition);

    //only write to unit if char exists in letter array
    if (currentLetterPosition != -1) {
      writeToUnit(unitIndex, currentLetterPosition, flapSpeed);
    }
  }

  //Wait for the display to stop moving before exit
  while (isDisplayMoving()) {
    SerialPrintln("Waiting for display to stop now message is display");
    delay(100);
  }
#endif
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

//Write letter position and speed in rpm to single unit (by 0-based unit index).
void writeToUnit(int unitIndex, int letter, int flapSpeed) {
  int sendArray[2] = {letter, flapSpeed}; //Array with values to send to unit

  Wire.beginTransmission(toI2cAddress(unitIndex));

  //Write values to send to slave in buffer
  for (unsigned int index = 0; index < sizeof sendArray / sizeof sendArray[0]; index++) {
    SerialPrint("sendArray: ");
    SerialPrintln(sendArray[index]);

    Wire.write(sendArray[index]);
  }
  Wire.endTransmission(); //send values to unit
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
      SerialPrintln("A unit in the display is busy");
      return true;
    }
  }

  SerialPrintln("Display is standing still");
  return false;
}

//Checks if single unit is moving (by 0-based unit index).
int checkIfMoving(int unitIndex) {
  int i2cAddress = toI2cAddress(unitIndex);
  int active;
  Wire.requestFrom(i2cAddress, ANSWER_SIZE, 1);
  active = Wire.read();

  SerialPrint(i2cAddress);
  SerialPrint(":");
  SerialPrintln(active);

  if (active == -1) {
    SerialPrintln("Try to wake up unit");
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
#if SERIAL_ENABLE == false && UNIT_CALLS_DISABLE == false
  SerialPrintln("Scanning I2C bus for units...");
  detectedUnitCount = 0;
  for (int unitIndex = 0; unitIndex < UNITS_AMOUNT; unitIndex++) {
    detectedUnitStates[unitIndex] = 0;  // silent by default
    int i2cAddress = toI2cAddress(unitIndex);
    Wire.beginTransmission(i2cAddress);
    if (Wire.endTransmission() != 0) continue;

    bool inBootloader = isUnitInBootloader(i2cAddress);
    detectedUnitStates[unitIndex] = inBootloader ? 2 : 1;
    detectedUnitAddresses[detectedUnitCount++] = i2cAddress;

    SerialPrint("- unit at 0x");
    SerialPrint(String(i2cAddress, HEX));
    SerialPrintln(inBootloader ? " is in BOOTLOADER mode" : " is running sketch");
  }
  SerialPrint("I2C scan complete. Detected ");
  SerialPrint(detectedUnitCount);
  SerialPrint("/");
  SerialPrint(UNITS_AMOUNT);
  SerialPrintln(" expected units.");
#endif
}
