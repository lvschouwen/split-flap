/* simavr C-API harness for the #499 in-system twiboot update proof.
 *
 * Runs updater_app.elf (the unit's own BootUpdateAvr.h core) on a simulated
 * ATmega328P whose boot section starts as <start_boot.bin>, models BOOTRST
 * (every reset enters 0x7C00), and power-cycles the chip on request.
 *
 * Usage:
 *   runtest <app.elf> <start_boot.bin> <expected_boot.bin> <report_sram_hex>
 *           [--start app|reset]      app: the sketch is already running (pc=0);
 *                                    reset: power-on through the boot section
 *           [--boots K]              finish K app runs ("done"), power-cycling
 *                                    between them (default 1)
 *           [--expect I:RESULT:STATE] the I-th done (0-based) must report these
 *                                    (repeatable)
 *           [--kill-at-spm N]        power loss just before the N-th (0-based)
 *                                    executed SPM instruction, then reset
 *           [--corrupt-at-spm N ADDR] invert flash byte ADDR just before the
 *                                    N-th SPM executes (a write that did not take)
 *           [--sweep-kill FROM TO]   one fresh run per N in [FROM,TO] with
 *                                    --kill-at-spm N; each must end matching
 *                                    the expected image in state New, unless
 *                                    page 0 was blank at the kill (a brick window)
 *           [--expect-windows W]     the sweep must find exactly W such points
 *           [--twiboot-ram CMD:FLAG:COUNT] when stage 1 jumps into twiboot,
 *                                    set the bytes twiboot's idle loop reads
 *                                    from (overlaid) application RAM: cmd
 *                                    0x019D (0x21 = jump to app), timeout-armed
 *                                    flag 0x0100, countdown 0x0101. On a unit
 *                                    they are whatever the sketch left there.
 *           [--expect-resets R]      exactly R resets (entries at 0x7C00)
 *           [--expect-spm S]         exactly S SPM instructions executed
 *           [--max-cycles C]         per-run instruction budget
 *
 * <report_sram_hex> is the raw data-space address of sim_report {done, result,
 * state} in the app. SPM instructions are counted wherever they execute
 * (twiboot's handler in stage 1, do_spm in stage 2), so a kill index is "this
 * many flash operations completed". simavr executes an SPM atomically and does
 * not model its multi-ms duration; a real power loss DURING an erase or write
 * leaves that one page undefined, which for any page but 0 is covered by the
 * same resume path as the point either side of it.
 *
 * Exit 0 iff every check passed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <simavr/sim_avr.h>
#include <simavr/sim_elf.h>

#define BOOT_START 0x7c00
#define BOOT_LEN   1024
#define PAGE_SIZE  128
#define SIM_DONE   0xD0
#define MAX_EXPECT 8

static elf_firmware_t fw;
static uint8_t start_boot[BOOT_LEN];
static uint8_t expected[BOOT_LEN];
static uint32_t report_a;
static int start_at_reset = 0;
static int boots = 1;
static long kill_at = -1;
static long corrupt_at = -1;
static uint32_t corrupt_addr = 0;
static uint64_t max_cycles = 60000000ULL;  /* ~2.4x the slowest legitimate run */
static struct { int idx, result, state; } expects[MAX_EXPECT];
static int n_expects = 0;
static int tw_ram_set = 0, tw_cmd, tw_flag, tw_count;
static long expect_resets = -1;
static long expect_spm = -1;

#define TWIBOOT_HANDLER_ENTRY 0x7e5a

static void load_bin(const char* path, uint8_t* dst) {
  memset(dst, 0xFF, BOOT_LEN);
  FILE* f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
  size_t n = fread(dst, 1, BOOT_LEN, f);
  fclose(f);
  if (n == 0) { fprintf(stderr, "empty %s\n", path); exit(2); }
}

static avr_t* fresh_avr(void) {
  avr_t* avr = avr_make_mcu_by_name("atmega328p");
  if (!avr) { fprintf(stderr, "avr_make_mcu_by_name failed\n"); exit(2); }
  avr_init(avr);
  avr->frequency = 16000000;
  avr->log = 0;
  avr_load_firmware(avr, &fw);
  /* simavr's ELF loader ignores a high --section-start, so the boot section is
   * placed here; the app ELF never touches it. */
  memcpy(&avr->flash[BOOT_START], start_boot, BOOT_LEN);
  avr->reset_pc = BOOT_START;  /* BOOTRST: every reset enters the boot section */
  avr->pc = start_at_reset ? BOOT_START : 0;
  return avr;
}

/* Power loss + power-on: RAM content is undefined (filled with a pattern so
 * nothing can lean on leftovers), flash survives, the core resets to BOOTRST. */
static void power_cycle(avr_t* avr) {
  avr_reset(avr);
  for (uint32_t a = 0x100; a <= avr->ramend; a++) avr->data[a] = 0xA5;
  avr->pc = BOOT_START;
}

static int page0_blank(avr_t* avr) {
  for (int i = 0; i < PAGE_SIZE; i++)
    if (avr->flash[BOOT_START + i] != 0xFF) return 0;
  return 1;
}

static int at_spm(avr_t* avr) {
  return avr->flash[avr->pc] == 0xE8 && avr->flash[avr->pc + 1] == 0x95;
}

static int boot_mismatches(avr_t* avr, int verbose) {
  int total = 0;
  for (int p = 0; p < BOOT_LEN / PAGE_SIZE; p++) {
    int bad = 0;
    for (int i = 0; i < PAGE_SIZE; i++)
      if (avr->flash[BOOT_START + p * PAGE_SIZE + i] != expected[p * PAGE_SIZE + i]) bad++;
    if (verbose)
      printf("  page %d (0x%04x): %d/%d match%s\n", p, BOOT_START + p * PAGE_SIZE,
             PAGE_SIZE - bad, PAGE_SIZE, bad ? "  <-- MISMATCH" : "");
    total += bad;
  }
  return total;
}

enum outcome { OUT_OK, OUT_WINDOW, OUT_FAIL };

/* One simulated life: run until `boots` dones, killing/corrupting as asked. */
static enum outcome run_once(long kill_n, int verbose, long* spm_total) {
  avr_t* avr = fresh_avr();
  long spm = 0;
  int dones = 0, killed = 0, ok = 1;
  int last_result = -1, last_state = -1;
  uint64_t ran = 0;
  int was_done = 0;
  long resets = 0;

  while (ran < max_cycles) {
    if (avr->pc == BOOT_START) {
      resets++;
      if (verbose) printf("RESET #%ld at cycle %llu\n", resets, (unsigned long long)avr->cycle);
    }
    if (tw_ram_set && avr->pc == TWIBOOT_HANDLER_ENTRY) {
      avr->data[0x019D] = (uint8_t)tw_cmd;
      avr->data[0x0100] = (uint8_t)tw_flag;
      avr->data[0x0101] = (uint8_t)tw_count;
    }
    if (at_spm(avr)) {
      if (!killed && kill_n >= 0 && spm == kill_n) {
        killed = 1;
        int blank = page0_blank(avr);
        if (verbose) printf("KILL before spm #%ld, page0 %s\n", spm, blank ? "BLANK" : "intact");
        if (blank) { avr_terminate(avr); return OUT_WINDOW; }
        power_cycle(avr);
        was_done = 0;
        continue;
      }
      if (corrupt_at >= 0 && spm == corrupt_at) {
        avr->flash[corrupt_addr] ^= 0xFF;
        if (verbose) printf("CORRUPT flash[0x%04x] before spm #%ld\n", corrupt_addr, spm);
        corrupt_at = -1;  /* once */
      }
      uint32_t pc0 = avr->pc;
      int st = avr_run(avr);
      ran++;
      if (avr->pc != pc0) spm++;
      if (st == cpu_Done || st == cpu_Crashed) break;
    } else {
      int st = avr_run(avr);
      ran++;
      if (st == cpu_Done || st == cpu_Crashed) break;
    }
    int done_now = avr->data[report_a] == SIM_DONE;
    if (done_now && !was_done) {
      last_result = avr->data[report_a + 1];
      last_state = avr->data[report_a + 2];
      if (verbose) printf("DONE #%d result=%d state=%d (spm so far %ld, cycles %llu)\n",
                          dones, last_result, last_state, spm,
                          (unsigned long long)avr->cycle);
      for (int e = 0; e < n_expects; e++) {
        if (expects[e].idx == dones &&
            (expects[e].result != last_result || expects[e].state != last_state)) {
          printf("EXPECT FAILED: done #%d wanted result=%d state=%d\n", dones,
                 expects[e].result, expects[e].state);
          ok = 0;
        }
      }
      dones++;
      if (dones >= boots) break;
      power_cycle(avr);
      done_now = 0;
    }
    was_done = done_now;
  }
  if (spm_total) *spm_total = spm;
  int bad = boot_mismatches(avr, verbose);
  if (verbose)
    printf("ran=%llu dones=%d spm=%ld BOOT SECTION: %d/%d match\n",
           (unsigned long long)ran, dones, spm, BOOT_LEN - bad, BOOT_LEN);
  avr_terminate(avr);
  if (dones < boots) {
    if (verbose) printf("FAIL: only %d of %d boots finished\n", dones, boots);
    return OUT_FAIL;
  }
  if (expect_spm >= 0 && spm != expect_spm) {
    if (verbose) printf("EXPECT FAILED: %ld SPMs executed, wanted %ld\n", spm, expect_spm);
    return OUT_FAIL;
  }
  if (expect_resets >= 0 && resets != expect_resets) {
    if (verbose) printf("EXPECT FAILED: %ld resets, wanted %ld\n", resets, expect_resets);
    return OUT_FAIL;
  }
  if (bad || !ok) return OUT_FAIL;
  return OUT_OK;
}

int main(int argc, char** argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: %s <app.elf> <start_boot.bin> <expected_boot.bin> "
                    "<report_sram_hex> [options]\n", argv[0]);
    return 2;
  }
  memset(&fw, 0, sizeof(fw));
  if (elf_read_firmware(argv[1], &fw) != 0) {
    fprintf(stderr, "elf_read_firmware failed for %s\n", argv[1]);
    return 2;
  }
  load_bin(argv[2], start_boot);
  load_bin(argv[3], expected);
  report_a = (uint32_t)strtoul(argv[4], NULL, 0);
  long sweep_from = -1, sweep_to = -1, expect_windows = -1;
  for (int i = 5; i < argc; i++) {
    const char* a = argv[i];
    if (!strcmp(a, "--start") && i + 1 < argc) {
      start_at_reset = !strcmp(argv[++i], "reset");
    } else if (!strcmp(a, "--boots") && i + 1 < argc) {
      boots = atoi(argv[++i]);
    } else if (!strcmp(a, "--expect") && i + 1 < argc && n_expects < MAX_EXPECT) {
      if (sscanf(argv[++i], "%d:%d:%d", &expects[n_expects].idx,
                 &expects[n_expects].result, &expects[n_expects].state) != 3) {
        fprintf(stderr, "bad --expect\n"); return 2;
      }
      n_expects++;
    } else if (!strcmp(a, "--kill-at-spm") && i + 1 < argc) {
      kill_at = strtol(argv[++i], NULL, 0);
    } else if (!strcmp(a, "--corrupt-at-spm") && i + 2 < argc) {
      corrupt_at = strtol(argv[++i], NULL, 0);
      corrupt_addr = (uint32_t)strtoul(argv[++i], NULL, 0);
    } else if (!strcmp(a, "--sweep-kill") && i + 2 < argc) {
      sweep_from = strtol(argv[++i], NULL, 0);
      sweep_to = strtol(argv[++i], NULL, 0);
    } else if (!strcmp(a, "--expect-windows") && i + 1 < argc) {
      expect_windows = strtol(argv[++i], NULL, 0);
    } else if (!strcmp(a, "--twiboot-ram") && i + 1 < argc) {
      if (sscanf(argv[++i], "%i:%i:%i", &tw_cmd, &tw_flag, &tw_count) != 3) {
        fprintf(stderr, "bad --twiboot-ram\n"); return 2;
      }
      tw_ram_set = 1;
    } else if (!strcmp(a, "--expect-spm") && i + 1 < argc) {
      expect_spm = strtol(argv[++i], NULL, 0);
    } else if (!strcmp(a, "--expect-resets") && i + 1 < argc) {
      expect_resets = strtol(argv[++i], NULL, 0);
    } else if (!strcmp(a, "--max-cycles") && i + 1 < argc) {
      max_cycles = strtoull(argv[++i], NULL, 0);
    } else {
      fprintf(stderr, "unknown/incomplete option %s\n", a);
      return 2;
    }
  }

  if (sweep_from < 0) {
    long spm_total = 0;
    enum outcome o = run_once(kill_at, 1, &spm_total);
    printf("SPM_TOTAL=%ld\n", spm_total);
    if (o == OUT_WINDOW) { printf("RESULT: killed inside a page-0 window\n"); return 3; }
    printf("RESULT: %s\n", o == OUT_OK ? "PASS" : "FAIL");
    return o == OUT_OK ? 0 : 3;
  }

  long n_ok = 0, n_window = 0, n_fail = 0, w_lo = -1, w_hi = -1;
  for (long n = sweep_from; n <= sweep_to; n++) {
    enum outcome o = run_once(n, 0, NULL);
    if (o == OUT_OK) {
      n_ok++;
    } else if (o == OUT_WINDOW) {
      n_window++;
      if (w_lo < 0) w_lo = n;
      if (w_hi >= 0 && n != w_hi + 1) printf("  window points %ld..%ld\n", w_lo, w_hi), w_lo = n;
      w_hi = n;
    } else {
      n_fail++;
      printf("  FAIL at kill #%ld (re-run with --kill-at-spm %ld for detail)\n", n, n);
    }
  }
  if (w_lo >= 0) printf("  window points %ld..%ld\n", w_lo, w_hi);
  printf("SWEEP %ld..%ld: recovered=%ld page0-window=%ld failed=%ld\n", sweep_from,
         sweep_to, n_ok, n_window, n_fail);
  if (n_fail) { printf("RESULT: FAIL\n"); return 3; }
  if (expect_windows >= 0 && n_window != expect_windows) {
    printf("RESULT: FAIL (expected %ld window points, found %ld)\n", expect_windows, n_window);
    return 3;
  }
  printf("RESULT: PASS\n");
  return 0;
}
