/* simavr C-API harness for the #499 stage-1 keystone.
 *
 * Loads the keystone ELF (which also pulls in the fielded twiboot via the
 * merged image we build separately), forces PC straight to `main` (the real
 * sketch calls the updater while already running, so we don't exercise
 * twiboot's boot flow), runs a bounded number of cycles, then reads flash and
 * SRAM directly — no gdb stub, no breakpoints.
 *
 * Usage: runtest <firmware.elf> <main_byte_addr_hex> <run_cycles>
 * Reports: whether `done`/`isr_hit` got set, and the first bytes of page 7.
 *
 * Addresses of done/isr_hit/page7_sample are read from the ELF symbol table by
 * the caller and passed via a tiny sidecar? No — simpler: we hardcode nothing;
 * we scan the loaded firmware's symbols via libsimavr's elf loader if exposed,
 * else the caller passes the SRAM addresses. We take them as argv to stay
 * decoupled from the build. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <simavr/sim_avr.h>
#include <simavr/sim_elf.h>
#include <simavr/avr_ioport.h>

int main(int argc, char** argv) {
  if (argc < 8) {
    fprintf(stderr,
            "usage: %s <elf> <twiboot.bin> <main_byte_hex> <cycles> "
            "<done_sram_hex> <isr_hit_sram_hex> <page7_sram_hex>\n",
            argv[0]);
    return 2;
  }
  const char* elf_path = argv[1];
  const char* twiboot_path = argv[2];
  uint32_t main_byte = (uint32_t)strtoul(argv[3], NULL, 0);
  uint64_t cycles = strtoull(argv[4], NULL, 0);
  /* SRAM data addresses are given as the raw data-space offset (e.g. 0x100),
   * NOT the 0x800000 gdb alias — avr->data[] is indexed directly. */
  uint32_t done_a = (uint32_t)strtoul(argv[5], NULL, 0);
  uint32_t isr_a = (uint32_t)strtoul(argv[6], NULL, 0);
  uint32_t p7_a = (uint32_t)strtoul(argv[7], NULL, 0);

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

  /* Overlay the real fielded twiboot into the boot section (0x7C00), so the
   * borrowed spm sites exist in flash. The ELF only carries the app. */
  {
    if (strcmp(twiboot_path,"none")==0) goto skip_overlay; FILE* f = fopen(twiboot_path, "rb");
    if (!f) {
      fprintf(stderr, "cannot open twiboot bin %s\n", twiboot_path);
      return 1;
    }
    uint8_t buf[1024];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    for (size_t i = 0; i < n && (0x7c00 + i) < avr->flashend + 1u; i++) {
      avr->flash[0x7c00 + i] = buf[i];
    }
    skip_overlay:; fprintf(stderr, "overlaid %zu twiboot bytes at 0x7c00; spm@0x7e60=%02x %02x\n",
            n, avr->flash[0x7e60], avr->flash[0x7e61]);
  }

  /* Jump straight into the application. PC in avr_t is a BYTE address. */
  avr->pc = main_byte;

  uint64_t ran = 0;
  int state = cpu_Running;
  while (ran < cycles) {
    state = avr_run(avr);
    ran++;
    if (state == cpu_Done || state == cpu_Crashed) break;
    /* stop early once recovery has signalled completion */
    if (avr->data[done_a] == 0xFF) break;
  }

  uint8_t done = avr->data[done_a];
  uint8_t isr_hit = avr->data[isr_a];
  printf("ran=%llu state=%d pc=0x%04x done=0x%02x isr_hit=0x%02x\n",
         (unsigned long long)ran, state, avr->pc, done, isr_hit);
  printf("page7_sample=%02x %02x %02x %02x\n", avr->data[p7_a],
         avr->data[p7_a + 1], avr->data[p7_a + 2], avr->data[p7_a + 3]);
  /* Also read the live flash at page 7 directly from the sim. */
  printf("flash[0x7f80..]=%02x %02x %02x %02x\n", avr->flash[0x7f80],
         avr->flash[0x7f81], avr->flash[0x7f82], avr->flash[0x7f83]);

  {
    int bad = 0, first_bad = -1;
    for (int i = 0; i < 128; i++) {
      if (avr->flash[0x7f80 + i] != (uint8_t)i) { bad++; if (first_bad<0) first_bad=i; }
    }
    printf("PAGE7 FULL: %d/128 match, mismatches=%d first_bad=%d\n", 128-bad, bad, first_bad);
  }
  {
    int bad=0, fb=-1;
    for (int i=0;i<128;i++){ if (avr->flash[0x7f00+i]!=(uint8_t)(0xC0+i)){bad++; if(fb<0)fb=i;} }
    printf("PAGE6: %d/128 match, mismatches=%d first_bad=%d (b0=%02x b1=%02x b127=%02x)\n",
           128-bad,bad,fb, avr->flash[0x7f00], avr->flash[0x7f01], avr->flash[0x7f7f]);
  }
  return (done == 0xFF) ? 0 : 3;
}
