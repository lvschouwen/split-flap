/* simavr proof for #502 / #499: the bootloader's pinned state is bounded.
 *
 * A master pins a unit in twiboot by writing a command to it in the
 * bootloader's first second; each command restarts a 30 s window
 * (SF_PIN_TIMEOUT_MS). A master that then goes silent must not leave the
 * unit in the bootloader for ever: it starts its program when the window
 * runs out.
 *
 *   (A) power-on, no master            → program starts after ~1 s
 *   (B) one CMD_WAIT at 200 ms, silence → still held at 25 s, program
 *                                          starts 30 s after the command
 *   (C) CMD_WAIT every 10 s for 60 s    → held throughout, program starts
 *                                          30 s after the last command
 *
 * The master side is played at the TWI register level (see hw_step).
 *
 * Usage:  pin_test <unit_app.bin> <twiboot.bin>
 * Exit 0 iff all checks pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <simavr/sim_avr.h>

#define BOOT_START     0x7C00
#define BOOT_LEN       1024
#define CYCLES_PER_MS  16000ULL
#define TWCR_ADDR      0xBC
#define TWAR_ADDR      0xBA
#define TWINT          0x80
#define CMD_WAIT       0x00
#define PIN_MS         30000.0
#define TOLERANCE_MS   600.0   /* window is counted in 128 ms prescaled ticks */

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

static avr_t *make_unit(void)
{
    avr_t *avr = avr_make_mcu_by_name("atmega328p");
    if (!avr) { fprintf(stderr, "avr_make_mcu_by_name failed\n"); exit(2); }
    avr_init(avr);
    avr->frequency = 16000000;
    avr->log = 0;
    memcpy(avr->flash, app_image, BOOT_START);
    memcpy(&avr->flash[BOOT_START], boot_image, BOOT_LEN);
    avr->reset_pc = BOOT_START;       /* BOOTRST fuse */
    avr_reset(avr);
    avr->pc = BOOT_START;
    return avr;
}

static double now_ms(avr_t *avr) { return (double)avr->cycle / CYCLES_PER_MS; }
static int in_boot(avr_t *avr) { return avr->pc >= BOOT_START; }

/* Runs to an absolute time; returns 1 if the program started (pc left the
 * boot section) before it, with *left_ms set to the instant. */
static int run_to(avr_t *avr, double until_ms, double *left_ms)
{
    while (now_ms(avr) < until_ms) {
        int st = avr_run(avr);
        if (st == cpu_Done || st == cpu_Crashed) {
            printf("  cpu stopped (%d)\n", st); exit(1);
        }
        if (!in_boot(avr)) { if (left_ms) *left_ms = now_ms(avr); return 1; }
    }
    return 0;
}

/* The harness is the TWI hardware of a slave being written to: for each bus
 * event it sets TWSR (and TWDR), raises TWINT and waits for the bootloader to
 * clear it by writing 1 (datasheet 22.9.2), exactly as twiboot's polling loop
 * expects. simavr 1.6's own slave side is not used: its TWINT is sticky to a
 * write of 1, a START carrying data is delivered twice, and a STOP never
 * reads 0xA0. */
#define TWSR_ADDR      0xB9
#define TWDR_ADDR      0xBB
#define TWEA           0x40
#define GPIOR2_ADDR    0x4B
#define PIN_TICKS      ((30000 / 16) / 8)   /* BOOT_TICKS(SF_PIN_TIMEOUT_MS) */

static avr_io_write_t twcr_orig;
static void *twcr_orig_param;

static void twcr_write(avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
    (void)param;
    twcr_orig(avr, addr, v, twcr_orig_param);
    if (v & TWINT) avr->data[TWCR_ADDR] &= (uint8_t)~TWINT;
}

/* 1 = handled (TWINT cleared), 0 = program started, -1 = not handled. */
static int hw_step(avr_t *avr, uint8_t status, uint8_t data)
{
    avr->data[TWDR_ADDR] = data;
    avr->data[TWSR_ADDR] = status;
    avr->data[TWCR_ADDR] |= TWINT;
    double deadline = now_ms(avr) + 50;
    while (now_ms(avr) < deadline) {
        avr_run(avr);
        if (!in_boot(avr)) return 0;
        if (!(avr->data[TWCR_ADDR] & TWINT)) return 1;
    }
    return -1;
}

/* One master write of a single command byte: SLA+W, the byte, STOP. The
 * command is checked by its effect: GPIOR2, the bootloader's countdown,
 * must hold the full pin window. */
static int send_cmd(avr_t *avr, uint8_t cmd)
{
    if (!(avr->data[TWCR_ADDR] & TWEA)) { printf("  slave not acknowledging\n"); return 0; }
    if (hw_step(avr, 0x60, 0) != 1) return 0;     /* SLA+W received, ACK */
    if (hw_step(avr, 0x80, cmd) != 1) return 0;   /* data received, ACK */
    if (hw_step(avr, 0xA0, 0) != 1) return 0;     /* STOP */
#ifndef PIN_SKIP_COUNTDOWN_CHECK   /* for running an unbounded image */
    if (avr->data[GPIOR2_ADDR] < PIN_TICKS - 1) {
        printf("  countdown reads %u, expected %u\n", avr->data[GPIOR2_ADDR], PIN_TICKS);
        return 0;
    }
#endif
    return in_boot(avr);
}

static avr_t *start(void)
{
    avr_t *avr = make_unit();
    int io = AVR_DATA_TO_IO(TWCR_ADDR);
    twcr_orig = avr->io[io].w.c;
    twcr_orig_param = avr->io[io].w.param;
    avr->io[io].w.c = twcr_write;
    return avr;
}

static int case_a(void)
{
    printf("\n=== (A) power-on, no master\n");
    avr_t *avr = start();
    double left = 0;
    if (!run_to(avr, 5000, &left)) { printf("  FAIL: still in the bootloader at 5 s\n"); return 0; }
    printf("  program started at %.0f ms  %s\n", left, left < 1500 ? "PASS" : "FAIL");
    return left < 1500;
}

static int case_b(void)
{
    printf("\n=== (B) one CMD_WAIT at 200 ms, then silence\n");
    avr_t *avr = start();
    if (run_to(avr, 200, NULL)) { printf("  FAIL: left before the command\n"); return 0; }
    printf("  bootloader listens on 0x%02X\n", avr->data[TWAR_ADDR] >> 1);
    if (!send_cmd(avr, CMD_WAIT)) { printf("  FAIL: command not taken\n"); return 0; }
    double sent = now_ms(avr);
    if (run_to(avr, sent + 25000, NULL)) { printf("  FAIL: not held for 25 s\n"); return 0; }
    printf("  held at %.0f ms (25 s after the command)\n", now_ms(avr));
    double left = 0;
    if (!run_to(avr, sent + 40000, &left)) {
        printf("  FAIL: still pinned 40 s after the last command\n"); return 0;
    }
    double after = left - sent;
    int ok = after > PIN_MS - TOLERANCE_MS && after < PIN_MS + TOLERANCE_MS;
    printf("  program started %.0f ms after the command  %s\n", after, ok ? "PASS" : "FAIL");
    return ok;
}

static int case_c(void)
{
    printf("\n=== (C) CMD_WAIT every 10 s for 60 s, then silence\n");
    avr_t *avr = start();
    if (run_to(avr, 200, NULL)) { printf("  FAIL: left before the command\n"); return 0; }
    double last = 0;
    for (int i = 0; i <= 6; i++) {
        if (i && run_to(avr, 200 + i * 10000.0, NULL)) {
            printf("  FAIL: left at %.0f ms while pinned\n", now_ms(avr)); return 0;
        }
        if (!send_cmd(avr, CMD_WAIT)) { printf("  FAIL: command %d not taken\n", i); return 0; }
        last = now_ms(avr);
    }
    printf("  held through %d commands, last at %.0f ms\n", 7, last);
    double left = 0;
    if (!run_to(avr, last + 40000, &left)) {
        printf("  FAIL: still pinned 40 s after the last command\n"); return 0;
    }
    double after = left - last;
    int ok = after > PIN_MS - TOLERANCE_MS && after < PIN_MS + TOLERANCE_MS;
    printf("  program started %.0f ms after the last command (%.0f ms total)  %s\n",
           after, left, ok ? "PASS" : "FAIL");
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <unit_app.bin> <twiboot.bin>\n", argv[0]);
        return 2;
    }
    memset(app_image, 0xFF, sizeof(app_image));
    if (read_file(argv[1], app_image, sizeof(app_image)) < 256) {
        fprintf(stderr, "unit image too small\n"); return 2;
    }
    memset(boot_image, 0xFF, BOOT_LEN);
    if (read_file(argv[2], boot_image, BOOT_LEN) < 896) {
        fprintf(stderr, "boot image too small\n"); return 2;
    }
    int pass = case_a();
    pass &= case_b();
    pass &= case_c();
    return pass ? 0 : 1;
}
