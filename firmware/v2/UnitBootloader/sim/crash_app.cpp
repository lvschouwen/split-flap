/* #542 crash-recovery proof app: verifies that twiboot's crash counter
 * (sf_crash_count in .noinit SRAM) actually prevents an app start when
 * the threshold is hit, and that the sentinel / POR / invalid-magic paths
 * clear the counter so the app does run. Used by prove.sh R-tests.
 *
 * Writes the sentinel (GPIOR1 = 0xAA) early, exactly as Unit.ino does in
 * setup(), then reports done via sim_report so the harness can detect
 * that twiboot jumped to the app. If twiboot holds (app_installed = 0),
 * the app never runs and sim_report is never written. */
#include <avr/io.h>
#include <avr/wdt.h>
#include <stdint.h>

#define SIM_DONE     0xD0
#define SF_SENTINEL  0xAA

struct SimReport {
  uint8_t done;
  uint8_t result;
  uint8_t state;
};
volatile SimReport sim_report __attribute__((used));

int main(void) {
  TCCR0B = _BV(CS01) | _BV(CS00);
  MCUSR = 0;
  wdt_disable();

  GPIOR1 = SF_SENTINEL;

  sim_report.result = 0;
  sim_report.state = 0;
  sim_report.done = SIM_DONE;

  while (1);
}
