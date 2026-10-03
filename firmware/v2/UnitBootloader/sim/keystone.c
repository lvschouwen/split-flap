/* Keystone proof for #499 stage 1: can application code borrow twiboot's own
 * `spm` instruction and regain control afterwards?
 *
 * Mechanism: arm Timer1 to raise a compare-match interrupt, set SPMCSR for a
 * page erase with Z = 0x7F80 (the EMPTY page 7, so the erase is harmless), then
 * jmp straight at the `spm` opcode inside twiboot at 0x7e60. The NRWW erase
 * halts the CPU for ~3.7 ms; Timer1 reaches its compare value during the halt,
 * so the instant the erase finishes and the CPU would run twiboot's next
 * instruction, the pending IRQ vectors into our naked ISR instead. The ISR
 * discards the twiboot return address off the stack and jumps to recovery.
 *
 * Success = recovery runs (isr_hit == 0xA5) and page 7 still reads 0xFF. If the
 * borrow failed, control stays in twiboot and recovery never runs.
 *
 * This is harness code, not shipped firmware — it exists to validate the
 * mechanism in simavr before the real updater is written. */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <stdint.h>

#define SPM_SITE_ERASE 0x7e60  /* a `spm` opcode in twiboot (entry-point gate) */
#define PAGE7_ADDR     0x7F80

/* Inspected over gdb in SRAM after the run. */
volatile uint8_t isr_hit __attribute__((used)) = 0x00;
volatile uint8_t page7_sample[4] __attribute__((used)) = {0, 0, 0, 0};
volatile uint8_t done __attribute__((used)) = 0x00;

static void recovery(void) __attribute__((noreturn, noinline, used));

/* Naked ISR: on entry the hardware has pushed the twiboot return address and
 * cleared the global interrupt flag. Drop that return address (2 bytes on the
 * 328P) so we never fall back into twiboot, disable the timer IRQ, and jump to
 * recovery. */
ISR(TIMER1_COMPA_vect, ISR_NAKED) {
  __asm__ __volatile__(
      "pop r0\n\t"   /* discard pushed PC high */
      "pop r0\n\t"   /* discard pushed PC low  */
      :::);
  TIMSK1 = 0;        /* no re-fire */
  recovery();
}

static void recovery(void) {
  isr_hit = 0xA5;
  for (uint8_t i = 0; i < 4; i++) {
    page7_sample[i] = pgm_read_byte(PAGE7_ADDR + i);
  }
  done = 0xFF;
  for (;;) {
  }
}

int main(void) {
  cli();

  /* Timer1, prescaler 1, compare match ~200 cycles out: far past the few
   * setup cycles below, far inside the ~59000-cycle erase halt. */
  TCCR1A = 0;
  TCCR1B = 0;
  TCNT1 = 0;
  OCR1A = 200;
  TIFR1 = (1 << OCF1A);     /* clear any stale flag */
  TIMSK1 = (1 << OCIE1A);   /* enable compare-match A IRQ */

  register uint8_t spmval __asm__("r18") = (1 << PGERS) | (1 << SELFPRGEN);

  __asm__ __volatile__(
      "ldi r30, lo8(%[pg])\n\t"   /* Z = page 7 erase target */
      "ldi r31, hi8(%[pg])\n\t"
      "ldi r24, %[cs10]\n\t"
      "sei\n\t"
      "sts %[tccr1b], r24\n\t"     /* start Timer1 (CS10 = /1) */
      "out %[spmcsr], %[val]\n\t"  /* arm SPM; 4-cycle window opens */
      "jmp %[site]\n\t"            /* run twiboot's spm at the borrow site */
      :
      : [pg] "i"(PAGE7_ADDR),
        [cs10] "M"(1 << CS10),
        [tccr1b] "i"(_SFR_MEM_ADDR(TCCR1B)),
        [spmcsr] "I"(_SFR_IO_ADDR(SPMCSR)),
        [val] "r"(spmval),
        [site] "i"(SPM_SITE_ERASE)
      : "r24", "r30", "r31");

  /* Never reached: the erase halts here, then the IRQ diverts to recovery. */
  for (;;) {
  }
}
