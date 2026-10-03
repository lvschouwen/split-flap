/* simavr C-API harness for the #499 in-system twiboot update proof.
 *
 * Loads an updater test ELF (the "app", linked at 0x0000), optionally overlays a
 * boot-section binary at 0x7C00 (the real fielded twiboot, since the ELF only
 * carries the app), forces PC straight to the updater entry (the real sketch
 * calls the updater while already running, so we don't exercise twiboot's own
 * boot flow), runs a bounded number of cycles, then compares the resulting boot
 * section against an expected image byte for byte.
 *
 * Usage:
 *   runtest <app.elf> <overlay.bin|none> <main_byte_hex> <cycles> \
 *           <done_sram_hex> <expected_boot.bin>
 *
 * <done_sram_hex> is the raw data-space offset of a volatile byte the app sets
 * to 0xFF when it has finished (NOT the 0x800000 gdb alias). <expected_boot.bin>
 * is the full 1024-byte boot section (0x7C00-0x7FFF) expected after the run.
 *
 * Exit 0 iff the app signalled done AND the whole boot section matches.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <simavr/sim_avr.h>
#include <simavr/sim_elf.h>

#define BOOT_START 0x7c00
#define BOOT_LEN   1024
#define PAGE_SIZE  128

int main(int argc, char** argv) {
  if (argc < 7) {
    fprintf(stderr,
            "usage: %s <app.elf> <overlay.bin|none> <main_byte_hex> <cycles> "
            "<done_sram_hex> <expected_boot.bin>\n",
            argv[0]);
    return 2;
  }
  const char* elf_path = argv[1];
  const char* overlay_path = argv[2];
  uint32_t main_byte = (uint32_t)strtoul(argv[3], NULL, 0);
  uint64_t cycles = strtoull(argv[4], NULL, 0);
  uint32_t done_a = (uint32_t)strtoul(argv[5], NULL, 0);
  const char* expected_path = argv[6];

  elf_firmware_t fw;
  memset(&fw, 0, sizeof(fw));
  if (elf_read_firmware(elf_path, &fw) != 0) {
    fprintf(stderr, "elf_read_firmware failed for %s\n", elf_path);
    return 1;
  }

  avr_t* avr = avr_make_mcu_by_name("atmega328p");
  if (!avr) {
    fprintf(stderr, "avr_make_mcu_by_name failed\n");
    return 1;
  }
  avr_init(avr);
  avr->frequency = 16000000;
  avr_load_firmware(avr, &fw);

  /* Overlay the starting boot section at 0x7C00. simavr's ELF loader ignores a
   * high --section-start and loads .text at 0, so the app's ELF never touches
   * the boot section; we place it here manually. */
  if (strcmp(overlay_path, "none") != 0) {
    FILE* f = fopen(overlay_path, "rb");
    if (!f) {
      fprintf(stderr, "cannot open overlay %s\n", overlay_path);
      return 1;
    }
    uint8_t buf[BOOT_LEN];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    for (size_t i = 0; i < n && (BOOT_START + i) < avr->flashend + 1u; i++) {
      avr->flash[BOOT_START + i] = buf[i];
    }
    fprintf(stderr, "overlaid %zu bytes at 0x%04x; spm@0x7e60=%02x %02x\n", n,
            BOOT_START, avr->flash[0x7e60], avr->flash[0x7e61]);
  }

  /* Load the expected post-run boot section. */
  uint8_t expected[BOOT_LEN];
  memset(expected, 0xFF, sizeof(expected));
  {
    FILE* f = fopen(expected_path, "rb");
    if (!f) {
      fprintf(stderr, "cannot open expected %s\n", expected_path);
      return 1;
    }
    fread(expected, 1, sizeof(expected), f);
    fclose(f);
  }

  avr->pc = main_byte; /* PC in avr_t is a BYTE address. */

  uint64_t ran = 0;
  int state = cpu_Running;
  while (ran < cycles) {
    state = avr_run(avr);
    ran++;
    if (state == cpu_Done || state == cpu_Crashed) break;
    if (avr->data[done_a] == 0xFF) break;
  }

  uint8_t done = avr->data[done_a];
  printf("ran=%llu state=%d pc=0x%04x done=0x%02x\n",
         (unsigned long long)ran, state, avr->pc, done);

  int total_bad = 0, first_bad = -1;
  for (int p = 0; p < BOOT_LEN / PAGE_SIZE; p++) {
    int bad = 0;
    for (int i = 0; i < PAGE_SIZE; i++) {
      int off = p * PAGE_SIZE + i;
      if (avr->flash[BOOT_START + off] != expected[off]) {
        bad++;
        if (first_bad < 0) first_bad = off;
      }
    }
    total_bad += bad;
    printf("  page %d (0x%04x): %d/%d match%s\n", p, BOOT_START + p * PAGE_SIZE,
           PAGE_SIZE - bad, PAGE_SIZE, bad ? "  <-- MISMATCH" : "");
  }
  printf("BOOT SECTION: %d/%d match, mismatches=%d first_bad_off=%d\n",
         BOOT_LEN - total_bad, BOOT_LEN, total_bad, first_bad);

  return (done == 0xFF && total_bad == 0) ? 0 : 3;
}
