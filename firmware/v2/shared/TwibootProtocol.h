#pragma once

#include <stdint.h>

// Twiboot I2C protocol constants. The client that frames them is
// TwibootFlash.h — the only place a twiboot command is built
// (tests/test_twiboot_shared.py). Protocol reference: the command dispatch in
// firmware/v2/UnitBootloader/main.c.

#define TWIBOOT_CMD_WAIT               0x00  // no-op; resets twiboot's boot-window countdown
#define TWIBOOT_CMD_SWITCH_APPLICATION 0x01  // followed by a boottype byte
#define TWIBOOT_CMD_ACCESS_MEMORY      0x02  // followed by memtype + 2 address bytes

#define TWIBOOT_BOOTTYPE_APPLICATION   0x80  // SWITCH_APPLICATION arg: jump to the sketch
// The same first byte with NO argument, followed by a read, returns the
// 16-byte version reply. Like the other two it holds twiboot's countdown.
#define TWIBOOT_CMD_READ_VERSION       TWIBOOT_CMD_SWITCH_APPLICATION
#define TWIBOOT_VERSION_LEN            16
// Version reply of an image with identity bytes (#541): 'S','F', a version
// byte (2 and up), a capability bitfield, 0xFF padding. Images before that
// answer with the stock "TWIBOOT v3.2" string.
#define TWIBOOT_CAP_DO_SPM             0x01  // page 7 carries the do_spm stub
#define TWIBOOT_CAP_BOUNDED_PIN        0x02  // a pinned bootloader times out
#define TWIBOOT_CAP_CRASH_RECORD       0x04  // chipinfo byte 12, see below
#define TWIBOOT_CAP_FUSE_CHIPINFO      0x08  // chipinfo bytes 8..11, see below
// Chipinfo is 8 bytes; an image with TWIBOOT_CAP_FUSE_CHIPINFO serves four
// more (#543): low fuse, lock, extended fuse, high fuse. An image without it
// wraps at 8, so the long read is only ever sent to one that advertises it.
#define TWIBOOT_CHIPINFO_LEN           8
#define TWIBOOT_CHIPINFO_FUSES_LEN     12
// An image with TWIBOOT_CAP_CRASH_RECORD serves one byte more (#542): how many
// watchdog resets in a row its application caused before it had run healthy.
// At TWIBOOT_CRASH_HOLD_COUNT the bootloader stops starting the application
// and waits to be flashed. A master's explicit start command clears the count.
#define TWIBOOT_CHIPINFO_CRASH_LEN     13
#define TWIBOOT_CRASH_HOLD_COUNT       3
// The count reads 1 after ANY reset that entered the bootloader, including
// the intentional one a reflash starts with. Only from here up does it say
// something worth showing.
#define TWIBOOT_CRASH_REPORT_FROM      2

#define TWIBOOT_MEMTYPE_CHIPINFO       0x00
#define TWIBOOT_MEMTYPE_FLASH          0x01

// SPM_PAGESIZE on the ATmega328P; twiboot reads/writes flash in these units.
#define TWIBOOT_PAGE_SIZE              128

// Chipinfo bytes 0..2 are the AVR device signature; the only chip we ever
// flash is the ATmega328P (0x1E 0x95 0x0F).
static inline bool isAtmega328pSignature(uint8_t sig0, uint8_t sig1, uint8_t sig2) {
  return sig0 == 0x1E && sig1 == 0x95 && sig2 == 0x0F;
}

// What a bootloader says about itself (TwibootFlash.h reads it; a row master
// keeps it in the unit's facts while the unit sits in its bootloader).
#define TWIBOOT_GEN_UNREAD      0     // not asked, or the read failed
#define TWIBOOT_GEN_NO_IDENTITY 1     // an image from before the identity bytes
#define TWIBOOT_GEN_UNKNOWN     0xFF  // answered with bytes this build cannot name

struct TwibootIdentity {
  uint8_t generation = TWIBOOT_GEN_UNREAD;  // else the image's version byte (2+)
  uint8_t caps = 0;                         // TWIBOOT_CAP_*
  bool fusesValid = false;                  // the four bytes below are fuses
  uint8_t lfuse = 0;
  uint8_t lock = 0;
  uint8_t efuse = 0;
  uint8_t hfuse = 0;
  bool crashValid = false;                  // crashCount was read
  uint8_t crashCount = 0;
};

// Is the bootloader holding the unit because its application keeps crashing?
// Starting the application again would only repeat it.
inline bool twibootHeldForCrashing(const TwibootIdentity& id) {
  return id.crashValid && id.crashCount >= TWIBOOT_CRASH_HOLD_COUNT;
}
