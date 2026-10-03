/* #499 simavr proof app: runs the unit firmware's own self-program core
 * (firmware/v2/Unit/BootUpdateAvr.h, included unchanged) in the order the
 * sketch uses it, with this app standing in for both the sketch's boot path
 * and the master's commands:
 *
 *   boot -> bootAutoResume()            (setup(): finish a half-done stage 2)
 *        -> state Old            -> stage 1 (never returns; WDT reset)
 *        -> state Page7Installed -> stage 2
 *        -> publish result + state in sim_report, spin.
 *
 * The harness (runtest.c) power-cycles the simulated chip around this to prove
 * the reset / resume behaviour. Lock bits are not modelled by simavr, so the
 * lock gate is fed an unlocked byte. */
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <stdint.h>

#include "BootUpdateAvr.h"

#define SIM_DONE 0xD0

struct SimReport {
  uint8_t done;    // SIM_DONE once this boot's work is finished
  uint8_t result;  // BootUpdateResult of the last action this boot (0 = none)
  uint8_t state;   // BootSectionState after it
};
volatile SimReport sim_report __attribute__((used));

int main(void) {
  /* As on the unit: Arduino's init() leaves Timer0 running at clk/64 (the
   * millis tick; twiboot's idle loop polls its overflow flag to count down its
   * timeout) and interrupts on; setup() then consumes the reset cause and
   * disables the watchdog. The millis ISR itself is not needed here. */
  TCCR0B = _BV(CS01) | _BV(CS00);
  MCUSR = 0;
  wdt_disable();
  sei();

  uint8_t r = bootAutoResume();
  BootSectionState st = bootCurrentState();
  if (st == BOOT_STATE_OLD) {
    r = bootRunStage(1, 0xFF);
  } else if (st == BOOT_STATE_PAGE7_INSTALLED) {
    r = bootRunStage(2, 0xFF);
  }
  sim_report.result = r;
  sim_report.state = bootCurrentState();
  sim_report.done = SIM_DONE;
  for (;;) {}
}
