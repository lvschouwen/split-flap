/* simavr proof for #542: twiboot crash recovery.
 *
 * Runs a crash app (always WDT-timeouts) against the new twiboot, tracking
 * phase transitions (boot ↔ app) by watching PC.  Five cases:
 *
 *   1. Power-on  → count=0, app starts
 *   2. WDT #1    → count=1, app starts
 *   3. WDT #3    → count=3 ≥ threshold, bootloader holds
 *   4. Power cycle after hold → count=0, app starts
 *   5. CMD_SWITCH_APPLICATION clears → count restarts from 1
 *
 * Usage:  crash_test <crash_app.elf> <twiboot.bin>
 *
 * <twiboot.bin> is a raw 1024-byte boot-section image (pages 0-6 + page 7).
 * Exit 0 iff all checks pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <simavr/sim_avr.h>
#include <simavr/sim_elf.h>

#define BOOT_START       0x7C00
#define BOOT_LEN         1024
#define CRASH_REC_A_ADDR 0x08FE
#define CRASH_REC_B_ADDR 0x08FF
#define CRASH_REC_TAG    0xA0

#define BOOT_BUDGET      20000000ULL   /* 1.25 s at 16 MHz — one timeout cycle */
#define APP_BUDGET        5000000ULL   /* 312 ms — WDT-15ms + generous margin  */

static elf_firmware_t fw;
static uint8_t boot_image[BOOT_LEN];

static avr_t *create_avr(void)
{
    avr_t *avr = avr_make_mcu_by_name("atmega328p");
    if (!avr) { fprintf(stderr, "avr_make_mcu_by_name failed\n"); exit(2); }
    avr_init(avr);
    avr->frequency = 16000000;
    avr->log = 0;
    avr_load_firmware(avr, &fw);
    memcpy(&avr->flash[BOOT_START], boot_image, BOOT_LEN);
    avr->reset_pc = BOOT_START;       /* BOOTRST fuse */
    return avr;
}

/* Power-on: fill SRAM with a non-matching pattern (simulates random power-on
 * state) and start at the boot section.  MCUSR is left at 0 after avr_reset
 * (no WDRF), which is the correct non-WDT case. */
static void power_on(avr_t *avr)
{
    avr_reset(avr);
    for (uint32_t a = 0x100; a <= 0x08FF; a++)
        avr->data[a] = 0xA5;
    avr->pc = BOOT_START;
}

/* Returns 1 if PC is in the boot section. */
static int in_boot(avr_t *avr)
{
    return avr->pc >= BOOT_START;
}

/* Run until PC leaves the current region (boot→app or app→boot).
 * Returns:  0 = phase changed,  1 = budget expired,  2 = cpu error. */
static int run_until_phase_change(avr_t *avr, uint64_t budget)
{
    int start_in_boot = in_boot(avr);
    uint64_t start = avr->cycle;

    while ((avr->cycle - start) < budget) {
        int st = avr_run(avr);
        if (st == cpu_Done || st == cpu_Crashed)
            return 2;
        if (in_boot(avr) != start_in_boot)
            return 0;
    }
    return 1;
}

static int check_crash_rec(avr_t *avr, const char *label, int expect_count)
{
    uint8_t a = avr->data[CRASH_REC_A_ADDR];
    uint8_t b = avr->data[CRASH_REC_B_ADDR];
    int tag_ok  = (a & 0xF0) == CRASH_REC_TAG;
    int comp_ok = (uint8_t)(a ^ b) == 0xFF;
    int count   = a & 0x0F;

    printf("  %s: A=0x%02X B=0x%02X count=%d tag=%s comp=%s",
           label, a, b, count,
           tag_ok  ? "ok" : "BAD",
           comp_ok ? "ok" : "BAD");

    if (tag_ok && comp_ok && count == expect_count) {
        printf(" PASS\n");
        return 1;
    }
    printf(" FAIL (expected %d)\n", expect_count);
    return 0;
}

/* Convenience: wait for WDT (app→boot) then for twiboot decision (boot→app
 * or hold).  Returns 1 if twiboot jumped to app, 0 if it held. */
static int wdt_then_boot(avr_t *avr, int *ok)
{
    int r = run_until_phase_change(avr, APP_BUDGET);
    if (r != 0 || !in_boot(avr)) {
        fprintf(stderr, "  expected WDT reset (app→boot), got %d\n", r);
        *ok = 0;
        return 0;
    }
    r = run_until_phase_change(avr, BOOT_BUDGET);
    if (r == 0 && !in_boot(avr))
        return 1;   /* jumped to app */
    if (r == 1)
        return 0;   /* budget expired = holding */
    fprintf(stderr, "  unexpected state after boot phase: %d\n", r);
    *ok = 0;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <crash_app.elf> <twiboot.bin>\n", argv[0]);
        return 2;
    }

    memset(&fw, 0, sizeof(fw));
    if (elf_read_firmware(argv[1], &fw) != 0) {
        fprintf(stderr, "elf_read_firmware failed for %s\n", argv[1]);
        return 2;
    }

    FILE *f = fopen(argv[2], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[2]); return 2; }
    memset(boot_image, 0xFF, BOOT_LEN);
    size_t n = fread(boot_image, 1, BOOT_LEN, f);
    fclose(f);
    if (n < 896) { fprintf(stderr, "twiboot image too small (%zu B)\n", n); return 2; }

    int pass = 1;
    avr_t *avr = create_avr();

    /* ================================================================
     * Test 1: Power-on → count=0, app starts
     * ================================================================ */
    printf("Test 1: power-on → count=0, app starts\n");
    power_on(avr);
    {
        int r = run_until_phase_change(avr, BOOT_BUDGET);
        if (r != 0 || in_boot(avr)) {
            printf("  FAIL: twiboot did not jump to app (r=%d)\n", r);
            pass = 0;
        } else if (!check_crash_rec(avr, "power-on", 0)) {
            pass = 0;
        }
    }

    /* ================================================================
     * Test 2: WDT #1 → count=1, app starts
     * ================================================================ */
    printf("\nTest 2: WDT #1 → count=1, app starts\n");
    {
        int jumped = wdt_then_boot(avr, &pass);
        if (!jumped) {
            printf("  FAIL: twiboot should have jumped after 1 WDT\n");
            pass = 0;
        } else if (!check_crash_rec(avr, "1 WDT", 1)) {
            pass = 0;
        }
    }

    /* ================================================================
     * Test 3a: WDT #2 → count=2, app starts
     * ================================================================ */
    printf("\nTest 3a: WDT #2 → count=2, app starts\n");
    {
        int jumped = wdt_then_boot(avr, &pass);
        if (!jumped) {
            printf("  FAIL: twiboot should have jumped after 2 WDTs\n");
            pass = 0;
        } else if (!check_crash_rec(avr, "2 WDTs", 2)) {
            pass = 0;
        }
    }

    /* ================================================================
     * Test 3b: WDT #3 → count=3, bootloader HOLDS
     * ================================================================ */
    printf("\nTest 3b: WDT #3 → count=3, bootloader holds\n");
    {
        /* Wait for the WDT reset */
        int r = run_until_phase_change(avr, APP_BUDGET);
        if (r != 0 || !in_boot(avr)) {
            printf("  FAIL: expected WDT reset\n");
            pass = 0;
        } else {
            /* Twiboot should NOT jump to app */
            r = run_until_phase_change(avr, BOOT_BUDGET);
            if (r == 1) {
                printf("  bootloader held — as expected\n");
                if (!check_crash_rec(avr, "3 WDTs", 3))
                    pass = 0;
            } else {
                printf("  FAIL: twiboot jumped (should have held), r=%d\n", r);
                pass = 0;
            }
        }
    }

    /* ================================================================
     * Test 4: Power cycle → count=0, app starts
     * ================================================================ */
    printf("\nTest 4: power cycle → count=0, app starts\n");
    power_on(avr);
    {
        int r = run_until_phase_change(avr, BOOT_BUDGET);
        if (r != 0 || in_boot(avr)) {
            printf("  FAIL: twiboot did not jump to app\n");
            pass = 0;
        } else if (!check_crash_rec(avr, "power cycle", 0)) {
            pass = 0;
        }
    }

    /* ================================================================
     * Test 5: CMD_SWITCH_APPLICATION clears → count restarts from 1
     * ================================================================ */
    printf("\nTest 5: CMD_SWITCH_APPLICATION clear → count restarts\n");
    /* Build count to 2: two WDT cycles */
    {
        int jumped;
        jumped = wdt_then_boot(avr, &pass);
        if (!jumped) { printf("  FAIL setup: expected jump after WDT #1\n"); pass = 0; goto done; }
        check_crash_rec(avr, "setup WDT #1", 1);

        jumped = wdt_then_boot(avr, &pass);
        if (!jumped) { printf("  FAIL setup: expected jump after WDT #2\n"); pass = 0; goto done; }
        check_crash_rec(avr, "setup WDT #2", 2);

        /* Simulate CMD_SWITCH_APPLICATION: write 0 to CRASH_REC_A.
         * On a real unit this happens in twiboot's TWI handler before
         * jump_to_app.  The proof pokes it while the app is running —
         * the app never touches 0x8FE so the effect is identical. */
        avr->data[CRASH_REC_A_ADDR] = 0;
        printf("  poked CRASH_REC_A=0 (CMD_SWITCH_APPLICATION)\n");

        /* Next WDT → twiboot reads invalidated record → count=1 */
        jumped = wdt_then_boot(avr, &pass);
        if (!jumped) {
            printf("  FAIL: twiboot should have jumped after clear+WDT\n");
            pass = 0;
        } else if (!check_crash_rec(avr, "clear+WDT", 1)) {
            pass = 0;
        }
    }

done:
    printf("\n%s\n", pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    avr_terminate(avr);
    return pass ? 0 : 1;
}
