// Firmware OTA for the Nano units.
//
// High-level flow driven by the /firmware/unit HTTP endpoint:
//   1. beginFirmwareFlash(addr): if the unit is running the sketch, send the
//      enter-bootloader I2C opcode and wait for twiboot to come up. Our
//      patched twiboot listens on the unit's DIP-derived address (not stock
//      0x29), so every twiboot call targets `addr` directly.
//   2. feedFirmwareChunk(data, len): called for each chunk of the uploaded
//      Intel HEX file. Parses line-by-line, accumulates into 128-byte pages,
//      flushes each full page to twiboot as it completes.
//   3. finishFirmwareFlash(): flush any trailing page, exit the bootloader,
//      verify the unit came back up at its normal address.
//
// Twiboot protocol: see the comment in UnitBootloader/main.c around line 150.
//
// IMPORTANT: Arduino preprocessor auto-generates function prototypes and
// puts them ABOVE every #include. That means custom types can't appear in
// any function signature or the build breaks with "<type> not declared".
// Anything that needs to be stateful is stored in file-static globals
// rather than passed via parameters.

#define TWIBOOT_PAGE_SIZE       128
// ATmega328p flash is 32KB. Bootloader sits at 0x7C00; the rest is the app
// section we're allowed to write. Addresses past this should be refused.
#define TWIBOOT_APP_MAX         0x7C00
// Upper bound on cumulative uploaded HEX file size. Way more than the app
// section can hold, but catches obviously-wrong uploads fast.
#define FIRMWARE_MAX_HEX_BYTES  (64UL * 1024UL)

volatile bool firmwareFlashInProgress = false;

// File-scope state for the currently-running flash. All flash helpers read
// and write these directly — no custom structs in function signatures (see
// header comment).
static uint8_t  flashPageBuf[TWIBOOT_PAGE_SIZE];
static uint16_t flashCurrentPageAddr = 0;
static bool     flashPageHasData     = false;
static uint32_t flashTotalBytesWritten = 0;
static uint32_t flashTotalHexBytesSeen = 0;
static uint8_t  flashTargetI2cAddress = 0;
// Address used when talking to twiboot. Our DIP-patched bootloader listens on
// the unit's own I2C address (I2C_ADDRESS_BASE + DIP), so this is always set
// to the target unit's address at the start of a flash.
static uint8_t  twibootAddr           = 0;
static String   flashErrorMsg         = "";
static char     flashLineBuf[64];
static int      flashLineLen          = 0;

// --- Twiboot I2C client --------------------------------------------------

static int twibootPing() {
  Wire.beginTransmission(twibootAddr);
  Wire.write((uint8_t)0x00);  // CMD_WAIT — resets twiboot's boot-window countdown
  return Wire.endTransmission();
}

static int twibootExit() {
  Wire.beginTransmission(twibootAddr);
  Wire.write((uint8_t)0x01);  // CMD_SWITCH_APPLICATION
  Wire.write((uint8_t)0x80);  // BOOTTYPE_APPLICATION
  return Wire.endTransmission();
}

// Queries twiboot chipinfo and checks it matches the ATmega328P. Populates
// `flashErrorMsg` on mismatch.
static bool twibootVerifyChip() {
  Wire.beginTransmission(twibootAddr);
  Wire.write((uint8_t)0x02);  // CMD_ACCESS_MEMORY
  Wire.write((uint8_t)0x00);  // MEMTYPE_CHIPINFO
  Wire.write((uint8_t)0x00);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission(false) != 0) {
    flashErrorMsg = "Wire endTransmission failed reading chipinfo";
    return false;
  }
  uint8_t got = Wire.requestFrom(twibootAddr, (uint8_t)8);
  if (got != 8) {
    flashErrorMsg = String("Chipinfo read returned ") + got + " bytes, expected 8";
    return false;
  }
  uint8_t sig0 = Wire.read(), sig1 = Wire.read(), sig2 = Wire.read();
  uint8_t pageSize = Wire.read();
  Wire.read(); Wire.read();  // flash size, unused
  Wire.read(); Wire.read();  // eeprom size, unused

  if (sig0 != 0x1E || sig1 != 0x95 || sig2 != 0x0F) {
    flashErrorMsg = String("Unexpected chip signature ") + String(sig0, HEX) + " " +
                    String(sig1, HEX) + " " + String(sig2, HEX);
    return false;
  }
  if (pageSize != TWIBOOT_PAGE_SIZE) {
    flashErrorMsg = String("Unexpected page size ") + pageSize;
    return false;
  }
  return true;
}

// Writes exactly one page (SPM_PAGESIZE = 128 bytes on ATmega328p).
static int twibootWriteFlashPage(uint16_t flashAddr) {
  Wire.beginTransmission(twibootAddr);
  Wire.write((uint8_t)0x02);  // CMD_ACCESS_MEMORY
  Wire.write((uint8_t)0x01);  // MEMTYPE_FLASH
  Wire.write((uint8_t)((flashAddr >> 8) & 0xFF));
  Wire.write((uint8_t)(flashAddr & 0xFF));
  for (int i = 0; i < TWIBOOT_PAGE_SIZE; i++) {
    Wire.write(flashPageBuf[i]);
  }
  return Wire.endTransmission();
}

// --- Intel HEX parser + page assembler -----------------------------------

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int hexByte(char hi, char lo) {
  int h = hexNibble(hi), l = hexNibble(lo);
  if (h < 0 || l < 0) return -1;
  return (h << 4) | l;
}

// Spin-poll twiboot with CMD_WAIT until it ACKs again (meaning its async
// flash-write finished) or we give up. Clock stretching is disabled in our
// patched twiboot build, so during the ~4.5 ms page-program window twiboot
// simply NACKs. Return true if twiboot is ready, false on timeout.
static bool twibootWaitReady(uint16_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  while ((int32_t)(millis() - deadline) < 0) {
    if (twibootPing() == 0) return true;
    delay(1);
  }
  return false;
}

static bool flushPageIfReady() {
  if (!flashPageHasData) return true;
  SerialPrint("Writing page 0x");
  SerialPrintln(String(flashCurrentPageAddr, HEX));

  // Make sure twiboot is ready before hitting it with a 132-byte burst.
  if (!twibootWaitReady(100)) {
    flashErrorMsg = String("twiboot not ready before page 0x") + String(flashCurrentPageAddr, HEX);
    return false;
  }

  int status = twibootWriteFlashPage(flashCurrentPageAddr);
  if (status != 0) {
    flashErrorMsg = String("twibootWriteFlashPage failed at 0x") + String(flashCurrentPageAddr, HEX) +
                    " status=" + String(status);
    return false;
  }

  // Wait for the async SPM cycle to finish. ~4.5 ms typical; 50 ms is
  // generous and still keeps the whole flash under a few seconds.
  if (!twibootWaitReady(50)) {
    flashErrorMsg = String("twiboot stuck busy after page 0x") + String(flashCurrentPageAddr, HEX);
    return false;
  }

  flashTotalBytesWritten += TWIBOOT_PAGE_SIZE;
  memset(flashPageBuf, 0xFF, TWIBOOT_PAGE_SIZE);
  flashPageHasData = false;
  return true;
}

// Parses one complete Intel HEX record. Returns true on parse OK (and
// possibly EOF), false on error (with flashErrorMsg set).
static bool parseHexLine(const char* line, int len) {
  if (len < 11 || line[0] != ':') {
    flashErrorMsg = "Invalid HEX line";
    return false;
  }

  int byteCount  = hexByte(line[1], line[2]);
  int addrHi     = hexByte(line[3], line[4]);
  int addrLo     = hexByte(line[5], line[6]);
  int recordType = hexByte(line[7], line[8]);
  if (byteCount < 0 || addrHi < 0 || addrLo < 0 || recordType < 0) {
    flashErrorMsg = "Malformed HEX header";
    return false;
  }

  uint16_t baseAddr = ((uint16_t)addrHi << 8) | addrLo;

  if (recordType == 0x01) return true;      // EOF
  if (recordType != 0x00) return true;      // 02/04 extended-address, ignored for 32KB targets

  if (len < 9 + byteCount * 2 + 2) {
    flashErrorMsg = "HEX data record truncated";
    return false;
  }

  for (int i = 0; i < byteCount; i++) {
    int b = hexByte(line[9 + i * 2], line[9 + i * 2 + 1]);
    if (b < 0) {
      flashErrorMsg = "Bad HEX data byte";
      return false;
    }

    uint16_t addr = baseAddr + i;
    if (addr >= TWIBOOT_APP_MAX) {
      flashErrorMsg = String("Address 0x") + String(addr, HEX) + " past app section";
      return false;
    }

    uint16_t pageBase   = addr & ~((uint16_t)TWIBOOT_PAGE_SIZE - 1);
    uint8_t  pageOffset = addr & (TWIBOOT_PAGE_SIZE - 1);

    if (flashPageHasData && pageBase != flashCurrentPageAddr) {
      if (!flushPageIfReady()) return false;
    }

    flashCurrentPageAddr     = pageBase;
    flashPageBuf[pageOffset] = (uint8_t)b;
    flashPageHasData         = true;
  }

  return true;
}

// --- Public API ----------------------------------------------------------

bool beginFirmwareFlash(uint8_t i2cAddress, String& error) {
  if (firmwareFlashInProgress) {
    error = "A flash is already in progress";
    return false;
  }

  SerialPrint("Firmware flash starting, target 0x");
  SerialPrintln(String(i2cAddress, HEX));

  // Our DIP-patched twiboot listens on the unit's own address, so that's
  // where every twiboot command must go.
  twibootAddr = i2cAddress;

  // If the unit is already sitting in twiboot, skip the reboot — sending
  // the enter-bootloader opcode to twiboot would be interpreted as the
  // unknown command 0x80. Probe with a chipinfo read.
  bool alreadyInBootloader = isUnitInBootloader((int)i2cAddress);

  if (!alreadyInBootloader) {
    int rebootStatus = rebootUnitToBootloader((int)i2cAddress);
    if (rebootStatus != 0) {
      error = String("Unit did not ack enter-bootloader (Wire status ") + rebootStatus + ")";
      return false;
    }
    SerialPrintln("Sent enter-bootloader opcode; waiting for twiboot");

    // Watchdog reset (~15ms) + twiboot init. 500ms is generous.
    delay(500);
  } else {
    SerialPrintln("Unit already in bootloader; skipping reboot");
  }

  bool bootloaderLive = false;
  for (int attempt = 0; attempt < 5; attempt++) {
    if (twibootPing() == 0) {
      bootloaderLive = true;
      break;
    }
    delay(100);
  }
  if (!bootloaderLive) {
    error = String("Twiboot not responding on 0x") + String(i2cAddress, HEX);
    return false;
  }
  SerialPrint("Twiboot responding on 0x");
  SerialPrintln(String(i2cAddress, HEX));

  if (!twibootVerifyChip()) {
    error = flashErrorMsg;
    return false;
  }
  SerialPrintln("Chip signature OK (ATmega328P, page=128)");

  memset(flashPageBuf, 0xFF, TWIBOOT_PAGE_SIZE);
  flashCurrentPageAddr     = 0;
  flashPageHasData         = false;
  flashTotalBytesWritten   = 0;
  flashTotalHexBytesSeen   = 0;
  flashTargetI2cAddress    = i2cAddress;
  flashErrorMsg            = "";
  flashLineLen             = 0;

  firmwareFlashInProgress = true;
  return true;
}

bool feedFirmwareChunk(const uint8_t* data, size_t len) {
  if (!firmwareFlashInProgress) return false;

  flashTotalHexBytesSeen += len;
  if (flashTotalHexBytesSeen > FIRMWARE_MAX_HEX_BYTES) {
    flashErrorMsg = "HEX upload exceeded size limit";
    return false;
  }

  for (size_t i = 0; i < len; i++) {
    char c = (char)data[i];
    if (c == '\n' || c == '\r') {
      if (flashLineLen > 0) {
        bool ok = parseHexLine(flashLineBuf, flashLineLen);
        flashLineLen = 0;
        if (!ok) {
          SerialPrint("Firmware chunk error: ");
          SerialPrintln(flashErrorMsg);
          return false;
        }
      }
    } else {
      if (flashLineLen < (int)sizeof(flashLineBuf) - 1) {
        flashLineBuf[flashLineLen++] = c;
      } else {
        flashErrorMsg = "HEX line exceeds internal buffer";
        return false;
      }
    }
  }
  return true;
}

bool finishFirmwareFlash(String& resultMsg) {
  if (!firmwareFlashInProgress) {
    resultMsg = "No flash in progress";
    return false;
  }

  uint8_t targetAddr = flashTargetI2cAddress;

  if (!flushPageIfReady()) {
    resultMsg = String("Failed flushing final page: ") + flashErrorMsg;
    firmwareFlashInProgress = false;
    return false;
  }

  int exitStatus = twibootExit();
  if (exitStatus != 0) {
    resultMsg = String("Exit bootloader failed (Wire status ") + exitStatus + ")";
    firmwareFlashInProgress = false;
    return false;
  }

  uint32_t bytesWritten = flashTotalBytesWritten;
  firmwareFlashInProgress = false;

  // Give the new sketch a couple of seconds to boot, then verify it
  // answers at its expected address.
  delay(2000);
  Wire.beginTransmission(targetAddr);
  int postFlashStatus = Wire.endTransmission();

  resultMsg = String("Flashed ") + bytesWritten + " bytes to 0x" + String(targetAddr, HEX);
  if (postFlashStatus == 0) {
    resultMsg += " — unit responding on its normal address";
    SerialPrintln(resultMsg);
  } else {
    resultMsg += String(" — WARNING: unit not responding post-flash (status ") + postFlashStatus + ")";
    SerialPrintln(resultMsg);
  }
  return true;
}

void abortFirmwareFlash(const String& reason) {
  SerialPrint("Aborting firmware flash: ");
  SerialPrintln(reason);
  firmwareFlashInProgress = false;
}

// Streams the PROGMEM-embedded unit firmware to twiboot by copying
// 128-byte pages into flashPageBuf and calling flushPageIfReady() (which
// handles the clock-stretch-off wait/retry).
bool flashUnitFromProgmem(uint8_t i2cAddress, String& resultMsg) {
  String beginError;
  if (!beginFirmwareFlash(i2cAddress, beginError)) {
    resultMsg = beginError;
    return false;
  }

  size_t pageCount = UNIT_FIRMWARE_BIN_LEN / TWIBOOT_PAGE_SIZE;
  for (size_t pageIndex = 0; pageIndex < pageCount; pageIndex++) {
    memcpy_P(flashPageBuf, UNIT_FIRMWARE_BIN + pageIndex * TWIBOOT_PAGE_SIZE, TWIBOOT_PAGE_SIZE);
    flashCurrentPageAddr = (uint16_t)(pageIndex * TWIBOOT_PAGE_SIZE);
    flashPageHasData     = true;
    if (!flushPageIfReady()) {
      resultMsg = String("PROGMEM flash failed: ") + flashErrorMsg;
      abortFirmwareFlash("PROGMEM flash page error");
      return false;
    }
  }

  return finishFirmwareFlash(resultMsg);
}

// Called from setup() after probeI2cBus() has populated detectedUnitStates.
// For every unit the probe flagged as being in bootloader mode, stream the
// PROGMEM-embedded unit binary. Best-effort: one failure doesn't abort the
// others.
extern int detectedUnitStates[];

void autoInstallFirmwareToBootloaderUnits() {
#if SERIAL_ENABLE == false && UNIT_CALLS_DISABLE == false
  int flashedCount = 0;
  for (int unitIndex = 0; unitIndex < UNITS_AMOUNT; unitIndex++) {
    if (detectedUnitStates[unitIndex] != 2 /* bootloader */) continue;

    int i2cAddress = toI2cAddress(unitIndex);
    SerialPrint("Auto-flashing unit at 0x");
    SerialPrint(String(i2cAddress, HEX));
    SerialPrint(" from PROGMEM (");
    SerialPrint(UNIT_FIRMWARE_BIN_LEN);
    SerialPrintln(" bytes)");

    String result;
    bool ok = flashUnitFromProgmem((uint8_t)i2cAddress, result);
    SerialPrint(ok ? "  [ok] " : "  [FAILED] ");
    SerialPrintln(result);
    if (ok) {
      //Just-provisioned unit is now running the sketch. Bump its state so
      //showMessage()/isDisplayMoving() stop skipping it.
      detectedUnitStates[unitIndex] = 1;
      flashedCount++;
    }
  }

  if (flashedCount > 0) {
    SerialPrint("Auto-install complete: ");
    SerialPrint(flashedCount);
    SerialPrintln(" unit(s) updated.");
  }
#endif
}
