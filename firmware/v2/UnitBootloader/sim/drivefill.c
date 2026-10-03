/* #499 stage-1, robust approach: drive twiboot's OWN erase+fill+write loop.
 * Seed twiboot's buf[] (SRAM 0x011D..0x019C) with the 128 page-7 bytes, set the
 * loop's register inputs, and jmp into the handler just past its boot-section
 * guard (0x7e5a). twiboot erases page 7, fills it from buf[], writes it, then
 * rjmp's to its idle main loop (0x7e12->0x7d02). A relaxed Timer1 IRQ (fires
 * well after the write, while twiboot idles) returns control to us. */
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <stdint.h>
#include "new_image.h"   /* new_twiboot_page7[128] — the real page-7 bytes */

#define ENTRY 0x7e5a
#define BUF   0x011D      /* twiboot's buf[] base in SRAM */
#define PAGE7 0x7F80

volatile uint8_t done = 0;
static void recovery(void) __attribute__((noreturn, noinline, used));

ISR(TIMER1_COMPA_vect, ISR_NAKED) {
  __asm__ __volatile__("pop r0\n\t pop r0\n\t" :::);
  TIMSK1 = 0;
  __asm__ __volatile__("rjmp recovery\n\t" :::);
}
static void recovery(void) { done = 0xFF; for (;;) {} }

int main(void) {
  cli();
  /* Seed buf[] with the REAL page-7 content of the new twiboot image (do_spm +
   * 0xFF pad + ABI marker), not a test pattern. */
  volatile uint8_t* buf = (volatile uint8_t*)BUF;
  for (uint8_t i = 0; i < 128; i++) buf[i] = pgm_read_byte(&new_twiboot_page7[i]);

  /* Relaxed timer: fire after the whole erase+fill+write finishes. In sim the
   * sequence is a few hundred cycles; OCR well past that lands us in twiboot's
   * idle loop. (Hardware tuning is a later detail.) */
  TCCR1A = 0; TCCR1B = 0; TCNT1 = 0;
  OCR1A = 4000;
  TIFR1 = (1 << OCF1A);
  TIMSK1 = (1 << OCIE1A);

  __asm__ __volatile__(
      "clr r1\n\t"
      "ldi r18, 0x03\n\t mov r9, r18\n\t"    /* erase SPMCSR */
      "ldi r18, 0x01\n\t mov r16, r18\n\t"   /* fill  SPMCSR */
      "ldi r18, 0x05\n\t mov r13, r18\n\t"   /* write SPMCSR */
      "ldi r18, 0x11\n\t mov r12, r18\n\t"   /* rww   SPMCSR */
      "ldi r18, 0x9D\n\t mov r14, r18\n\t"   /* fill end lo (0x019D) */
      "ldi r18, 0x01\n\t mov r15, r18\n\t"   /* fill end hi */
      "ldi r24, 0x80\n\t ldi r25, 0x7F\n\t"  /* pagestart = 0x7F80 */
      "ldi r18, %[cs10]\n\t"
      "sei\n\t"
      "sts %[tccr1b], r18\n\t"               /* start Timer1 (/1) */
      "jmp %[entry]\n\t"
      :
      : [cs10] "M"(1 << CS10), [tccr1b] "i"(_SFR_MEM_ADDR(TCCR1B)),
        [entry] "i"(ENTRY)
      : "r1", "r9", "r12", "r13", "r14", "r15", "r16", "r18", "r24", "r25");
  for (;;) {}
}
