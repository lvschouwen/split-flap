/* simavr proof for #554: a unit whose flash stopped short is kept by its
 * bootloader instead of starting a part-written program.
 *
 * A flash writes the hold page (twibootFillHoldPage, shared/TwibootFlash.h)
 * to address 0 first and the image's own first page last. Until that last
 * write, whatever else is in flash, a start of the program is sixteen bytes
 * that switch the watchdog on and wait for it. The bootloader counts the
 * watchdog resets (#542) and keeps the unit at the third.
 *
 *   (1) hold page + the rest of the unit image   → held at the 3rd reset
 *   (2) hold page + blank flash above it         → held at the 3rd reset
 *   (3) the whole unit image (the flash ended)   → starts and stays up
 *
 * In (1) and (2) the program counter never leaves the hold code, so what
 * sits above page 0 cannot matter. The watchdog resets are simavr's own.
 *
 * Usage:  torn_flash_test <unit_app.bin> <twiboot.bin> <hold_page.bin>
 * Exit 0 iff all checks pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <simavr/sim_avr.h>

#define BOOT_START       0x7C00
#define BOOT_LEN         1024
#define PAGE_SIZE        128
#define HOLD_CODE_LEN    16
#define CRASH_REC_A_ADDR 0x08FE
#define CRASH_REC_B_ADDR 0x08FF
#define CRASH_REC_TAG    0xA0

#define CYCLES_PER_MS    16000ULL
#define BOOT_BUDGET      (1250 * CYCLES_PER_MS)  /* past twiboot's 1 s listen */
#define HOLD_RESET_MAX   (40 * CYCLES_PER_MS)    /* 16 ms watchdog, with room */
#define HELD_FOR         (40000 * CYCLES_PER_MS) /* past SF_PIN_TIMEOUT_MS (30 s) */
#define APP_UP_FOR       (3000 * CYCLES_PER_MS)

static uint8_t boot_image[BOOT_LEN];
static uint8_t app_image[BOOT_START];
static uint8_t hold_page[PAGE_SIZE];

static size_t read_file(const char *path, uint8_t *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static avr_t *make_unit(const uint8_t *app)
{
    avr_t *avr = avr_make_mcu_by_name("atmega328p");
    if (!avr) { fprintf(stderr, "avr_make_mcu_by_name failed\n"); exit(2); }
    avr_init(avr);
    avr->frequency = 16000000;
    avr->log = 0;
    memcpy(avr->flash, app, BOOT_START);
    memcpy(&avr->flash[BOOT_START], boot_image, BOOT_LEN);
    avr->reset_pc = BOOT_START;       /* BOOTRST fuse */
    avr_reset(avr);
    for (uint32_t a = 0x100; a <= 0x08FF; a++)
        avr->data[a] = 0xA5;          /* power-on: no crash record */
    avr->pc = BOOT_START;
    return avr;
}

static int in_boot(avr_t *avr) { return avr->pc >= BOOT_START; }

/* Runs until the unit crosses between bootloader and program.
 * 0 = crossed, 1 = budget spent, 2 = cpu error, 3 = program left the hold
 * code (only checked when `hold_only`). */
static int run_until_crossing(avr_t *avr, uint64_t budget, int hold_only)
{
    int start_in_boot = in_boot(avr);
    uint64_t start = avr->cycle;
    while ((avr->cycle - start) < budget) {
        int st = avr_run(avr);
        if (st == cpu_Done || st == cpu_Crashed) return 2;
        if (in_boot(avr) != start_in_boot) return 0;
        if (hold_only && !start_in_boot && avr->pc >= HOLD_CODE_LEN) return 3;
    }
    return 1;
}

static int crash_count(avr_t *avr)
{
    uint8_t a = avr->data[CRASH_REC_A_ADDR];
    uint8_t b = avr->data[CRASH_REC_B_ADDR];
    if ((a & 0xF0) != CRASH_REC_TAG || (uint8_t)(a ^ b) != 0xFF) return 0;
    return a & 0x0F;
}

/* A flash that stopped short: three starts, each reset by the watchdog, then
 * the bootloader keeps the unit. */
static int stopped_flash_is_held(const char *label, const uint8_t *app)
{
    printf("\n=== %s\n", label);
    avr_t *avr = make_unit(app);
    for (int reset = 1; reset <= 3; reset++) {
        int r = run_until_crossing(avr, BOOT_BUDGET, 0);
        if (r != 0 || in_boot(avr)) {
            printf("  FAIL: start %d: the bootloader did not start the program (%d)\n", reset, r);
            return 0;
        }
        uint64_t started = avr->cycle;
        r = run_until_crossing(avr, HOLD_RESET_MAX, 1);
        if (r != 0) {
            printf("  FAIL: start %d: %s\n", reset,
                   r == 3 ? "the program left the hold code"
                          : "no watchdog reset");
            return 0;
        }
        printf("  start %d: watchdog reset after %.1f ms\n", reset,
               (double)(avr->cycle - started) / CYCLES_PER_MS);
    }
    int r = run_until_crossing(avr, HELD_FOR, 0);
    if (r != 1) {
        printf("  FAIL: the bootloader let the unit go again (%d)\n", r);
        return 0;
    }
    if (crash_count(avr) != 3) {
        printf("  FAIL: crash record reads %d, expected 3\n", crash_count(avr));
        return 0;
    }
    printf("  held by the bootloader for %llu s, crash record 3  PASS\n",
           (unsigned long long)(HELD_FOR / CYCLES_PER_MS / 1000));
    return 1;
}

static int whole_image_stays_up(const uint8_t *app)
{
    printf("\n=== (3) the whole image: starts and stays up\n");
    avr_t *avr = make_unit(app);
    int r = run_until_crossing(avr, BOOT_BUDGET, 0);
    if (r != 0 || in_boot(avr)) {
        printf("  FAIL: the bootloader did not start the program (%d)\n", r);
        return 0;
    }
    r = run_until_crossing(avr, APP_UP_FOR, 0);
    if (r != 1) {
        printf("  FAIL: the program went back to the bootloader (%d)\n", r);
        return 0;
    }
    printf("  up for %llu s, crash record %d  PASS\n",
           (unsigned long long)(APP_UP_FOR / CYCLES_PER_MS / 1000),
           crash_count(avr));
    return crash_count(avr) == 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <unit_app.bin> <twiboot.bin> <hold_page.bin>\n", argv[0]);
        return 2;
    }
    memset(app_image, 0xFF, sizeof(app_image));
    size_t app_len = read_file(argv[1], app_image, sizeof(app_image));
    memset(boot_image, 0xFF, BOOT_LEN);
    if (read_file(argv[2], boot_image, BOOT_LEN) < 896) {
        fprintf(stderr, "boot image too small\n"); return 2;
    }
    if (read_file(argv[3], hold_page, PAGE_SIZE) != PAGE_SIZE) {
        fprintf(stderr, "hold page is not one page\n"); return 2;
    }
    if (app_len <= PAGE_SIZE) { fprintf(stderr, "unit image too small\n"); return 2; }
    printf("unit image %zu bytes\n", app_len);

    static uint8_t torn[BOOT_START];
    int pass = 1;

    memcpy(torn, app_image, sizeof(torn));
    memcpy(torn, hold_page, PAGE_SIZE);
    pass &= stopped_flash_is_held("(1) hold page + the rest of the image", torn);

    memset(torn, 0xFF, sizeof(torn));
    memcpy(torn, hold_page, PAGE_SIZE);
    pass &= stopped_flash_is_held("(2) hold page + blank flash", torn);

    pass &= whole_image_stays_up(app_image);

    return pass ? 0 : 1;
}
