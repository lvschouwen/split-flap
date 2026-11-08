/* simavr proof for #502: a unit whose hall sensor never fires.
 *
 * Runs the real unit image (under the units' bootloader) with the hall line
 * held high (no magnet, ever) and no master on the bus, and records every
 * change of the stepper coil outputs (PB0..PB3). It must hold:
 *
 *   - the unit seeks by itself after its standalone deadline (~30 s)
 *   - each seek gives up after three revolutions, leaves `homed` false and
 *     the displayed letter untouched (no flap move after a failed seek)
 *   - between seeks the coils do not change (no blind stepping)
 *   - the gap from the end of one seek to the start of the next is
 *     30 s, 60 s, 120 s, 240 s, ... capped at 16 min
 *
 * Usage:  home_fail_test <unit.bin> <twiboot.bin> <homed addr> <homeBackoff addr>
 *         (data addresses from avr-nm, 0x800000 offset stripped or not)
 * Exit 0 iff all checks pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <simavr/sim_avr.h>
#include <simavr/sim_irq.h>
#include <simavr/avr_ioport.h>

#define BOOT_START     0x7C00
#define BOOT_LEN       1024
#define CYCLES_PER_MS  16000ULL
#define PORTB_ADDR     0x25
#define COILS          0x0F
#define HALL_PIN       7        /* PD7 */
#define SEEK_GAP_MS    1000.0   /* coil silence that ends a seek */
#define MAX_SEEKS      8
#define STEPS_PER_REV  2038

static uint8_t boot_image[BOOT_LEN];
static uint8_t app_image[BOOT_START];

static size_t read_file(const char *path, uint8_t *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

/* simavr sleeps the host in real time while the AVR sleeps; the unit idles in
 * SLEEP_MODE_IDLE, so run its sleep at simulation speed instead. */
static void no_host_sleep(avr_t *avr, avr_cycle_count_t how_long) { (void)avr; (void)how_long; }

struct seek { double start, end; long changes; int homed_after; uint8_t fails_after; };

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <unit.bin> <twiboot.bin> <homed addr> <homeBackoff addr>\n", argv[0]);
        return 2;
    }
    memset(app_image, 0xFF, sizeof(app_image));
    if (read_file(argv[1], app_image, sizeof(app_image)) < 256) return 2;
    memset(boot_image, 0xFF, BOOT_LEN);
    if (read_file(argv[2], boot_image, BOOT_LEN) < 896) return 2;
    uint32_t homed_addr = strtoul(argv[3], NULL, 16) & 0xFFFF;
    uint32_t backoff_addr = strtoul(argv[4], NULL, 16) & 0xFFFF;

    avr_t *avr = avr_make_mcu_by_name("atmega328p");
    avr_init(avr);
    avr->frequency = 16000000;
    avr->vcc = avr->avcc = avr->aref = 5000;
    avr->log = 0;
    avr->sleep = no_host_sleep;
    memcpy(avr->flash, app_image, BOOT_START);
    memcpy(&avr->flash[BOOT_START], boot_image, BOOT_LEN);
    avr->reset_pc = BOOT_START;
    avr_reset(avr);
    avr->pc = BOOT_START;

    /* DIP switches open (address index 0), hall high: no magnet. */
    for (int pin = 3; pin <= 6; pin++)
        avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('D'), pin), 1);
    avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('D'), HALL_PIN), 1);

    struct seek seeks[MAX_SEEKS];
    int n = 0;
    int in_seek = 0;
    double last_change = 0;
    uint8_t coils = avr->data[PORTB_ADDR] & COILS;
    /* the waits 30,60,120,240,480,960,960 s plus eight seeks fit in 55 min */
    const double end_ms = 55 * 60 * 1000.0;
    double app_start = -1;

    while (n < MAX_SEEKS) {
        int st = avr_run(avr);
        if (st == cpu_Done || st == cpu_Crashed) { printf("cpu stopped (%d)\n", st); return 1; }
        double t = (double)avr->cycle / CYCLES_PER_MS;
        if (app_start < 0 && avr->pc < BOOT_START) app_start = t;
        uint8_t c = avr->data[PORTB_ADDR] & COILS;
        if (c != coils) {
            coils = c;
            if (!in_seek) {
                in_seek = 1;
                seeks[n].start = t;
                seeks[n].changes = 0;
            }
            seeks[n].changes++;
            last_change = t;
        } else if (in_seek && t - last_change > SEEK_GAP_MS) {
            in_seek = 0;
            seeks[n].end = last_change;
            seeks[n].homed_after = avr->data[homed_addr];
            seeks[n].fails_after = avr->data[backoff_addr];
            printf("  seek ended at %.1f s\n", t / 1000);
            fflush(stdout);
            n++;
        }
        if (t > end_ms) break;
    }

    int pass = 1;
    printf("%d seeks in %.0f s of unit time\n", n, (double)avr->cycle / CYCLES_PER_MS / 1000);
    if (n < MAX_SEEKS) { printf("FAIL: expected %d seeks\n", MAX_SEEKS); pass = 0; }
    /* standalone deadline: 30 s after the program starts, + DIP 0 * 600 ms,
     * + up to 250 ms jitter */
    double first = n ? seeks[0].start - app_start : 0;
    printf("program started at %.0f ms; first seek %.0f ms later\n", app_start, first);
    if (n && (first < 30000 || first > 30300)) {
        printf("FAIL: first seek not at the standalone deadline\n"); pass = 0;
    }
    for (int i = 0; i < n; i++) {
        double dur = (seeks[i].end - seeks[i].start) / 1000;
        /* the Stepper library writes the four coil pins one at a time, so
         * each step shows as two pattern changes */
        double revs = (double)seeks[i].changes / 2 / STEPS_PER_REV;
        printf("seek %d: %8.1f s .. %8.1f s (%5.1f s, %ld coil changes = %.2f rev)  homed=%d fails=%u",
               i + 1, seeks[i].start / 1000, seeks[i].end / 1000, dur, seeks[i].changes, revs,
               seeks[i].homed_after, seeks[i].fails_after);
        if (seeks[i].homed_after) { printf("  FAIL homed\n"); pass = 0; continue; }
        if (seeks[i].fails_after != i + 1) { printf("  FAIL count\n"); pass = 0; continue; }
        if (revs < 2.9 || revs > 3.2) { printf("  FAIL revs\n"); pass = 0; continue; }
        if (i > 0) {
            double gap = seeks[i].start - seeks[i - 1].end;
            double want = 30000.0 * (1u << (i - 1 > 5 ? 5 : i - 1));
            int ok = gap > want && gap < want + 1000;
            printf("  gap %.1f s (want %.0f s)%s", gap / 1000, want / 1000, ok ? "" : "  FAIL gap");
            if (!ok) pass = 0;
        }
        printf("\n");
    }
    printf(pass ? "HOME FAIL PROOF PASSED\n" : "HOME FAIL PROOF FAILED\n");
    return pass ? 0 : 1;
}
