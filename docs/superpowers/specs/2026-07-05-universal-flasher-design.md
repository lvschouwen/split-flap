# Universal Flasher — `split-flap-flasher.exe` (issue #124, revised scope)

**Date:** 2026-07-05
**Status:** Design approved in session; Codex cross-model review done (3 High / 3 Medium / 2 Low findings, all verified against the code and folded in below)
**Supersedes:** the two-`.bat` Windows flow and the `flash-display.ps1` plan originally sketched in #124.

## Problem

First-time provisioning of a stock display (N Nanos + 1 ESP-01) currently needs: an Arduino IDE install just to build the ArduinoISP programmer, two stale `.bat` scripts (one still writes the obsolete LittleFS image at `0xBB000` — post-#98 that region is OTA staging headroom), a gitignored `prebuilt/` folder that silently drifts out of date, and a README whose DIP table stops at 10 units. Nothing verifies its work; failures surface as cryptic avrdude/esptool errors.

## Decisions (from brainstorming)

- **Windows-only for now**; core logic stays plain Python so it is pytest-able on Linux CI. ("Universal" = one tool for every flash job, not cross-platform UX.)
- **Single-file exe** (PyInstaller one-file) — the complete package: tool + all firmware + avrdude, one versioned artifact.
- **Unit count is asked at run time (1–16)**, never assumed. Drives the flash loop, DIP table, and final verdict.
- **Programmer bootstrap is in scope**: the exe can turn a factory spare Uno/Nano into an Arduino-as-ISP programmer over plain USB — no Arduino IDE anywhere in the flow.
- **Binaries come only from the build** — a `manifest.json` (git rev, build date, SHA-256 per image) is baked in; the tool refuses to run without a valid manifest.

## What ships

`split-flap-flasher.exe` containing:

| Baked-in asset | Purpose |
| --- | --- |
| `ArduinoISP.hex` (vendored, source noted) | Step-0 programmer prep via the spare board's serial bootloader |
| `twiboot-atmega328p-16mhz.hex` | Per-unit I2C bootloader (vendored patched build) |
| `firmware.bin` (ESPMaster, fresh `pio run`) | Master flash at `0x0` |
| `unit-firmware.hex` (fresh `pio run`) | Reference copy only — master pushes it over I2C itself |
| `avrdude.exe` + `avrdude.conf` (pinned release, checksummed) | ICSP + serial-bootloader AVR writes; GPL compliance: license file + exact upstream version and source-release URL recorded in the manifest and README (source offer) |
| `manifest.json` | git rev, build date, SHA-256 of each asset |

esptool + pyserial are Python dependencies compiled into the exe.

## UX

Menu on launch (build rev + date in the banner):

```
 1. Provision a new display        (guided, start here)
 2. Prepare Arduino-as-ISP programmer
 3. Flash twiboot to a single unit
 4. Flash master firmware (USB serial)
 5. Update master firmware (WiFi/OTA)
 6. Check display status
 7. Wiring help
 0. Exit
```

### Wiring help (option 7, and inline at every hardware step)

The exe carries pin-by-pin ASCII wiring diagrams and shows the relevant one *inside* each wizard step before the "press Enter" — the operator never needs a browser or the repo README at the bench. Diagrams (single source: `wiring.py`, also reused by option 7's standalone browser):

- **Programmer build**: spare Uno/Nano → target Nano ICSP, 6 wires (programmer D10 → target RST, D11 → D11/MOSI, D12 → D12/MISO, D13 → D13/SCK, 5V, GND), plus the 10 µF cap between the *programmer's* RESET and GND (fitted only *after* the ArduinoISP sketch is flashed).
- **ESP-01 ↔ USB-UART**: 3.3 V only (never 5 V), TX↔RX crossed, CH_PD/EN → 3.3 V, GPIO0 → GND for programming mode, and the remove-jumper/power-cycle sequence.
- **DIP switches**: per-unit 4-switch pattern rendered visually (`↑↑·· `-style) for the current unit number.
- **Display assembly**: shared 5 V rail, ESP-01 GPIO1/GPIO3 → SDA/SCL to every Nano, one bus, contiguous DIP addressing from `0x01`.

### Wizard (option 1)

1. **Environment check** — enumerate COM ports via pyserial; zero ports → print CH340/CP210x driver instructions and wait/retry.
2. **Ask unit count (1–16)** — persisted in a session file; interrupted runs resume ("unit 9 of 12"), including per-unit done/skipped state.
3. **Programmer prep** (skippable) — flash `ArduinoISP.hex` to the spare board via avrdude `-c arduino` (115200, fall back to 57600 for old bootloaders), then show the 10 µF reset-cap and 6-wire ICSP hookup.
4. **Per-unit loop** — "Unit 3/12 — set DIP to `0010`, **disconnect the stepper from the unit PCB**, clip ICSP, press Enter." (The stepper shares/loads the SPI-adjacent pins and is a known cause of `0xFFFFFF` signature reads — instruct up front, not as a failure hint.) Order per unit: device-signature probe (catches swapped MOSI/MISO before any write) → twiboot flash → fuse write (LFUSE 0xFF, HFUSE 0xDC, EFUSE 0xFD) → fuse read-back verify → tally. On failure: retry / skip (marked in session) / abort.
5. **Master flash** — guide GPIO0→GND, detect the adapter's COM port appearing (port-set diff), validate the image header first (esp8266 magic `0xE9` + flash-size nibble compatible with the ESP-01 1 MB `esp01_1m` build — refuse anything else so a 4 MB-header build can't ship and trip the OTA flash-config-mismatch path later), then esptool `write_flash 0x0`, `--before no_reset --after no_reset`, then "remove jumper, power-cycle".
6. **Assemble before first boot** — explicit wizard stop: install the master into the display; every Nano DIP-set, powered from the shared rail, and on the I2C bus **before** the ESP's first normal boot, because the master auto-pushes unit firmware during its boot bus scan. Only then: portal instructions (`split-flap-<chipid>-setup`).
7. **Network verify** (optional) — poll `/settings`; compare running git rev to the manifest, and verify **`detectedUnitCount` == the step-2 answer AND `detectedUnitAddresses` == contiguous `0x01..0xN`** (not `unitCount`, which is `displayWidth` = highest responder + 1 and reads 12 even when only units 1 and 12 answer) → "Display alive — 12/12 units, firmware `<rev>` ✓". On mismatch, print which addresses are missing.

Options 2–4 reuse the same building blocks standalone. Option 5 is a Python port of `ota-master.sh` carrying over its **full semantics, not just the happy path**: MD5 + `?v=` stamp, `/settings` polling, the SUCCESS / EBOOT SILENT REVERT / FLASH CONFIG MISMATCH / UPLOAD DID NOT REACH HANDLER / INCONSISTENT verdicts, retry **only** on `reverted` (never on handler/config failures), and the optional quiet-OTA arming (`POST /firmware/ota-mode` before an attempt). Option 6 pretty-prints `/settings`.

## Code layout

`flashing/flasher/` Python package (small focused modules):

- `main.py` — entry point + menu
- `wizard.py` — provisioning flow
- `ports.py` — pyserial enumeration, plug-in watching, driver hints
- `avr.py` — avrdude wrapper: serial-bootloader flash, ICSP signature probe, twiboot flash, fuse write + read-back parse
- `esp.py` — esptool invocation
- `ota.py` — upload + verdict logic
- `assets.py` — resource resolution (`sys._MEIPASS` frozen vs repo paths in dev) + manifest validation
- `session.py` — resumable state JSON
- `wiring.py` — ASCII wiring diagrams + DIP pattern rendering (pure, tested)
- `ui.py` — prompts, colors (Windows ANSI enable)

Dev mode: `python -m flasher` from the repo. The exe is packaging only.

## Build (GitHub Actions)

Job on `windows-latest`, **in this order** (order is load-bearing — `build_assets.py` embeds `ESPMaster/data/unit-firmware.hex` + its `.rev` sidecar into the master's PROGMEM, it does NOT pull the Unit build automatically):

1. `pio run` **Unit** first.
2. Copy `.pio/build/unit/firmware.hex` → `ESPMaster/data/unit-firmware.hex` and write the matching `unit-firmware.rev`.
3. `pio run` **ESPMaster** (now embeds the fresh unit hex).
4. Collect vendored twiboot + ArduinoISP hexes; download pinned avrdude release (checksum verified).
5. Generate manifest; **consistency gate**: the manifest build fails unless the master's embedded `BUNDLED_UNIT_REV` equals the freshly built Unit rev — the drift class that rotted `prebuilt/` cannot recur.
6. PyInstaller one-file → upload artifact; attach to release on tags.

ArduinoISP.hex is compiled once and committed (upstream sketch is frozen); CI never needs Arduino tooling.

## Testing

Pure logic under pytest on Linux CI (`flashing/flasher/tests/`): manifest validation, DIP-pattern rendering, avrdude/esptool argument construction, fuse read-back parsing, OTA verdict decisions, session resume. subprocess/HTTP mocked. Hardware paths verified at the bench.

## Repo cleanup riding along

- Delete `1-flash-unit-bootloader.bat`, `2-flash-master.bat`, `config.bat`, `package-flasher.sh` (CI replaces it; it also still calls the dead `buildfs` target).
- Rewrite `flashing/README.md` around the exe; full 16-row DIP table.
- Fix stale "Phase 2/3 unimplemented" status in `firmware/v1/UnitBootloader/README.md`.
- `ota-master.sh` stays (Linux dev convenience).
- #124 gets a scope-update comment; no new issue.

## Known trade-offs

- Unsigned exe → one-time SmartScreen "Run anyway"; PyInstaller one-file occasionally trips AV heuristics. Acceptable for a personal bench tool.
- Exe must be built on Windows → delegated to CI; not buildable locally on the Linux dev box (dev runs the .py directly).
- New exe per firmware release (assets baked in) — intended: one artifact, zero version skew.
