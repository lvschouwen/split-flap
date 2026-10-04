#pragma once

#include <stdint.h>

// Boot-section state for the in-system twiboot update (#499). Classifies the
// unit's own boot section (0x7C00-0x7FFF) from a few cheaply-computed facts so
// the updater can gate each stage and auto-resume an interrupted stage 2.
//
// Pure logic, natively tested (test_boot_classify). The unit computes the facts
// by reading its own boot section (lpm / twiboot's unbounded flash read) and
// CRC32-ing it; this header only decides what those facts mean. The two image
// CRCs are passed in from the generated new-image header
// (make_new_twiboot.py → NEW_TWIBOOT_CRC32 / NEW_TWIBOOT_PAGE7_INSTALLED_CRC32)
// so the classifier and the image it is built against can never drift apart.

enum BootSectionState {
  BOOT_STATE_UNKNOWN = 0,      // unrecognized image — refuse every update
  BOOT_STATE_OLD,              // fielded twiboot (empty page 7), ready for stage 1
  BOOT_STATE_PAGE7_INSTALLED,  // fielded pages 0-6 + do_spm in page 7 (post stage 1)
  BOOT_STATE_TRAMPOLINE,       // page 0 is a jmp-to-app trampoline (mid stage 2)
  BOOT_STATE_NEW,              // new twiboot fully installed
};

// crc32 (zlib / reflected 0xEDB88320) of the fielded image over 0x7C00-0x7FFF.
// A fact of the deployed fleet (#511), pinned by tests/test_twiboot_entry_points.
#define BOOT_FIELDED_CRC32 0x18173addUL
// crc32 of the current twiboot image over the same range — what a row master
// expects every unit to carry (BootIntegrity.h). The generated image header's
// NEW_TWIBOOT_CRC32 is the source; the unit build asserts the two equal and
// tests/test_new_twiboot_image pins it for the trees that never see that header.
#define BOOT_CURRENT_CRC32 0xe422a668UL
// crc32 of just the fielded pages 0-6 (0x7C00-0x7F7F, the twiboot core, page 7
// excluded). Stage 1 only writes page 7, so this is what survives a half-done or
// retried stage 1 — see the retriable case in classifyBootSection. Also pinned
// by tests/test_twiboot_entry_points.
#define BOOT_FIELDED_PAGES_0_6_CRC32 0x2524e944UL

// The trampoline the updater writes at 0x7C00 during stage 2 is AVR `jmp 0x0000`
// — a 2-word instruction, opcode 0x940C then target 0x0000 (little-endian in
// flash: 0C 94 00 00). On reset the CPU enters the boot section at 0x7C00 and
// this jumps straight to the application, which then resumes stage 2.
#define BOOT_TRAMPOLINE_WORD0 0x940CU
#define BOOT_TRAMPOLINE_WORD1 0x0000U

struct BootSectionFacts {
  uint32_t fullCrc32;      // crc32 over the whole boot section (1024 B)
  uint32_t pages0_6Crc32;  // crc32 over pages 0-6 only (0x7C00-0x7F7F, 896 B)
  uint16_t page0Word0;     // instruction word at 0x7C00 (little-endian)
  uint16_t page0Word1;     // instruction word at 0x7C02 (little-endian)
};

// Clean, CRC-identified images (New / Old / Page7Installed) are terminal and win
// over the trampoline marker — a fully written image is never mid-stage-2, and
// the CRC is a far stronger identity than a single instruction word. Only when
// the CRC matches no known image does the page-0 jmp-to-app mark a half-done
// stage 2 that must be resumed. Anything else is Unknown and refuses updates.
inline BootSectionState classifyBootSection(const BootSectionFacts& f,
                                            uint32_t newFullCrc32,
                                            uint32_t page7InstalledCrc32) {
  if (f.fullCrc32 == newFullCrc32) return BOOT_STATE_NEW;
  if (f.fullCrc32 == BOOT_FIELDED_CRC32) return BOOT_STATE_OLD;
  if (f.fullCrc32 == page7InstalledCrc32) return BOOT_STATE_PAGE7_INSTALLED;
  if (f.page0Word0 == BOOT_TRAMPOLINE_WORD0 &&
      f.page0Word1 == BOOT_TRAMPOLINE_WORD1) {
    return BOOT_STATE_TRAMPOLINE;
  }
  // Fielded twiboot core (pages 0-6) intact but page 7 is neither blank (Old)
  // nor the installed do_spm (Page7Installed) — i.e. a half-done or interrupted
  // stage 1. Report Old so stage 1 can be retried: it only touches page 7, and
  // twiboot's handler erases page 7 before rewriting it, so this is safe and is
  // what keeps a failed stage 1 off the ICSP-recovery path (never a brick, since
  // page 7 is off every execution path). Stage 2 rewrites pages 0-6, so any
  // post-stage-2 state has a different pages-0-6 CRC and never lands here.
  if (f.pages0_6Crc32 == BOOT_FIELDED_PAGES_0_6_CRC32) {
    return BOOT_STATE_OLD;
  }
  return BOOT_STATE_UNKNOWN;
}

// SPM can only write the boot section when boot-lock bit BLB11 is unprogrammed
// (reads as 1). The provisioning chip-erase leaves the lock byte 0xFF (no
// locks), so fielded units pass; a unit someone locked is refused rather than
// bricked by an SPM that silently no-ops. BLB11 is bit 4 of the ATmega328P lock
// byte (value 0x10) — hardcoded here so the pure logic has no AVR-header dep.
#define BOOT_LOCK_BLB11_MASK 0x10
inline bool bootLockPermitsBootWrite(uint8_t lockByte) {
  return (lockByte & BOOT_LOCK_BLB11_MASK) != 0;
}
