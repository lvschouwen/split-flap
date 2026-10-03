/* #499 stage-2 proof: with a callable do_spm already in page 7 (installed by
 * stage 1), the running sketch rewrites twiboot's pages 0-6 with the REAL new
 * twiboot image — plain ret-based calls, no borrow/timer tricks.
 *
 * The harness overlays the post-stage-1 boot section (fielded twiboot pages 0-6
 * + real page 7 = do_spm) and checks the result equals the full new image. */
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <stdint.h>
#include "new_image.h"   /* new_twiboot_image[1024] */

typedef void (*spm_fn)(uint16_t addr, uint8_t action, uint16_t data);
#define DO_SPM ((spm_fn)(0x7F80 >> 1))   /* AVR fn pointers are word addresses */
#define BOOT_START 0x7C00
#define PAGE_SIZE  128

volatile uint8_t done;

int main(void) {
  SP = RAMEND;
  /* Rewrite pages 0-6 (0x7C00..0x7F7F) with the new image. Page 7 (do_spm) is
   * already final from stage 1, so it is left untouched. */
  for (uint8_t pg = 0; pg < 7; pg++) {
    uint16_t pagestart = BOOT_START + (uint16_t)pg * PAGE_SIZE;
    uint16_t off = (uint16_t)pg * PAGE_SIZE;
    DO_SPM(pagestart, (1 << PGERS) | (1 << SELFPRGEN), 0);          /* erase */
    for (uint8_t i = 0; i < PAGE_SIZE / 2; i++) {
      uint16_t w = (uint16_t)pgm_read_byte(&new_twiboot_image[off + 2 * i]) |
                   ((uint16_t)pgm_read_byte(&new_twiboot_image[off + 2 * i + 1]) << 8);
      DO_SPM(pagestart + 2 * i, (1 << SELFPRGEN), w);               /* fill */
    }
    DO_SPM(pagestart, (1 << PGWRT) | (1 << SELFPRGEN), 0);          /* write */
    DO_SPM(pagestart, (1 << RWWSRE) | (1 << SELFPRGEN), 0);         /* rww */
  }
  done = 0xFF;
  for (;;) {}
}
