/* Minimal crash app for the #542 simavr proof.
 *
 * Built with avr-libc startup code and --defsym=__stack=0x8FD so the stack
 * stops below the crash record at 0x08FE/0x08FF — the same linker symbol the
 * real unit firmware uses.  A .bss region large enough to prove it never
 * reaches the crash record, plus a deliberate WDT timeout to exercise the
 * crash record mechanism.
 *
 * The harness reads sim_report.done and sim_report.boots to detect when the
 * app has started and how many times it has booted.
 */

#include <avr/io.h>
#include <avr/wdt.h>
#include <stdint.h>

/* Report structure the harness reads from SRAM. */
struct {
    volatile uint8_t done;     /* 0xD0 = "app started" */
    volatile uint8_t boots;    /* incremented each boot */
    volatile uint8_t mode;     /* 0 = crash (let WDT fire), 1 = stay alive */
} sim_report __attribute__((used));

/* Push BSS close to the real unit app's size (~450 bytes) to prove the crash
 * record at 0x08FE-0x08FF is never overwritten by startup code. */
volatile uint8_t bss_padding[400] __attribute__((used));

int main(void) {
    sim_report.boots++;
    sim_report.done = 0xD0;

    if (sim_report.mode == 0) {
        wdt_enable(WDTO_15MS);
        for (;;) {}
    }

    /* mode == 1: stay alive (don't crash). */
    for (;;) {}
}
