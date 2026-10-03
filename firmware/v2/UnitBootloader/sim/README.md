# In-system twiboot update — simavr feasibility harness (#499)

Proof-of-concept harness that validated the #499 mechanism (replace twiboot on a
fielded unit over I2C, no ICSP) against the **real committed image**
(`../prebuilt/twiboot-atmega328p-16mhz.hex`, CRC `18173add`) before any unit
firmware was written. This is scaffolding, not shipped firmware or a CI test —
it documents and reproduces the proof.

## Why it exists

A unit can only run `spm` (flash self-program) from its boot section, and the
only code there is twiboot — which refuses to write its own boot section. The
question was whether the running sketch can nonetheless rewrite twiboot. It can,
in two stages, both proven here in simavr:

- **Stage 1** — get a small callable `do_spm` into twiboot's one empty page
  (page 7, `0x7F80`). The sketch seeds twiboot's own page buffer and loop state
  in SRAM and jumps into twiboot's write handler *just past* its boot-section
  guard (`0x7e5a`), so twiboot erases+fills+writes page 7 itself, then drops
  into its idle loop; a relaxed timer IRQ returns control. `drivefill.c`.
- **Stage 2** — the sketch simply `call`s `do_spm` in page 7 to rewrite
  twiboot's pages 0–6. Clean `ret`-based flow, no tricks. `do_spm.S` + `stage2.c`.

`keystone.c` is the earlier minimal proof that a borrowed `spm` returns control
at all. The static assumptions (CRC, `spm` sites, empty page 7) are gated by
`tests/test_twiboot_entry_points.py` in CI.

## Prerequisites

```bash
sudo apt install -y libsimavr-dev        # C API (the gdb stub is unusable here)
export PATH=$HOME/.platformio/packages/toolchain-atmelavr/bin:$PATH   # avr-gcc
```

## Run the proofs

```bash
gcc runtest.c -o runtest -lsimavr

# Extract the fielded twiboot as a raw boot-section binary (1 KB at 0x7C00):
avr-objcopy -I ihex -O binary ../prebuilt/twiboot-atmega328p-16mhz.hex tw.bin

# Stage 1: drive twiboot's loop to write page 7 (expect PAGE7 128/128):
avr-gcc -Os -mmcu=atmega328p -o drivefill.elf drivefill.c
MAIN=$(avr-nm drivefill.elf | awk '/ main$/{print "0x"$1}')
DONE=$(avr-nm drivefill.elf | awk '/ done$/{print "0x"substr($1,5)}')
./runtest drivefill.elf tw.bin $MAIN 2000000 $DONE $DONE $DONE

# Stage 2: call do_spm@0x7F80 to rewrite page 6 (expect PAGE6 128/128):
avr-gcc -mmcu=atmega328p -nostartfiles -Wl,--section-start=.text=0x7f80 -o do_spm.elf do_spm.S
avr-objcopy -O binary -j .text do_spm.elf do_spm.bin
python3 -c "img=bytearray(b'\xff'*1024); d=open('do_spm.bin','rb').read(); img[0x380:0x380+len(d)]=d; open('boot_do_spm.bin','wb').write(img)"
avr-gcc -Os -mmcu=atmega328p -o stage2.elf stage2.c
MAIN=$(avr-nm stage2.elf | awk '/ main$/{print "0x"$1}'); DONE=$(avr-nm stage2.elf | awk '/ done$/{print "0x"substr($1,5)}')
./runtest stage2.elf boot_do_spm.bin $MAIN 2000000 $DONE $DONE $DONE
```

## Key facts the harness nailed down

- simavr's **gdb stub is unusable** here (ignores hardware breakpoints and
  watchpoints, single-steps 32-bit instructions one word at a time). Use the C
  API and read `avr->flash[]` / `avr->data[]` directly.
- simavr's ELF loader **ignores a high `--section-start`** and loads `.text` at
  `0`; `runtest` overlays the boot-section binary at `0x7C00` manually.
- simavr does **not** model the multi-ms NRWW erase/write halt, so the exact
  stage-1 return-timer value cannot be sim-validated — on real silicon use a
  generous margin (~50 ms, past the ~8 ms of erase+write) while twiboot idles.
  The control *flow* is what the sim proves.
- twiboot write-handler entry past the boot-section guard: `0x7e5a`. Loop state:
  `buf` at SRAM `0x011D`, fill-loop end pointer `r14:r15 = 0x019D`, SPMCSR
  constants `r9=0x03`/`r16=0x01`/`r13=0x05`/`r12=0x11`, pagestart in `r24:r25`.
