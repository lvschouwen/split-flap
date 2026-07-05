# Split-Flap — flashing

Everything flash-related now lives in **one tool**: `split-flap-flasher.exe`.
Download it from the latest GitHub release (or the `split-flap-flasher`
artifact of the *Flasher exe* workflow), double-click, and follow the menu.
Windows SmartScreen will warn once (unsigned exe) — "More info → Run anyway".

The exe contains the wizard, esptool, avrdude, and **the firmware images it
flashes** (built fresh by CI from the same commit — see `manifest.json`
baked inside). There is nothing else to download and no stale-binaries drift.

## What it does

1. **Provision a new display** — guided cold start: asks how many units
   (1–16), turns a spare Uno/Nano into an Arduino-as-ISP programmer (no
   Arduino IDE needed), flashes twiboot to every Nano with signature +
   fuse verification, flashes the ESP-01 master, then walks assembly,
   WiFi setup, and a live network verification. Interrupted runs resume.
2. **Prepare programmer / single unit / master serial** — the same steps
   standalone, for redoing one piece.
3. **Update master (WiFi/OTA)** — upload with MD5 + verdict polling
   (same semantics as `ota-master.sh`).
4. **Check display status** — `/settings` pretty-print + unit-count verify.
5. **Wiring help** — every connection diagram (ICSP, ESP-01 UART, DIP,
   display assembly), also shown inline at each step.

Prerequisite on the bench machine: **nothing** (the exe is self-contained).
If no COM port appears when you plug something in, install the CH340 driver
(clone Nanos/adapters) — the tool detects this and shows instructions.

## Only one ICSP flash per Nano — twiboot only

The Unit sketch is bundled inside the master firmware (PROGMEM) and pushed
to each Nano over I2C automatically the first time the master sees it in
bootloader mode. Unit firmware updates ride along with master OTA updates.

## DIP switch addresses

`1` = switch up. The unit firmware offsets the DIP value by 1 for I2C
(address 0x00 is reserved), so DIP 0000 → I2C 0x01. Units must be addressed
contiguously from unit 1.

| Unit | DIP  | I2C  | Unit | DIP  | I2C  |
| ---- | ---- | ---- | ---- | ---- | ---- |
| 1    | 0000 | 0x01 | 9    | 1000 | 0x09 |
| 2    | 0001 | 0x02 | 10   | 1001 | 0x0A |
| 3    | 0010 | 0x03 | 11   | 1010 | 0x0B |
| 4    | 0011 | 0x04 | 12   | 1011 | 0x0C |
| 5    | 0100 | 0x05 | 13   | 1100 | 0x0D |
| 6    | 0101 | 0x06 | 14   | 1101 | 0x0E |
| 7    | 0110 | 0x07 | 15   | 1110 | 0x0F |
| 8    | 0111 | 0x08 | 16   | 1111 | 0x10 |

## Developing the flasher

The tool is a plain Python package (`flasher/`). Dev loop on any OS:

    cd flashing
    pip install pyserial esptool pytest
    python -m pytest flasher/tests/ -v      # pure-logic tests, no hardware
    # stage real firmware assets for a live run:
    (cd ../firmware/v1/Unit && pio run)
    python flasher/make_manifest.py stage
    (cd ../firmware/v1/ESPMaster && pio run)
    python flasher/make_manifest.py collect
    python -m flasher

The exe is built by `.github/workflows/flasher.yml` (windows-latest):
Unit build → stage hex+rev into ESPMaster/data → ESPMaster build →
manifest + consistency gate → PyInstaller. The gate fails the build if the
master's embedded unit firmware doesn't match the freshly built Unit hex.

`make_manifest.py stage` writes into the **committed**
`firmware/v1/ESPMaster/data/unit-firmware.hex` and `.rev` files — after
running the dev loop above, `git diff` will show those two files changed.
That's expected: commit the refreshed hex/rev alongside your `Unit/` code
change so the master keeps embedding the matching unit firmware.

**Release note:** `.github/workflows/flasher.yml` also triggers on `v*` tag
pushes, and its release-attach step (`gh release upload`) needs the GitHub
Release to already exist — `gh release upload` 404s against a tag with no
release. When cutting a release, create the GitHub Release **first** (or
immediately after pushing the tag), not after. If the workflow's attach step
already failed because the release didn't exist yet, just re-run it via
`workflow_dispatch` ("Flasher exe" workflow) once the release is created —
no need to re-tag.

`ota-master.sh` remains for Linux-side OTA from a dev checkout:
`./ota-master.sh <fw.bin> http://host:port`.
