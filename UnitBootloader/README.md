# Unit I2C bootloader (twiboot)

This is a vendored + minimally-patched copy of [twiboot](https://github.com/orempel/twiboot) by Olaf Rempel — an I2C-speaking bootloader for AVR MCUs. After this bootloader is installed on each split-flap unit, the master can push new unit firmware over I2C (no more per-unit physical re-flash).

Upstream is licensed GPLv2; see `LICENSE`.

## What's different from upstream

Two small patches on top of stock twiboot — no behavior change other than making it build for the Arduino Nano out of the box:

- **`Makefile`** — default `MCU = atmega328p`, fuses updated for Nano's 16 MHz external crystal (`LFUSE=0xFF`), default programmer set to `-c usbasp`.
- **`main.c`** — `F_CPU` and `TIMER_IRQFREQ_MS` are now `#ifndef`-guarded so the Makefile can set them per-MCU. At 16 MHz the default `TIMER_IRQFREQ_MS=25` overflows the 8-bit timer preload; the Makefile overrides to `16` so timing math stays correct and the boot-window timeout remains ~1 s.

Everything else — the I2C protocol, flash-write machinery, reset behavior — is stock twiboot.

## Protocol summary

- Bootloader listens on I2C address **`0x29`** while its boot window is open.
- On reset, the boot window stays open for ~1 s. Any I2C activity at `0x29` keeps it open; otherwise the bootloader jumps to the application sketch.
- The master sends commands in twiboot's chunked page-write protocol. See upstream README (`UPSTREAM_README.md`) for the byte-level details.
- The bootloader **does not erase itself**. A failed mid-write leaves the unit in bootloader mode on next reset — retry until the upload succeeds.

## Entering the bootloader from the running sketch

Because every unit listens on `0x29` when in bootloader mode, and only one unit at a time should be in that state (otherwise writes collide), the sketch has the authority to reboot into the bootloader when the master tells it to. The master's flash procedure is:

1. Send an "enter bootloader" I2C opcode to **the target unit's normal address** (`I2C_ADDRESS_BASE + dipValue`).
2. The sketch sets a magic byte in EEPROM, enables the watchdog, spins until it fires.
3. Unit reboots into twiboot, which listens on `0x29`.
4. Master streams the new `.hex` to `0x29`.
5. When done, bootloader hands off to the new sketch; master goes back to normal operation.

The sketch-side opcode and EEPROM magic-byte handshake are **not yet implemented** — that's [issue #10](https://github.com/lvschouwen/split-flap/issues/10) Phase 2.

## Building

Requires an AVR toolchain (`avr-gcc`, `avr-objcopy`, etc.). PlatformIO bundles these — add its binaries to your `PATH` and run `make`:

```bash
export PATH=$HOME/.platformio/packages/toolchain-atmelavr/bin:$PATH
make              # produces twiboot.hex
make clean
```

Expected size: ~930 bytes, comfortably under the 1024-byte (512-word) bootloader section configured by `HFUSE=0xDC`.

A prebuilt `.hex` is checked in at `prebuilt/twiboot-atmega328p-16mhz.hex` so you don't strictly need a toolchain.

## Installing the bootloader

**One-time physical flash per unit.** Once it's installed, future unit firmware updates can go over I2C (from the master's web UI).

### Option 1: USBasp (or any avrdude-compatible programmer)

Connect the programmer to the Nano's ICSP header (6-pin) and run:

```bash
# From UnitBootloader/ with the AVR toolchain on PATH:
make install
make fuses
```

This writes `twiboot.hex` to flash and sets `LFUSE=0xFF`, `HFUSE=0xDC`, `EFUSE=0xFD`. `HFUSE=0xDC` sets the BOOTRST bit so the Nano powers up into the bootloader instead of the sketch.

### Option 2: Arduino-as-ISP

Use a spare Arduino Uno/Nano as the programmer:

```bash
AVRDUDE_PROG="-c stk500v1 -P /dev/ttyACM0 -b 19200" make install
AVRDUDE_PROG="-c stk500v1 -P /dev/ttyACM0 -b 19200" make fuses
```

### Option 3: Flash the prebuilt `.hex` directly

Without cloning the whole repo:

```bash
avrdude -c usbasp -p m328p -U flash:w:prebuilt/twiboot-atmega328p-16mhz.hex
avrdude -c usbasp -p m328p -U lfuse:w:0xff:m -U hfuse:w:0xdc:m -U efuse:w:0xfd:m
```

## Verifying the install

After flashing the bootloader but before flashing the split-flap unit sketch, power-cycle the Nano. You should see the status LED behave according to twiboot's boot-window pattern (LED on during the ~1 s boot window, off when jumping to — non-existent, for now — sketch).

A quick-and-dirty I2C sanity check (requires an I2C master on the bus):

```bash
i2cdetect -y 1     # should show a device at 0x29 during the boot window
```

Once you see `0x29` respond, you're good to proceed to Phase 2 (sketch-side handshake) and Phase 3 (master-side flash client).

## Current status vs [issue #10](https://github.com/lvschouwen/split-flap/issues/10)

- [x] Phase 0 — design in the issue body.
- [x] **Phase 1 — this directory. Vendored twiboot, patched for 16 MHz Nano, `.hex` builds and is checked in. Manual install procedure documented.**
- [ ] Phase 2 — sketch-side: EEPROM identity + DIP fallback, jump-to-bootloader I2C opcode.
- [ ] Phase 3 — master-side: twiboot protocol client + firmware-upload endpoint.
- [ ] Phase 4 — UI page with progress bar.
- [ ] Phase 5 — EEPROM layout migration (avoid clash with calibration-offset slot).
