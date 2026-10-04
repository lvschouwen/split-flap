// In-system twiboot update (#499) — sketch glue.
//
// The self-program core (stage 1/2, read-back checks, auto-resume, brick
// windows) is BootUpdateAvr.h, which the simavr proof in
// firmware/v2/UnitBootloader/sim/ compiles and runs unchanged. This file adds
// only what needs the sketch: the drum-idle precondition, lock/fuse reads, the
// cached GET_BOOT_INFO reply, and TWI re-initialisation after stage 2.
//
// Concatenated after Unit.ino, so its globals/includes are visible here.

// The Z addresses bootLockFuseReadFellThrough() assumes (BootUpdateReport.h).
static_assert(GET_LOW_FUSE_BITS == 0 && GET_LOCK_BITS == 1 &&
                  GET_EXTENDED_FUSE_BITS == 2 && GET_HIGH_FUSE_BITS == 3,
              "avr/boot.h lock/fuse Z addresses moved");

// Lock + fuse bytes into `r`, with lockFuseReadable cleared when this chip
// answered with its own flash bytes instead (#518).
//
// boot_lock_fuse_bits_get is `sts SPMCSR,..; lpm` and needs the lpm within 3
// cycles; an ISR in between returns a program byte instead. An EEPROM write in
// progress blocks lock/fuse reads too. Every EEPROM write starts in loop()
// context, so the first wait (interrupts on) is the one that can take
// milliseconds: the periodic refresh must not hold the TWI ISR off that long.
static void readLockAndFuses(BootUpdateReport& r) {
  eeprom_busy_wait();
  noInterrupts();
  eeprom_busy_wait();
  uint8_t lock = boot_lock_fuse_bits_get(GET_LOCK_BITS);
  uint8_t low = boot_lock_fuse_bits_get(GET_LOW_FUSE_BITS);
  uint8_t high = boot_lock_fuse_bits_get(GET_HIGH_FUSE_BITS);
  uint8_t ext = boot_lock_fuse_bits_get(GET_EXTENDED_FUSE_BITS);
  interrupts();
  uint8_t flash0to3[4];
  for (uint8_t i = 0; i < 4; i++) flash0to3[i] = pgm_read_byte(i);
  if (bootLockFuseReadFellThrough(lock, low, high, ext, flash0to3)) {
    r.lockFuseReadable = false;  // the bytes keep their 0xFF placeholders
    return;
  }
  r.lockFuseReadable = true;
  r.lockByte = lock;
  r.fuseLow = low;
  r.fuseHigh = high;
  r.fuseExt = ext;
}

// Rebuild the cached GET_BOOT_INFO reply. Called at boot, after each update
// attempt, and on the BootIntegrity.h timer while the drum is parked — not
// every loop: the CRC32 over 1 KB is ~8 k iterations.
void refreshBootInfoReply() {
  BootSectionFacts f = bootReadFacts();
  BootUpdateReport r;
  readLockAndFuses(r);
  r.bootCrc32 = f.fullCrc32;
  r.state = bootClassify(f);
  r.lastResult = lastBootUpdateResult;
  uint8_t buf[BOOT_INFO_REPLY_LEN];
  bootInfoEncode(r, buf);
  noInterrupts();
  for (uint8_t i = 0; i < BOOT_INFO_REPLY_LEN; i++) bootInfoReplyBuf[i] = buf[i];
  interrupts();
}

// Stage 2 leaves TWI disabled (BootUpdateAvr.h). Bring it back from scratch
// rather than restoring a register snapshot: Wire.begin() re-runs twi_init
// (twi_state READY, TWCR = TWEN|TWIE|TWEA) and keeps the onReceive/onRequest
// hooks; TWGCE is re-armed after it, as in setup(). TWINT is cleared first so a
// stale flag cannot vector the TWI ISR the moment TWIE comes back on.
static void bootUpdateReinitTwi() {
  TWCR = _BV(TWINT);
  Wire.begin(i2cAddress);
  TWAR |= (1 << TWGCE);
}

void runBootUpdate(uint8_t stage) {
  // Refused while the drum is moving or not homed: an update must never race a
  // motor move, and an unhomed unit is in an undefined mechanical state.
  if (!homed || currentlyrotating) {
    lastBootUpdateResult = BOOT_RESULT_REFUSED_BUSY;
    refreshBootInfoReply();
    return;
  }
  // The 8 s loop watchdog is live here; stage 2 holds the CPU ~80 ms.
  wdt_reset();
  // Stage 1, when its gates pass, does not return: the chip resets and the
  // master reads the outcome as state Page7Installed after the reboot.
  BootUpdateReport lockFuses;
  readLockAndFuses(lockFuses);
  // Stage 1 ends in a watchdog reset when its gates pass; if it returns, no
  // reset happened and the marker must not explain a later one.
  if (stage == 1) markResetRequested();
  BootUpdateResult r = bootRunStage(stage, bootEffectiveLockByte(lockFuses));
  if (stage == 1) clearResetRequested();
  // A master that resends stage 2 after timing out in the NACK window gets
  // REFUSED_STATE (state is already New); keep the STAGE2_OK it missed. Masters
  // key on the state, the result only explains it.
  if (!(r == BOOT_RESULT_REFUSED_STATE &&
        lastBootUpdateResult == BOOT_RESULT_STAGE2_OK)) {
    lastBootUpdateResult = r;
  }
  if (!(TWCR & _BV(TWEN))) bootUpdateReinitTwi();  // stage 2 disabled it
  refreshBootInfoReply();
  // No reset on STAGE2_OK: the sketch at 0x0000 is untouched, so the new
  // bootloader only matters on the next reset. The master reads the result via
  // GET_BOOT_INFO, then issues REBOOT when ready.
}

// Called from setup() with the watchdog disabled and before Wire.begin() (TWI
// is not yet initialised, so there is nothing to restore). Publishes NONE when
// there was nothing to resume, which keeps a previous boot's code from
// lingering — the result is not persisted across resets.
void bootUpdateAutoResume() {
  lastBootUpdateResult = bootAutoResume();
}
