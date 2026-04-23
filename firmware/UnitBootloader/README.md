# Unit I2C bootloader (twiboot)

This is a vendored + minimally-patched copy of [twiboot](https://github.com/orempel/twiboot) by Olaf Rempel — an I2C-speaking bootloader for AVR MCUs. After this bootloader is installed on each split-flap unit, the master can push new unit firmware over I2C (no more per-unit physical re-flash).

Upstream is licensed GPLv2; see `LICENSE`.

## What's different from upstream

A handful of focused patches on top of stock twiboot. The first two are pure "build for the Nano" plumbing; the rest are the split-flap protocol features.

- **`Makefile`** — default `MCU = atmega328p`, fuses updated for the Nano's 16 MHz external crystal (`LFUSE=0xFF`, `HFUSE=0xDA`, `EFUSE=0xFD`), default programmer set to `-c usbasp`. `HFUSE=0xDA` gives us a 2 KB bootloader section; the stock twiboot Makefile and earlier revisions of this file used a 1 KB BLS (`0xDC`).
- **`main.c` — `F_CPU` / `TIMER_IRQFREQ_MS` guards** — `#ifndef`-wrapped so the Makefile can set them per-MCU. At 16 MHz the default `TIMER_IRQFREQ_MS=25` overflows the 8-bit timer preload; the Makefile overrides to `16` so timing math stays correct and the boot-window timeout remains ~1 s.
- **`main.c` — EEPROM-first address resolution** (issue #72): at init, twiboot reads EEPROM slot 2 (identity magic) and slot 3 (address). If magic equals `0xA5` (the same `EEPROM_ID_MAGIC_VALUE` the sketch uses) and the address is in `1..126`, twiboot listens there. Otherwise it falls back to the DIP-derived `I2C_ADDRESS_BASE + dipValue` (pins PD3..PD6). This keeps the bootloader and the sketch in perfect agreement about where the unit lives on the bus, so I2C OTA keeps working even after the provisioning wizard assigns an EEPROM identity.
- **`main.c` — TWAMR dual-match with `0x29`** (issue #72): `TWAMR = (resolved_address XOR 0x29) << 1` configures the TWI mask register so the hardware matches BOTH the unit's resolved address AND the `0x29` broadcast-recovery address. A bricked unit (scrambled identity, wrong DIPs, whatever) can always be reached via `0x29`. On a lone-master bus with unit addresses in `0x01..0x10`, the handful of spurious mask matches (e.g. `0x21`, `0x39`) are inert — the master never addresses them.
- **`main.c` — `do_spm` self-update hook** (issue #72): exported at a fixed address (`0x7F80` on ATmega328P with 2 KB BLS) via the `.hook` linker section. Future app-side "bootloader-updater sketches" can call this function via a function pointer to patch the BLS over OTA. AVR hardware restricts `SPM` to code executing inside the BLS (§28.4) — the hook runs inside the BLS, so it can legally flash the BLS pages. See [§`do_spm` self-update hook](#do_spm-self-update-hook) below.

Protocol + flash-write machinery + reset behaviour otherwise stock.

## Protocol summary

- Bootloader listens on I2C address **`resolved_address`** (EEPROM-first, DIP-fallback — see "What's different from upstream" above) AND on **`0x29`** broadcast-recovery simultaneously, for the entire boot window.
- On reset, the boot window stays open for ~1 s. Any I2C activity at either address keeps it open; otherwise the bootloader jumps to the application sketch.
- The master sends commands in twiboot's chunked page-write protocol. See upstream README (`UPSTREAM_README.md`) for the byte-level details.
- The bootloader protects itself from the normal flash-write path — `write_flash_page()` at `main.c:253` refuses any write at or above `BOOTLOADER_START`. To update the bootloader itself, use the `do_spm` hook (see below).
- A failed mid-write during an app flash leaves the unit in bootloader mode on next reset — retry until the upload succeeds.

## Entering the bootloader from the running sketch

Each unit has exactly one normal sketch-side address (EEPROM identity if set, else DIP-derived). The bootloader also listens on that address plus `0x29`. The master's flash procedure is:

1. Send `CMD_ENTER_BOOTLOADER` (`0x80`) to **the unit's normal address**. `Unit.ino`'s `receiveLetter()` handler sets the watchdog and spins until it fires.
2. Unit reboots into twiboot. Twiboot resolves its listen address the same way the sketch did, so it comes up on the same address the master was just talking to.
3. Master streams the new `.hex` page-by-page to that address.
4. When done, bootloader exits via `CMD_SWITCH_APPLICATION` → sketch is running again at the same address. Master resumes normal operation.

For brick recovery, the master can substitute `0x29` for the unit address in steps 3-4 if the normal address is unreachable.

## `do_spm` self-update hook

Stock twiboot cannot rewrite its own BLS pages (by design — protects the bootloader from self-destruction during a failed flash). To keep future bootloader revisions OTA-deliverable anyway, we export a minimal `do_spm(uint16_t addr, uint8_t cmd, uint16_t data)` function at a linker-pinned address. A future "bootloader-updater" sketch can load itself as the normal application over I2C OTA, then call `do_spm` via a function pointer to write new BLS pages from BLS context.

**Pinned address on ATmega328P (2 KB BLS):** `0x7F80` (byte address) = `0x3FC0` (AVR function-pointer word address). The Makefile variable `HOOK_START` controls this.

**App-side usage:**

```c
typedef void (*do_spm_t)(uint16_t addr, uint8_t cmd, uint16_t data);
do_spm_t do_spm = (do_spm_t)(0x7F80 / 2);  /* AVR fn ptr = word addr */

/* Erase + rewrite one page (atomic sequence) */
cli();
do_spm(page_addr,  (1 << PGERS) | (1 << SPMEN), 0);              /* erase   */
for (uint8_t i = 0; i < SPM_PAGESIZE; i += 2) {
    uint16_t w = ((uint16_t)new_bytes[i + 1] << 8) | new_bytes[i];
    do_spm(page_addr + i, (1 << SPMEN), w);                      /* fill    */
}
do_spm(page_addr,  (1 << PGWRT)  | (1 << SPMEN), 0);             /* commit  */
do_spm(0,          (1 << RWWSRE) | (1 << SPMEN), 0);             /* re-RWW  */
sei();
```

**Safety:** The hook has no validation — it trusts the caller. A partial BLS write will brick the unit (no bootloader = no way back in without ICSP). The updater sketch MUST disable the watchdog, disable interrupts during the erase/fill/write sequence, and verify each page after writing. Ship the updater sketch only after thorough testing on a dev unit; that one sacrifice is worth saving ICSP on all the others.

**Not in this PR:** no updater sketch exists today. This PR only exports the hook so it's available the first time we actually need to patch the bootloader. Writing the updater is [future work](https://github.com/lvschouwen/split-flap/issues/72).

## Building

Requires an AVR toolchain (`avr-gcc`, `avr-objcopy`, etc.). PlatformIO bundles these — add its binaries to your `PATH` and run `make`:

```bash
export PATH=$HOME/.platformio/packages/toolchain-atmelavr/bin:$PATH
make              # produces twiboot.hex
make clean
```

Expected size: ~1 KB (`.text` ~920 bytes + `.hook` ~62 bytes + `.data` ~26 bytes), comfortably under the 2048-byte (1024-word) bootloader section configured by `HFUSE=0xDA`. The 2 KB BLS replaces the old 1 KB setup (`HFUSE=0xDC`) as of #72 — we needed the extra room for EEPROM-first address resolution, TWAMR dual-match with `0x29`, and the `do_spm` self-update hook described below.

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

This writes `twiboot.hex` to flash and sets `LFUSE=0xFF`, `HFUSE=0xDA`, `EFUSE=0xFD`. `HFUSE=0xDA` sets the BOOTRST bit (so the Nano powers up into the bootloader instead of the sketch) and `BOOTSZ=01` (2 KB BLS). If you're coming from an earlier install that used `HFUSE=0xDC` (1 KB BLS), re-running `make fuses` updates the fuse in place — you don't need to erase the chip.

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
avrdude -c usbasp -p m328p -U lfuse:w:0xff:m -U hfuse:w:0xda:m -U efuse:w:0xfd:m
```

### Windows

You're flashing a bootloader, so it has to go through the Nano's ICSP header — the USB-serial port on the Nano itself only lets you flash *sketches* via the existing optiboot. You still need a USBasp or a second Arduino running Arduino-as-ISP (same as on Linux/macOS). Native Windows works fine; there's no reason to use WSL2 for this (WSL2 can't see USB devices without `usbipd-win`, which is extra setup for a one-off task).

**USBasp on Windows:**

1. Install the libusb driver for the USBasp using [Zadig](https://zadig.akeo.ie/): plug the USBasp in, open Zadig, **Options → List All Devices**, select the USBasp, install the `libusb-win32` driver. One-time.
2. Get `avrdude.exe`. Either grab an official [avrdude Windows release](https://github.com/avrdudes/avrdude/releases) or reuse Arduino IDE's bundled copy under `%LocalAppData%\Arduino15\packages\arduino\tools\avrdude\<version>\bin\avrdude.exe`.
3. From the repo root (cmd or PowerShell):

```
avrdude -c usbasp -p m328p -U flash:w:UnitBootloader\prebuilt\twiboot-atmega328p-16mhz.hex
avrdude -c usbasp -p m328p -U lfuse:w:0xff:m -U hfuse:w:0xda:m -U efuse:w:0xfd:m
```

**Arduino-as-ISP on Windows:**

Prep the programmer exactly as in [Option 2 above](#option-2-arduino-as-isp): upload the `ArduinoISP` sketch, add the 10 µF reset-disable cap, wire programmer-to-target per the pin table. Then find the programmer's COM port in Device Manager and run (substituting your port):

```
avrdude -c stk500v1 -P COM4 -b 19200 -p m328p -U flash:w:UnitBootloader\prebuilt\twiboot-atmega328p-16mhz.hex
avrdude -c stk500v1 -P COM4 -b 19200 -p m328p -U lfuse:w:0xff:m -U hfuse:w:0xda:m -U efuse:w:0xfd:m
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

Once you see the bootloader respond (at the DIP- or EEPROM-derived address, and/or at `0x29`), you're good to let the master push the unit sketch over I2C on the next boot.

## Current status vs [issue #10](https://github.com/lvschouwen/split-flap/issues/10) + [#72](https://github.com/lvschouwen/split-flap/issues/72)

Issue #10 is closed — core OTA path shipped in [PR #15](https://github.com/lvschouwen/split-flap/pull/15) (commit `940dbd3`). Issue #72 picks up where #10 Phase 5 left off and adds the self-update hook.

- [x] Phase 0 — design in the issue body.
- [x] Phase 1 — this directory. Vendored twiboot, patched for 16 MHz Nano, `.hex` builds and is checked in. Manual install procedure documented.
- [x] Phase 2 — sketch-side: DIP-derived address in the bootloader + jump-to-bootloader I2C opcode in `Unit.ino` (watchdog-reset handshake).
- [x] Phase 3 — master-side: twiboot protocol client in `ESPMaster/ServiceFirmwareFunctions.ino` (CMD_WAIT ping, page-write, CMD_SWITCH_APPLICATION, chip-ID verify) + `POST /firmware/unit?address=<hex>` upload endpoint + auto-install-from-LittleFS on boot.
- [x] Phase 4 — web UI: Firmware card on the main page with target dropdown, `.hex` picker, confirmation dialog, and live progress via the existing `/log` tail.
- [x] Phase 5 — EEPROM-first address resolution, TWAMR dual-match with `0x29`, `do_spm` self-update hook (#72). Unit side (`CMD_SET_I2C_ADDRESS` / `CMD_CLEAR_I2C_ADDRESS` / `CMD_IDENTIFY`) shipped with #56 partial ship. Master-side wizard UI in progress (#72 PR 4).
