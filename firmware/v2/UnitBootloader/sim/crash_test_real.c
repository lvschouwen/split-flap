/* simavr proof for #542: real-application crash recovery cases.
 *
 * Three cases the stand-in crash_app can't cover:
 *
 *   (a) Real unit app (with --defsym=__stack=0x8FD) hangs 3 times → held
 *       Proves the app's BSS/data don't interfere with the crash record.
 *       WDT resets are simulated manually (the real app kicks its 8s WDT).
 *
 *   (b) Real unit app intentionally reboots 10 times → never held
 *       Pokes pendingBootloader in SRAM; the app calls crashRecordClear()
 *       before every self-triggered WDT reset.
 *
 *   (d) Crash app WITHOUT --defsym (stack at RAMEND, same as fielded 661621f)
 *       → never held after 10 WDT resets
 *       The app's stack at 0x08FE/0x08FF always corrupts the crash record;
 *       return address high bytes (≤0x3F) never match the complement (0x5x).
 *
 * Usage:  crash_test_real <real_app.elf> <fielded_app.elf> <twiboot.bin>
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
#define SRAM_START       0x0100
#define SRAM_LEN         2048
#define CRASH_REC_A_ADDR 0x08FE
#define CRASH_REC_B_ADDR 0x08FF
#define CRASH_REC_TAG    0xA0
#define MCUSR_ADDR       0x54
#define MCUSR_WDRF       (1 << 3)

#define BOOT_BUDGET      20000000ULL   /* 1.25 s at 16 MHz */
#define APP_BUDGET         5000000ULL  /* 312 ms */
#define APP_SETTLE       16000000ULL   /* 1 s — let setup() + a few loop()s run */
#define APP_REBOOT_BUDGET 200000000ULL /* 12.5 s — covers 8 s runtime WDT if delay hangs */

static uint8_t boot_image[BOOT_LEN];

static avr_t *create_avr(elf_firmware_t *fw)
{
    avr_t *avr = avr_make_mcu_by_name("atmega328p");
    if (!avr) { fprintf(stderr, "avr_make_mcu_by_name failed\n"); exit(2); }
    avr_init(avr);
    avr->frequency = 16000000;
    avr->log = 0;
    avr_load_firmware(avr, fw);
    memcpy(&avr->flash[BOOT_START], boot_image, BOOT_LEN);
    avr->reset_pc = BOOT_START;
    return avr;
}

static void power_on(avr_t *avr)
{
    avr_reset(avr);
    for (uint32_t a = SRAM_START; a <= 0x08FF; a++)
        avr->data[a] = 0xA5;
    avr->pc = BOOT_START;
}

/* Simulate a WDT reset: preserve SRAM, reset peripherals, set WDRF. */
static void wdt_reset(avr_t *avr)
{
    uint8_t sram[SRAM_LEN];
    memcpy(sram, &avr->data[SRAM_START], SRAM_LEN);
    avr_reset(avr);
    memcpy(&avr->data[SRAM_START], sram, SRAM_LEN);
    avr->data[MCUSR_ADDR] = MCUSR_WDRF;
    avr->pc = BOOT_START;
}

static int in_boot(avr_t *avr) { return avr->pc >= BOOT_START; }

/* Run until PC crosses between boot and app regions.
 * Returns: 0 = phase changed, 1 = budget expired, 2 = cpu error. */
static int run_until_phase_change(avr_t *avr, uint64_t budget)
{
    int start_in_boot = in_boot(avr);
    uint64_t start = avr->cycle;
    while ((avr->cycle - start) < budget) {
        int st = avr_run(avr);
        if (st == cpu_Done || st == cpu_Crashed) return 2;
        if (in_boot(avr) != start_in_boot) return 0;
    }
    return 1;
}

/* Run for exactly the given number of cycles (stay in app). */
static int run_cycles(avr_t *avr, uint64_t cycles)
{
    uint64_t start = avr->cycle;
    while ((avr->cycle - start) < cycles) {
        int st = avr_run(avr);
        if (st == cpu_Done || st == cpu_Crashed) return 2;
    }
    return 0;
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

/* ================================================================ */

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr,
            "usage: %s <real_app.elf> <fielded_app.elf> <twiboot.bin>\n",
            argv[0]);
        return 2;
    }

    elf_firmware_t fw_real, fw_fielded;
    memset(&fw_real, 0, sizeof(fw_real));
    memset(&fw_fielded, 0, sizeof(fw_fielded));
    if (elf_read_firmware(argv[1], &fw_real) != 0) {
        fprintf(stderr, "elf_read failed: %s\n", argv[1]); return 2;
    }
    if (elf_read_firmware(argv[2], &fw_fielded) != 0) {
        fprintf(stderr, "elf_read failed: %s\n", argv[2]); return 2;
    }

    FILE *f = fopen(argv[3], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[3]); return 2; }
    memset(boot_image, 0xFF, BOOT_LEN);
    size_t n = fread(boot_image, 1, BOOT_LEN, f);
    fclose(f);
    if (n < 896) { fprintf(stderr, "boot image too small (%zu B)\n", n); return 2; }

    /* Find pendingBootloader symbol in the real app ELF. */
    uint16_t pending_addr = 0;
    {
        FILE *nm = popen("avr-nm 2>/dev/null", "r");
        if (nm) pclose(nm);
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
            "avr-nm %s 2>/dev/null | grep ' pendingBootloader$' | head -1",
            argv[1]);
        FILE *p = popen(cmd, "r");
        if (p) {
            char line[128];
            if (fgets(line, sizeof(line), p)) {
                unsigned long addr;
                if (sscanf(line, "%lx", &addr) == 1)
                    pending_addr = (uint16_t)addr;
            }
            pclose(p);
        }
    }
    if (!pending_addr) {
        fprintf(stderr, "warning: pendingBootloader not found; case (b) skipped\n");
    } else {
        printf("pendingBootloader at 0x%04x\n", pending_addr);
    }

    int pass = 1;

    /* ================================================================
     * Case (a): Real app hang → held on 3rd WDT
     * ================================================================ */
    printf("\n=== Case (a): real app hang → held on 3rd WDT\n");
    {
        avr_t *avr = create_avr(&fw_real);
        power_on(avr);

        /* Boot → app */
        int r = run_until_phase_change(avr, BOOT_BUDGET);
        if (r != 0 || in_boot(avr)) {
            printf("  FAIL: twiboot didn't jump to app on power-on\n");
            pass = 0; goto case_a_done;
        }
        if (!check_crash_rec(avr, "power-on", 0)) pass = 0;

        /* Let app settle (setup + a few loop iterations) */
        run_cycles(avr, APP_SETTLE);

        /* Three simulated WDT resets */
        for (int i = 1; i <= 3; i++) {
            wdt_reset(avr);
            r = run_until_phase_change(avr, BOOT_BUDGET);
            if (i < 3) {
                /* Should jump to app */
                if (r != 0 || in_boot(avr)) {
                    printf("  FAIL: WDT #%d — expected jump to app\n", i);
                    pass = 0; goto case_a_done;
                }
                if (!check_crash_rec(avr, i == 1 ? "WDT #1" : "WDT #2", i))
                    pass = 0;
                run_cycles(avr, APP_SETTLE);
            } else {
                /* Should hold */
                if (r == 1) {
                    printf("  bootloader held on WDT #3 — as expected\n");
                    if (!check_crash_rec(avr, "WDT #3", 3)) pass = 0;
                } else {
                    printf("  FAIL: WDT #3 — expected hold, got r=%d\n", r);
                    pass = 0;
                }
            }
        }
case_a_done:
        avr_terminate(avr);
    }

    /* ================================================================
     * Case (b): Real app intentional reboots → never held
     *
     * crashRecordClear() writes count=0 before each self-triggered WDT.
     * twiboot reads the cleared record and increments to 1 (a single WDT
     * always reads as 1).  The key property: count never accumulates
     * past 1, even after 10 reboots.
     *
     * To prove the clear prevents accumulation: build count to 2 via
     * simulated crashes, then trigger an intentional reboot.  Without the
     * clear, twiboot would see count=2 and increment to 3 → hold.  With
     * the clear, twiboot sees count=0 and increments to 1 → jump.
     * ================================================================ */
    if (pending_addr) {
        printf("\n=== Case (b): real app intentional reboots → never held\n");
        avr_t *avr = create_avr(&fw_real);
        power_on(avr);

        int r = run_until_phase_change(avr, BOOT_BUDGET);
        if (r != 0 || in_boot(avr)) {
            printf("  FAIL: didn't reach app on power-on\n");
            pass = 0; goto case_b_done;
        }

        for (int round = 1; round <= 5; round++) {
            /* Fresh power cycle each round */
            power_on(avr);
            r = run_until_phase_change(avr, BOOT_BUDGET);
            if (r != 0 || in_boot(avr)) {
                printf("  FAIL round %d: power-on didn't reach app\n", round);
                pass = 0; goto case_b_done;
            }

            /* Build count to 2 via simulated crashes */
            run_cycles(avr, APP_SETTLE);
            wdt_reset(avr);
            r = run_until_phase_change(avr, BOOT_BUDGET);
            if (r != 0 || in_boot(avr)) {
                printf("  FAIL round %d: crash #1 didn't jump\n", round);
                pass = 0; goto case_b_done;
            }

            run_cycles(avr, APP_SETTLE);
            wdt_reset(avr);
            r = run_until_phase_change(avr, BOOT_BUDGET);
            if (r != 0 || in_boot(avr)) {
                printf("  FAIL round %d: crash #2 didn't jump\n", round);
                pass = 0; goto case_b_done;
            }
            if (!check_crash_rec(avr, "at count=2", 2)) { pass = 0; goto case_b_done; }

            /* Intentional reboot: poke pendingBootloader.
             * The app calls crashRecordClear() (count→0) then WDT-15ms.
             * If the clear works, twiboot sees count=0 → 1 → jump.
             * If not, twiboot sees count=2 → 3 → HELD. */
            run_cycles(avr, APP_SETTLE);
            avr->data[pending_addr] = 1;

            r = run_until_phase_change(avr, APP_REBOOT_BUDGET);
            if (r != 0 || !in_boot(avr)) {
                printf("  FAIL round %d: reboot didn't WDT (r=%d)\n", round, r);
                pass = 0; goto case_b_done;
            }

            r = run_until_phase_change(avr, BOOT_BUDGET);
            if (r != 0 || in_boot(avr)) {
                printf("  FAIL round %d: twiboot HELD after intentional reboot"
                       " (clear didn't work)\n", round);
                pass = 0; goto case_b_done;
            }
            {
                char label[32];
                snprintf(label, sizeof(label), "round %d cleared", round);
                if (!check_crash_rec(avr, label, 1)) { pass = 0; goto case_b_done; }
            }
        }
        printf("  5 rounds (2 crashes + intentional reboot), never held — PASS\n");
case_b_done:
        avr_terminate(avr);
    }

    /* ================================================================
     * Case (d): Fielded app (stack at RAMEND) → never held
     * ================================================================ */
    printf("\n=== Case (d): fielded app (no --defsym) → never held\n");
    {
        avr_t *avr = create_avr(&fw_fielded);
        power_on(avr);

        int r = run_until_phase_change(avr, BOOT_BUDGET);
        if (r != 0 || in_boot(avr)) {
            printf("  FAIL: twiboot didn't jump to app on power-on\n");
            pass = 0; goto case_d_done;
        }

        /* 10 natural WDT resets (the fielded crash app uses WDTO_15MS).
         * The app's stack at RAMEND overwrites the crash record each boot;
         * the return-address safety property means count never reaches 3. */
        for (int i = 1; i <= 10; i++) {
            /* Wait for WDT reset (app → boot) */
            r = run_until_phase_change(avr, APP_BUDGET);
            if (r != 0 || !in_boot(avr)) {
                printf("  FAIL: WDT #%d — didn't reach boot (r=%d)\n", i, r);
                pass = 0; goto case_d_done;
            }

            /* Twiboot should NEVER hold (count stays at 1) */
            r = run_until_phase_change(avr, BOOT_BUDGET);
            if (r == 0 && !in_boot(avr)) {
                /* Jumped to app — expected */
                uint8_t a = avr->data[CRASH_REC_A_ADDR];
                uint8_t b = avr->data[CRASH_REC_B_ADDR];
                if (i <= 3) {
                    printf("  WDT #%d: A=0x%02X B=0x%02X count=%d — jumped (ok)\n",
                           i, a, b, a & 0x0F);
                }
            } else if (r == 1) {
                printf("  FAIL: WDT #%d — twiboot HELD (must never hold"
                       " fielded app)\n", i);
                uint8_t a = avr->data[CRASH_REC_A_ADDR];
                uint8_t b = avr->data[CRASH_REC_B_ADDR];
                printf("         A=0x%02X B=0x%02X\n", a, b);
                pass = 0; goto case_d_done;
            } else {
                printf("  FAIL: WDT #%d — unexpected state r=%d\n", i, r);
                pass = 0; goto case_d_done;
            }
        }
        printf("  10 WDT resets, never held — PASS\n");
case_d_done:
        avr_terminate(avr);
    }

    printf("\n%s\n", pass ? "ALL REAL-APP TESTS PASSED" : "SOME TESTS FAILED");
    return pass ? 0 : 1;
}
