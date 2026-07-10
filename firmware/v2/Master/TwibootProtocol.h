#pragma once

#include <stdint.h>

// Twiboot I2C protocol constants — v2 copy of v1's TwibootProtocol.h (the
// v2 copy policy: master-internal pure headers are copies, not shared
// includes; fix bugs in both trees). Slice A (#203) uses only the
// chipinfo probe (bootloader-mode detection); the flash constants stay so
// slice C's reflash client lands against the same header. Protocol
// reference: the command dispatch in firmware/v1/UnitBootloader/main.c.

#define TWIBOOT_CMD_WAIT               0x00  // no-op; resets twiboot's boot-window countdown
#define TWIBOOT_CMD_SWITCH_APPLICATION 0x01  // followed by a boottype byte
#define TWIBOOT_CMD_ACCESS_MEMORY      0x02  // followed by memtype + 2 address bytes

#define TWIBOOT_BOOTTYPE_APPLICATION   0x80  // SWITCH_APPLICATION arg: jump to the sketch

#define TWIBOOT_MEMTYPE_CHIPINFO       0x00
#define TWIBOOT_MEMTYPE_FLASH          0x01

// SPM_PAGESIZE on the ATmega328P; twiboot reads/writes flash in these units.
#define TWIBOOT_PAGE_SIZE              128

// Chipinfo bytes 0..2 are the AVR device signature; the only chip we ever
// flash is the ATmega328P (0x1E 0x95 0x0F).
static inline bool isAtmega328pSignature(uint8_t sig0, uint8_t sig1, uint8_t sig2) {
  return sig0 == 0x1E && sig1 == 0x95 && sig2 == 0x0F;
}
