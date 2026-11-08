/* simavr proof for #502: a unit clamps the speed byte to 12 RPM.
 *
 * Runs the real unit image (under the units' bootloader), plays a master
 * that sends one letter command with speed byte 255, and times every step
 * on the stepper coil outputs (PB0..PB3). The hall sensor is modelled from
 * the steps taken: low over a 30-step window once per 2038-step revolution.
 *
 * The unit is unhomed, so the letter homes it first (fixed HOMING_RPM 10,
 * 2.944 ms per step) and then moves the flaps at the commanded speed. It
 * must hold: `stepperSpeed` reads 12 after the command, and no stretch of
 * steps runs faster than 12 RPM (2.453 ms per step).
 *
 * The master side is played at the TWI register level, as Wire's ISR sees
 * it: SLA+W (0x60), two data bytes (0x80), STOP (0xA0), each with TWINT
 * raised and the TWI interrupt pending. simavr 1.6's own slave side is not
 * used (see pin_test.c).
 *
 * Usage:  speed_clamp_test <unit.bin> <twiboot.bin> <stepperSpeed addr>
 * Exit 0 iff all checks pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <simavr/sim_avr.h>
#include <simavr/sim_irq.h>
#include <simavr/sim_interrupts.h>
#include <simavr/avr_ioport.h>
#include <simavr/avr_twi.h>

#define BOOT_START     0x7C00
#define BOOT_LEN       1024
#define CYCLES_PER_MS  16000ULL
#define PORTB_ADDR     0x25
#define COILS          0x0F
#define HALL_PIN       7
#define TWSR_ADDR      0xB9
#define TWDR_ADDR      0xBB
#define TWCR_ADDR      0xBC
#define TWINT          0x80
#define STEPS_PER_REV  2038
#define HALL_WINDOW    30
#define LETTER         10
#define SPEED_BYTE     255
#define MS_PER_STEP_12 (60000.0 / (12 * STEPS_PER_REV))   /* 2.453 */
#define MS_PER_STEP_10 (60000.0 / (10 * STEPS_PER_REV))   /* 2.944 */

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

static void no_host_sleep(avr_t *avr, avr_cycle_count_t how_long) { (void)avr; (void)how_long; }

static avr_io_write_t twcr_orig;
static void *twcr_orig_param;
static void twcr_write(avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
    (void)param;
    twcr_orig(avr, addr, v, twcr_orig_param);
    if (v & TWINT) avr->data[TWCR_ADDR] &= (uint8_t)~TWINT;
}

static avr_t *avr;
static avr_twi_t *twi;
static avr_irq_t *hall;
static uint8_t coils;
static long changes;
static int drum = 1000;          /* steps past the magnet at power-on */
static double last_step_ms = -1;
static double intervals[40000];
static int n_intervals;

static double now_ms(void) { return (double)avr->cycle / CYCLES_PER_MS; }

/* One instruction, with the coil watch and the hall model. */
static void tick(void)
{
    int st = avr_run(avr);
    if (st == cpu_Done || st == cpu_Crashed) { printf("cpu stopped (%d)\n", st); exit(1); }
    uint8_t c = avr->data[PORTB_ADDR] & COILS;
    if (c == coils) return;
    coils = c;
    /* Stepper writes the four pins one at a time: two pattern changes per
     * step, microseconds apart. A step is a change after a quiet spell. */
    double t = now_ms();
    if (last_step_ms < 0 || t - last_step_ms > 0.5) {
        if (last_step_ms >= 0 && n_intervals < 40000)
            intervals[n_intervals++] = t - last_step_ms;
        last_step_ms = t;
        drum = (drum + 1) % STEPS_PER_REV;
        avr_raise_irq(hall, drum < HALL_WINDOW ? 0 : 1);
    }
    changes++;
}

static int hw_step(uint8_t status, uint8_t data)
{
    avr->data[TWDR_ADDR] = data;
    avr->data[TWSR_ADDR] = status;
    avr_raise_interrupt(avr, &twi->twi);
    double deadline = now_ms() + 50;
    while (now_ms() < deadline) {
        tick();
        if (!(avr->data[TWCR_ADDR] & TWINT)) return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <unit.bin> <twiboot.bin> <stepperSpeed addr>\n", argv[0]);
        return 2;
    }
    memset(app_image, 0xFF, sizeof(app_image));
    if (read_file(argv[1], app_image, sizeof(app_image)) < 256) return 2;
    memset(boot_image, 0xFF, BOOT_LEN);
    if (read_file(argv[2], boot_image, BOOT_LEN) < 896) return 2;
    uint32_t speed_addr = strtoul(argv[3], NULL, 16) & 0xFFFF;

    avr = avr_make_mcu_by_name("atmega328p");
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

    for (avr_io_t *io = avr->io_port; io; io = io->next)
        if (io->kind && !strcmp(io->kind, "twi")) twi = (avr_twi_t *)io;
    if (!twi) { fprintf(stderr, "no TWI peripheral\n"); return 2; }
    int io = AVR_DATA_TO_IO(TWCR_ADDR);
    twcr_orig = avr->io[io].w.c;
    twcr_orig_param = avr->io[io].w.param;
    avr->io[io].w.c = twcr_write;

    for (int pin = 3; pin <= 6; pin++)
        avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('D'), pin), 1);
    hall = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('D'), HALL_PIN);
    avr_raise_irq(hall, 1);
    coils = avr->data[PORTB_ADDR] & COILS;

    while (now_ms() < 3000) tick();                 /* program up, unhomed */
    int ok_bus = hw_step(0x60, 0) && hw_step(0x80, LETTER) &&
                 hw_step(0x80, SPEED_BYTE) && hw_step(0xA0, 0);
    if (!ok_bus) { printf("FAIL: the unit did not take the command\n"); return 1; }
    printf("sent letter %d with speed byte %d at %.0f ms\n", LETTER, SPEED_BYTE, now_ms());

    /* run until the drum has been still for 3 s after moving */
    while (now_ms() < 120000) {
        tick();
        if (last_step_ms > 0 && now_ms() - last_step_ms > 3000) break;
    }
    int speed = avr->data[speed_addr] | (avr->data[speed_addr + 1] << 8);
    printf("stepperSpeed after the command: %d\n", speed);

    /* intervals inside a move only (gaps over 20 ms are pauses) */
    double min = 1e9;
    int at10 = 0, at12 = 0, other = 0, moving = 0;
    for (int i = 0; i < n_intervals; i++) {
        double d = intervals[i];
        if (d > 20) continue;
        moving++;
        if (d < min) min = d;
        if (d > MS_PER_STEP_10 - 0.05 && d < MS_PER_STEP_10 + 0.05) at10++;
        else if (d > MS_PER_STEP_12 - 0.05 && d < MS_PER_STEP_12 + 0.05) at12++;
        else other++;
    }
    printf("%d steps timed: %d at 10 RPM (%.3f ms), %d at 12 RPM (%.3f ms), %d other\n",
           moving, at10, MS_PER_STEP_10, at12, MS_PER_STEP_12, other);
    printf("shortest step %.3f ms = %.1f RPM\n", min, 60000.0 / (min * STEPS_PER_REV));

    int pass = speed == 12 && at12 >= LETTER * STEPS_PER_REV / 45 - 2 &&
               min > MS_PER_STEP_12 - 0.05;
    printf(pass ? "SPEED CLAMP PROOF PASSED\n" : "SPEED CLAMP PROOF FAILED\n");
    return pass ? 0 : 1;
}
