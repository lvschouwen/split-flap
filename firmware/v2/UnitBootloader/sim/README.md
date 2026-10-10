# In-system twiboot update — simavr proof (#499)

Proves the in-system twiboot update against the **real committed images**
(fielded `../prebuilt/twiboot-atmega328p-16mhz.hex`, CRC `18173add`, and the
new image built by `../make_new_twiboot.py`) by running the unit firmware's own
self-program core, `firmware/v2/Unit/BootUpdateAvr.h`, compiled unchanged into
`updater_app.cpp`. Scaffolding, not a CI test (CI has no simavr); the static
facts it relies on are CI-gated by `tests/test_twiboot_entry_points.py` and
`tests/test_new_twiboot_image.py`.

## Run

```bash
sudo apt install -y libsimavr-dev        # C API (the gdb stub is unusable here)
./prove.sh                               # avr-gcc from ~/.platformio is put on PATH
```

Prints `ALL PROOFS PASSED`, or `PROOF FAILED: ...` and exits non-zero. ~40 s.

## What it proves

`updater_app.cpp` does what the sketch does at boot (`bootAutoResume()`), then
stands in for the master: state Old → stage 1, Page7Installed or PrevNew → stage 2.
`runtest.c` models BOOTRST (every reset enters 0x7C00), counts every executed
`spm` (twiboot's in stage 1, `do_spm`'s in stage 2), and can power-cycle or
corrupt flash at any SPM index. Stage 1 = 67 flash operations, stage 2 = 8 page
writes × 67 = 536.

| Step | Scenario | Must hold |
|---|---|---|
| A1–A3 | whole update from power-on | ends byte-identical to the new image; stage 1 regains control by twiboot's own jump-to-app (cmd byte 0x21 in overlaid RAM) or by the watchdog reset; an armed twiboot countdown cannot exit (stage 1 zeroes `r10`, the command its expiry stores) |
| B | power loss before each of the 603 flash operations + reset | every point recovers to the new image by itself, except exactly 130 points where page 0 is erased and not yet rewritten — the trampoline write and the final page-0 write, 65 each |
| C | a page-3 write that does not verify | stage 2 stops with the trampoline in page 0; the next boot resumes to New |
| D | a final page-0 write that does not verify | the trampoline is written back; the next boot resumes to New |
| H | a trampoline write that does not verify | retried in the same run, ends New |
| E | trampoline but no `do_spm` in page 7 | refused with zero SPMs |
| F | resume with trampoline + pages 1–6 already done | writes only page 0 (67 SPMs) |
| G | unit already on the new image | untouched |
| P1 | a unit on the previous image (`prebuilt/twiboot-prev-e422a668.hex`, what the fleet carries) | stage 2 alone reaches New: 536 operations, no stage 1, no reset |
| P2 | power loss before each of those 536 operations | all recover through the trampoline, except the same two page-0 windows |

Each guard was proven by breaking it in a scratch copy of the header: dropping
the page-7 check fails E, always rewriting the trampoline fails F, dropping
per-page verify fails C, dropping the trampoline restore fails D, dropping the page-0 retry fails H, and writing
the real page 0 first (no trampoline) makes every B kill point inside pages 1–6
fail.

## Stopped flash (#554)

`./prove-torn-flash.sh` (needs the Unit firmware built) runs what a flash that
stopped short leaves behind: the hold page at address 0
(`twibootFillHoldPage`, built from `shared/TwibootFlash.h` by `hold_page.cpp`)
under the units' bootloader. With the rest of the unit image above it, and with
blank flash above it, the program is reset by its watchdog 16 ms after each
start and the bootloader keeps the unit at the third reset, for longer than
`SF_PIN_TIMEOUT_MS`; the program counter never leaves the hold code. The whole
image, as the control, starts and stays up. Prints `STOPPED FLASH PROOF PASSED`.

## Not modelled

- SPM duration: simavr executes an erase/write atomically. A real power loss
  during an erase or write leaves that one page undefined — for pages 1–7 the
  same recovery as the neighbouring kill points applies; for page 0 it is inside
  the windows above.
- Lock bits (the app passes an unlocked byte), brown-out, and other ISRs
  (Timer0 runs, as under Arduino's `init()`, but no ISR is attached).

## Harness facts

- simavr's **gdb stub is unusable** here (ignores hardware breakpoints and
  watchpoints, single-steps 32-bit instructions one word at a time). Use the C
  API and read `avr->flash[]` / `avr->data[]` directly.
- simavr's ELF loader **ignores a high `--section-start`** and loads `.text` at
  `0`; `runtest` places the boot-section binary at `0x7C00` itself.
- twiboot write-handler entry past the boot-section guard: `0x7e5a`. Loop state:
  `buf` at SRAM `0x011D`, fill-loop end pointer `r14:r15 = 0x019D`, SPMCSR
  constants `r9=0x03`/`r16=0x01`/`r13=0x05`/`r12=0x11`, pagestart in `r24:r25`.
  Its idle loop reads its command byte at `0x019D` and timeout state at
  `0x0100`/`0x0101` — application RAM while stage 1 runs.
- `keystone.c` is the earlier minimal proof that a borrowed `spm` returns
  control at all; it is not part of `prove.sh`.

## Replacing the image again

A unit can only be updated from an image the classifier names. Before the
current image is replaced, move its CRC to `BOOT_PREV_NEW_CRC32`
(`shared/BootSectionClassify.h`), commit its bytes as a `prebuilt/` hex and
point case P at it — and keep page 7 byte-identical, or stage 2 alone is no
longer a valid path (`tests/test_new_twiboot_image.py` pins both).
