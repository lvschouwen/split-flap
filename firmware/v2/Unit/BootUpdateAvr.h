#pragma once

// In-system twiboot update (#499) — the AVR self-program core.
//
// AVR-only (lpm/spm/inline asm, no Arduino or Wire dependency). Compiled by the
// Unit sketch (glue in UnitBootUpdate.ino) AND by the simavr proof app
// (UnitBootloader/sim/updater_app.cpp), so the simulation executes this exact
// source rather than a copy of it. Pure decisions (classifier, lock gate, reply
// codec) live in shared/BootSectionClassify.h + BootUpdateReport.h.
//
// Brick windows (stated once, here): the CPU resets into 0x7C00 (BOOTRST), so
// boot page 0 is on the reset path. Every page-0 erase leaves it blank until
// the matching page write completes, and a reset in that gap executes erased
// flash — unrecoverable without ICSP. Stage 2 has exactly two such gaps (the
// trampoline write, then the final page-0 write), each one erase+write, ~9 ms.
// Every other moment is recoverable: stage 1 only touches page 7 (off every
// execution path), and pages 1-6 are written while page 0 is a jmp-0 trampoline
// that boots the application, which then resumes stage 2 (bootAutoResume).

#include <stdint.h>
#include <avr/eeprom.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>

#include "BootSectionClassify.h"
#include "BootUpdateReport.h"
#include "twiboot-new-progmem.h"

#define BOOT_SECTION_START 0x7C00
#define BOOT_SECTION_LEN   1024
#define BOOT_PAGE_SIZE     128
#define BOOT_PAGE7_OFFSET  (7 * BOOT_PAGE_SIZE)
// A page-0 write that does not verify leaves the reset path corrupt while the
// application is still alive to repair it, so page-0 writes are retried.
#define BOOT_PAGE0_WRITE_ATTEMPTS 3

// do_spm lives at a fixed word address in page 7 after stage 1; AVR function
// pointers are word addresses. ABI (optiboot): r24:r25 addr, r22 action,
// r20:r21 data. Source: UnitBootloader/do_spm.S. Must only be called once
// bootPage7HoldsDoSpm() has confirmed the bytes — calling into a blank or
// foreign page 7 executes garbage.
typedef void (*boot_spm_fn)(uint16_t addr, uint8_t action, uint16_t data);
#define BOOT_DO_SPM ((boot_spm_fn)(NEW_TWIBOOT_DO_SPM_ADDR >> 1))

// Twiboot write-handler entry just past its boot-section guard, and its SRAM
// page buffer — both pinned against the fielded image by
// tests/test_twiboot_entry_points.py. Derivation: UnitBootloader/sim/README.md.
#define TWIBOOT_HANDLER_ENTRY 0x7e5a
#define TWIBOOT_BUF_ADDR      0x011D

// zlib/PNG crc32 (reflected 0xEDB88320) so it matches the Python-computed
// NEW_TWIBOOT_* constants and BootSectionClassify's fielded CRC.
static inline uint32_t bootCrc32Update(uint32_t crc, uint8_t b) {
  crc ^= b;
  for (uint8_t i = 0; i < 8; i++) {
    crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)(-(int32_t)(crc & 1)));
  }
  return crc;
}

// One pass over the boot section yields both CRCs the classifier needs: the
// running value is snapshotted at the pages-0-6 boundary (what keeps a
// half-done stage 1 retriable), then finished for the full section.
static inline BootSectionFacts bootReadFacts() {
  BootSectionFacts f;
  f.pages0_6Crc32 = 0;
  uint32_t crc = 0xFFFFFFFFUL;
  for (uint16_t a = 0; a < BOOT_SECTION_LEN; a++) {
    crc = bootCrc32Update(crc, pgm_read_byte(BOOT_SECTION_START + a));
    if (a == BOOT_PAGE7_OFFSET - 1) f.pages0_6Crc32 = ~crc;
  }
  f.fullCrc32 = ~crc;
  f.page0Word0 = pgm_read_word(BOOT_SECTION_START);
  f.page0Word1 = pgm_read_word(BOOT_SECTION_START + 2);
  return f;
}

static inline BootSectionState bootClassify(const BootSectionFacts& f) {
  return classifyBootSection(f, NEW_TWIBOOT_CRC32,
                             NEW_TWIBOOT_PAGE7_INSTALLED_CRC32);
}

static inline BootSectionState bootCurrentState() {
  return bootClassify(bootReadFacts());
}

// --- read-back checks ---------------------------------------------------------

static inline bool bootPageMatchesImage(uint8_t pageIndex) {
  uint16_t off = (uint16_t)pageIndex * BOOT_PAGE_SIZE;
  for (uint8_t i = 0; i < BOOT_PAGE_SIZE; i++) {
    if (pgm_read_byte(BOOT_SECTION_START + off + i) !=
        pgm_read_byte(&new_twiboot_image[off + i])) {
      return false;
    }
  }
  return true;
}

// Page 7 must be byte-exact before anything calls BOOT_DO_SPM. The Trampoline
// classification does not imply it (that state is keyed on page 0 only).
static inline bool bootPage7HoldsDoSpm() { return bootPageMatchesImage(7); }

static inline bool bootPage0IsTrampoline() {
  return pgm_read_word(BOOT_SECTION_START) == BOOT_TRAMPOLINE_WORD0 &&
         pgm_read_word(BOOT_SECTION_START + 2) == BOOT_TRAMPOLINE_WORD1;
}

// --- stage 1: install do_spm into twiboot's empty page 7 ----------------------
//
// The running sketch cannot SPM (SPM only executes from the boot section) and
// the fielded twiboot has no callable do_spm, so this drives twiboot's OWN
// write handler once: seed its page buffer with the real page-7 bytes, set the
// handler's register inputs, and jump in past its boot-section guard. twiboot
// erases+fills+writes page 7, then drops into its idle loop, and the armed
// watchdog resets the chip. Never returns: twiboot's buffer and globals overlay
// the application's .data/.bss, so the application's RAM is gone by then. The
// outcome is the post-reset boot-section state (Page7Installed), not a result
// code.
static void bootStage1InstallDoSpm() __attribute__((noreturn, unused));
static void bootStage1InstallDoSpm() {
  // Interrupts off BEFORE seeding: buf[] overlays live application RAM (the
  // millis counter, Wire's callback pointers), so any ISR from here on would
  // run on, or write into, the seeded bytes. Off for the whole twiboot drive
  // too — an ISR between twiboot's `sts SPMCSR` and `spm` voids the SPM.
  cli();
  // An EEPROM write in progress blocks SPM (ATmega328P datasheet, "Self-
  // Programming"); the application's EEPROM.write() returns while its ~3.3 ms
  // write is still running. twiboot's handler does not check.
  eeprom_busy_wait();
  // TWI off with its flag cleared (write-one-to-clear TWINT): twiboot's idle
  // loop polls TWINT, and must not run its I2C state machine on traffic that
  // was meant for the application. TWEN=0 also releases the bus.
  TWCR = _BV(TWINT);

  volatile uint8_t* buf = (volatile uint8_t*)TWIBOOT_BUF_ADDR;
  for (uint8_t i = 0; i < BOOT_PAGE_SIZE; i++) {
    buf[i] = pgm_read_byte(&new_twiboot_page7[i]);
  }

  // Fires after the ~9 ms NRWW erase+write (the WDT oscillator runs through the
  // CPU halt); twiboot's idle loop never executes `wdr`. twiboot leaves that
  // loop only two ways: this watchdog reset, or — when the overlaid application
  // byte at its command address 0x019D happens to read 0x21 — an immediate jump
  // to 0x0000. The jump is not a reset (twiboot leaves TWI off, Timer0 stopped,
  // PORTB = 0); the application re-runs its startup, which re-initialises those
  // and disables this watchdog in setup(). Its timeout countdown cannot exit:
  // expiry stores r10 as the command, and r10 is zeroed below. All three cases
  // are exercised by sim/prove.sh (A1-A3).
  wdt_reset();
  wdt_enable(WDTO_250MS);

  // twiboot's write-handler register inputs: SPMCSR constants, the fill-loop
  // end pointer r14:r15 = buf+128 = 0x019D, pagestart r24:r25 = 0x7F80 (all
  // pinned by test_twiboot_entry_points). r10/r11/r17 are not loaded on this
  // entry path but its idle loop uses them (timeout expiry stores r10 as its
  // command, PORTB ^= r11 per Timer0 tick, TWCR = r17 | TWINT): zero keeps the
  // stepper pins still, TWI off, and the timeout from jumping anywhere.
  __asm__ __volatile__(
      "clr r1\n\t"
      "clr r10\n\t"
      "clr r11\n\t"
      "ldi r18, 0x03\n\t mov r9, r18\n\t"    /* erase SPMCSR */
      "ldi r18, 0x01\n\t mov r16, r18\n\t"   /* fill  SPMCSR */
      "ldi r18, 0x05\n\t mov r13, r18\n\t"   /* write SPMCSR */
      "ldi r18, 0x11\n\t mov r12, r18\n\t"   /* rww   SPMCSR */
      "ldi r18, 0x9D\n\t mov r14, r18\n\t"   /* fill end lo (0x019D) */
      "ldi r18, 0x01\n\t mov r15, r18\n\t"   /* fill end hi */
      "ldi r17, 0x00\n\t"                    /* idle-loop TWCR value */
      "ldi r24, 0x80\n\t ldi r25, 0x7F\n\t"  /* pagestart = 0x7F80 */
      "jmp %[entry]\n\t"
      :
      : [entry] "i"(TWIBOOT_HANDLER_ENTRY)
      : "r1", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "r16", "r17",
        "r18", "r24", "r25");
  __builtin_unreachable();
}

// --- stage 2: rewrite pages 0-6 with the new twiboot via do_spm ---------------

static inline void bootWritePageFromImage(uint8_t pageIndex) {
  uint16_t ps = BOOT_SECTION_START + (uint16_t)pageIndex * BOOT_PAGE_SIZE;
  uint16_t off = (uint16_t)pageIndex * BOOT_PAGE_SIZE;
  BOOT_DO_SPM(ps, _BV(PGERS) | _BV(SELFPRGEN), 0);
  for (uint8_t i = 0; i < BOOT_PAGE_SIZE / 2; i++) {
    uint16_t w = (uint16_t)pgm_read_byte(&new_twiboot_image[off + 2 * i]) |
                 ((uint16_t)pgm_read_byte(&new_twiboot_image[off + 2 * i + 1]) << 8);
    BOOT_DO_SPM(ps + 2 * i, _BV(SELFPRGEN), w);
  }
  BOOT_DO_SPM(ps, _BV(PGWRT) | _BV(SELFPRGEN), 0);
  BOOT_DO_SPM(ps, _BV(RWWSRE) | _BV(SELFPRGEN), 0);
}

// Page 0 = `jmp 0x0000` + erased padding. Brick window: erase -> write.
static inline void bootWriteTrampolinePage() {
  uint16_t ps = BOOT_SECTION_START;
  BOOT_DO_SPM(ps, _BV(PGERS) | _BV(SELFPRGEN), 0);
  BOOT_DO_SPM(ps + 0, _BV(SELFPRGEN), BOOT_TRAMPOLINE_WORD0);
  BOOT_DO_SPM(ps + 2, _BV(SELFPRGEN), BOOT_TRAMPOLINE_WORD1);
  for (uint8_t i = 2; i < BOOT_PAGE_SIZE / 2; i++) {
    BOOT_DO_SPM(ps + 2 * i, _BV(SELFPRGEN), 0xFFFF);
  }
  BOOT_DO_SPM(ps, _BV(PGWRT) | _BV(SELFPRGEN), 0);
  BOOT_DO_SPM(ps, _BV(RWWSRE) | _BV(SELFPRGEN), 0);
}

static inline bool bootWriteTrampolineVerified() {
  for (uint8_t i = 0; i < BOOT_PAGE0_WRITE_ATTEMPTS; i++) {
    bootWriteTrampolinePage();
    if (bootPage0IsTrampoline()) return true;
  }
  return false;
}

// Ordering, each step verified by read-back before the next:
//   1. page 7 must already hold do_spm (else nothing is called);
//   2. page 0 -> trampoline, unless it already is one (a resume); retried up
//      to BOOT_PAGE0_WRITE_ATTEMPTS times;
//   3. pages 1-6 -> image, skipping pages that already match (a resume);
//   4. only when pages 1-7 all match: page 0 -> image;
//   5. whole-section CRC must classify New.
// A failed check in 3 stops with page 0 still the trampoline: the unit keeps
// booting its application and resumes on the next boot or command. A failed
// page-0 write in 4 rewrites the trampoline (the only state a reset can boot
// from, same retry) before reporting. A trampoline that never verifies leaves
// page 0 unknown: the next reset is unrecoverable, which retrying is the only
// defence against. Interrupts are off and TWI is disabled throughout
// (TWEN=0: the bus NACKs instead of being clock-stretched for ~80 ms); the
// caller must re-initialise TWI afterwards. Not for an ISR context.
static inline BootUpdateResult bootStage2Rewrite() {
  if (!bootPage7HoldsDoSpm()) return BOOT_RESULT_REFUSED_STATE;
  uint8_t sreg = SREG;
  cli();
  // An EEPROM write in progress silently blocks SPM; one that ends between a
  // page-0 erase and its write would program an unerased page 0.
  eeprom_busy_wait();
  TWCR = _BV(TWINT);
  BootUpdateResult r = BOOT_RESULT_VERIFY_FAILED;
  bool ok = true;
  if (!bootPage0IsTrampoline()) {
    ok = bootWriteTrampolineVerified();
  }
  for (uint8_t pg = 1; ok && pg <= 6; pg++) {
    if (bootPageMatchesImage(pg)) continue;
    bootWritePageFromImage(pg);
    ok = bootPageMatchesImage(pg);
  }
  if (ok) {
    bootWritePageFromImage(0);
    if (bootPageMatchesImage(0)) {
      if (bootCurrentState() == BOOT_STATE_NEW) r = BOOT_RESULT_STAGE2_OK;
    } else {
      bootWriteTrampolineVerified();
    }
  }
  SREG = sreg;
  return r;
}

// --- dispatch -----------------------------------------------------------------

// State + lock gates for a requested stage. Stage 1 never returns when it
// proceeds. The caller adds its own preconditions (drum idle + homed) and owns
// TWI re-initialisation after a stage-2 attempt.
static inline BootUpdateResult bootRunStage(uint8_t stage, uint8_t lockByte) {
  BootSectionState st = bootCurrentState();
  if (stage == 1) {
    if (st != BOOT_STATE_OLD) return BOOT_RESULT_REFUSED_STATE;
    if (!bootLockPermitsBootWrite(lockByte)) return BOOT_RESULT_REFUSED_LOCK;
    bootStage1InstallDoSpm();
  }
  if (stage == 2) {
    if (st != BOOT_STATE_PAGE7_INSTALLED && st != BOOT_STATE_TRAMPOLINE) {
      return BOOT_RESULT_REFUSED_STATE;
    }
    if (!bootLockPermitsBootWrite(lockByte)) return BOOT_RESULT_REFUSED_LOCK;
    return bootStage2Rewrite();
  }
  return BOOT_RESULT_REFUSED_STATE;
}

// A unit that boots classified Trampoline has no working bootloader (page 0 is
// a jmp-0 stub) and must finish stage 2 unprompted. Acts on that state only.
// No lock gate: the trampoline itself proves boot-section writes succeed.
// Returns BOOT_RESULT_NONE when there was nothing to resume.
static inline BootUpdateResult bootAutoResume() {
  if (bootCurrentState() != BOOT_STATE_TRAMPOLINE) return BOOT_RESULT_NONE;
  return bootStage2Rewrite();
}
