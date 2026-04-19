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

Use a spare Arduino Uno/Nano as the programmer. Three steps before avrdude will work:

**1. Upload the `ArduinoISP` sketch to the spare Arduino.** It ships with the Arduino IDE — **File → Examples → 11.ArduinoISP → ArduinoISP** — upload it normally via the spare's USB port. LED 9 should pulse (heartbeat) once it's running.

**2. Add a 10 µF reset-disable capacitor.** On Uno/Nano, opening the USB-serial port auto-resets the board — right when avrdude needs the programmer to stay running. Prevent this with an electrolytic capacitor between **RESET and GND of the programmer** (not the target): `+` leg to RESET, `-` leg to GND. Install the cap *after* uploading ArduinoISP. Without it, avrdude usually fails on the first command with `not in sync`.

**3. Wire the programmer to the target Nano's ICSP header:**

| Programmer pin | Target pin |
| -------------- | ---------- |
| D11 (MOSI)     | MOSI       |
| D12 (MISO)     | MISO       |
| D13 (SCK)      | SCK        |
| D10            | RESET      |
| GND            | GND        |
| 5V             | VCC        |

The target's MOSI/MISO/SCK are available on the 6-pin ICSP header *or* on D11/D12/D13. Power the programmer via its own USB; the target draws 5 V through the wiring.

**4. Run avrdude** (substitute your programmer's serial port):

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

### Windows

You're flashing a bootloader, so it has to go through the Nano's ICSP header — the USB-serial port on the Nano itself only lets you flash *sketches* via the existing optiboot. You still need a USBasp or a second Arduino running Arduino-as-ISP (same as on Linux/macOS). Native Windows works fine; there's no reason to use WSL2 for this (WSL2 can't see USB devices without `usbipd-win`, which is extra setup for a one-off task).

**USBasp on Windows:**

1. Install the libusb driver for the USBasp using [Zadig](https://zadig.akeo.ie/): plug the USBasp in, open Zadig, **Options → List All Devices**, select the USBasp, install the `libusb-win32` driver. One-time.
2. Get `avrdude.exe`. Either grab an official [avrdude Windows release](https://github.com/avrdudes/avrdude/releases) or reuse Arduino IDE's bundled copy under `%LocalAppData%\Arduino15\packages\arduino\tools\avrdude\<version>\bin\avrdude.exe`.
3. From the repo root (cmd or PowerShell):

```
avrdude -c usbasp -p m328p -U flash:w:UnitBootloader\prebuilt\twiboot-atmega328p-16mhz.hex
avrdude -c usbasp -p m328p -U lfuse:w:0xff:m -U hfuse:w:0xdc:m -U efuse:w:0xfd:m
```

**Arduino-as-ISP on Windows:**

Prep the programmer exactly as in [Option 2 above](#option-2-arduino-as-isp): upload the `ArduinoISP` sketch, add the 10 µF reset-disable cap, wire programmer-to-target per the pin table. Then find the programmer's COM port in Device Manager and run (substituting your port):

```
avrdude -c stk500v1 -P COM4 -b 19200 -p m328p -U flash:w:UnitBootloader\prebuilt\twiboot-atmega328p-16mhz.hex
avrdude -c stk500v1 -P COM4 -b 19200 -p m328p -U lfuse:w:0xff:m -U hfuse:w:0xdc:m -U efuse:w:0xfd:m
```

**WSL2 alternative (optional):**

If you'd rather use the Linux toolchain, install `usbipd-win` and forward the USB device into WSL:

```
winget install usbipd
usbipd list
usbipd bind --busid X-Y
usbipd attach --wsl --busid X-Y
```

Then use the Linux `make install` / `make fuses` flow from inside WSL. Works, but more moving parts than just running avrdude on Windows.

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
