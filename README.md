# Split-Flap

[![Build](https://github.com/lvschouwen/split-flap/actions/workflows/build.yml/badge.svg)](https://github.com/lvschouwen/split-flap/actions/workflows/build.yml)

![Split Flap Display](./Images/Split-Flap.jpg)

---
**NOTE:** I will make my best attempt to provide answers where possible in issues and try to keep things up to date where possible dependency wise, however I do not have the time/capacity to work on this project on a consistent basis. Answers and updates may take time.

My personal Split-Flap has been working since I first published this repository and continues to work to this day happily. Several people have also successfully been in touch with their working creations of this Split-Flap which I am very happy it has worked for :) 

I hope this project continues to work for any whom wish to embark on it but be aware, it is a lot of work to complete.

Thank you everyone whom has contributed, included in the "main" release or not, it is very appreciated. I hope to keep the project going in future.

<3

---

This project has been forked from the brilliant [Split Flap Project](https://github.com/Dave19171/split-flap) by [David Königsmann](https://github.com/Dave19171). None of this would have been possible without the great foundations that have been put in place.

3D-files here on [Printables](https://www.prusaprinters.org/prints/69464-split-flap-display)!

---

## What this is

A mechanical split-flap display driven by an **ESP32-S3 master** that talks over I2C to a chain of **Arduino-Nano flap units** (one Nano per flap drum). The master hosts the web UI, WiFi portal, NTP clock, MQTT/Home-Assistant integration, and pushes both master and unit firmware over the air.

Multiple masters can be joined into a **multi-display cluster** — a wall of N rows driven as one logical display over your LAN, with automatic leader/follower coordination and firmware convergence across the fleet.

> **Release:** the current stack is **v2** (`v2.0.0`, the full port of the original ESP8266 firmware onto the ESP32-S3 plus the cluster feature). See the [releases page](https://github.com/lvschouwen/split-flap/releases) for notes and firmware binaries.

## Firmware layout

Everything is built with [PlatformIO](https://platformio.org/) — no Arduino IDE. Each project has its own `platformio.ini`; run `pio` commands from the project directory.

| Folder | Target | Role |
|---|---|---|
| `firmware/v2/Master/` | ESP32-S3-WROOM-1 (N16R8) | **The master.** Web UI, WiFi, NTP, MQTT/HA, I2C unit bus, OTA, cluster leader/follower. |
| `firmware/v2/Unit/` | Arduino Nano | Per-flap unit: stepper homing, I2C slave, EEPROM offset/address. |
| `firmware/v2/FollowerEsp01/` | ESP8266 ESP-01 | Optional "dumb row" — turns legacy ESP-01 master hardware into a cheap cluster follower row under an S3 leader. |
| `firmware/v2/Rescue/` | ESP32-S3 (factory slot) | Break-glass recovery image; installs a good master image when the app slots are bad. |
| `firmware/v2/Bootloader/` | ESP32-S3 | Custom second-stage bootloader (adds a factory-reset button on GPIO 4). |
| `firmware/v2/UnitBootloader/` | Arduino Nano | Vendored + patched **twiboot** for reflashing units over I2C (see its README). |
| `firmware/v1/ESPMaster/` | ESP8266 ESP-01 | **Frozen, deprecated** — the original v1 master, kept as reference only, out of CI (see its README). |

Install PlatformIO Core once:

```bash
pip install -U platformio
```

Then, from a project directory:

```bash
pio run                  # build
pio run -t upload        # USB flash (first time / devkit)
pio device monitor       # serial monitor at 115200 baud
pio test -e native       # host-side unit tests
python -m pytest tests/  # python-side tests (where present)
```

The web UI and the bundled unit firmware are gzipped and compiled into the master binary at build time by `firmware/v2/Master/build_assets.py` — there is no separate filesystem-flash step; `pio run -t upload` lands the whole thing.

For a Nano whose upload fails on the stock bootloader, use the fallback env:

```bash
pio run -e unit_old_bootloader -t upload
```

## The master (ESP32-S3)

The master is an **ESP32-S3-WROOM-1-N16R8 devkit**. It drives the units over I2C (SDA = GPIO 8, SCL = GPIO 9, 100 kHz) and exposes everything through a three-tab web UI.

Highlights:

- **WiFi portal** — on first boot (or when the stored network can't be reached for ~30 s) the master brings up a `<device-name>-setup` access point. Connect, pick your network, done. Credentials live in NVS and survive reboots and firmware updates.
- **Clock / NTP** — full IANA timezone picker (baked table served at `/tz.json`); the time is set at boot, on WiFi join, and whenever you change the zone.
- **OTA** — upload a new `firmware-<rev>.bin` from **Maintenance → Master Firmware (OTA)** (mandatory `?md5=` integrity check). The S3's A/B slots mean a bad image rolls back automatically. The same flow can reflash every detected unit right after.
- **Rescue / factory reset** — hold the GPIO 4 button for 5 s through reset to boot the rescue app from the factory slot, or `POST /firmware/rescue-boot`. WiFi credentials always survive a factory reset.
- **MQTT / Home Assistant** — see below.
- **Live vitals** — the System tab shows heap, load, I2C traffic, MQTT state, and NTP sync age; live display changes stream over SSE.

## The units (Arduino Nano)

Each split-flap unit is an Arduino Nano on a custom PCB, driving a **28BYJ-48** 12 V stepper through a **ULN2003** and homing the drum against a **KY-003 hall sensor** + magnet. Per-unit calibration (step offset) and the I2C address live in the Nano's EEPROM.

Flash a unit's Nano from `firmware/v2/Unit/`:

```bash
pio run -t upload
```

### Set the unit address

Each unit's address comes from a 4-way DIP switch (the firmware adds 1 so DIP `0000` → I2C `0x01`; address `0x00` is the reserved general-call address). Set them ascending from zero:

| Unit | DIP  | I2C address |
| ---- | ---- | ----------- |
| 1    | 0000 | 0x01        |
| 2    | 0001 | 0x02        |
| 3    | 0010 | 0x03        |
| 4    | 0011 | 0x04        |
| 5    | 0100 | 0x05        |
| …    | …    | …           |

The master scans the bus at boot and derives the display width from the highest responding address. A dead unit keeps its slot (the layout doesn't shift), but a unit parked on a high address with nothing below it widens the display. The web UI's **Units: N / W** field shows detected responders vs. derived width. A unit count can also be pinned manually (Maintenance → Display width) for headless/dummy setups.

### Calibrate the zero position

The zero (blank-flaps) position is set by homing to the hall sensor and stepping a few steps forward — an offset unique to each unit, stored in EEPROM. Calibrate from the web UI, no reflashing:

1. Open **Maintenance → Calibration**.
2. Pick a test letter; the master sends it to every unit.
3. For each unit, type what the drum is *actually* showing.
4. **Apply All** — the master reads each offset, computes the corrective delta, writes EEPROM, and re-homes, all over I2C.

Expand **Advanced** for raw offset + jog controls and half-flap fine tuning.

### Reflash units over I2C

Units carry the [patched twiboot bootloader](./firmware/v2/UnitBootloader/README.md) (flashed once per unit via ICSP). The master ships the compiled unit sketch in its own binary and:

- auto-installs it on any Nano it finds sitting in twiboot (e.g. a freshly ICSP-flashed one), and
- reflashes every unit on demand via **Maintenance → Actions → Flash all units** (or automatically for out-of-date units after a master OTA).

## Multi-display cluster

Several masters on the same LAN can be joined into one logical wall. One master is the **leader**; the others are **followers** (either full S3 masters or cheap ESP-01 "dumb rows" running `firmware/v2/FollowerEsp01`). The leader wraps/aligns your text across the grid and drives every row in sync; followers render their assigned segment.

- Configure it from **Settings → Cluster** (member editor, network scan, live status pills, rollout progress).
- The leader converges follower firmware to its own build automatically (rev mismatch in the join handshake triggers a streamed OTA), so the fleet stays on one image.
- Leader/follower wire traffic is authenticated (per-member HMAC) and LAN-scoped; if the leader dies, a follower can be promoted.

Design details: [`docs/superpowers/specs/2026-07-13-multi-display-cluster-design.md`](./docs/superpowers/specs/2026-07-13-multi-display-cluster-design.md) and the sibling cluster specs.

## MQTT / Home Assistant

The display joins Home Assistant over MQTT with automatic discovery: inbound notification text (shown for a dwell, then reverts), a **Mode** select (text/clock), and health telemetry — including per-unit wear and cluster health when leading. Configure it entirely from **Settings → MQTT Broker** (host, port, username, password); leave the host empty to keep MQTT off. If a broker is advertised over mDNS on your LAN, **Detect broker** prefills host/port. Create a dedicated HA user for the display rather than reusing your own login.

## Device name & running multiple displays

Every network-facing name (mDNS `<name>.local`, DHCP hostname, MQTT client id/topics, AP SSIDs) derives from one per-device identity. Out of the box it's `split-flap-<hex chip id>` — unique per board, so several displays can share a LAN running the **same image** with no per-device edits. Set a friendly name in **Settings → Device → Device Name** (lowercase letters/digits/hyphens, ≤24 chars). Use a **DHCP reservation** on the board's MAC for a fixed IP.

## Flashing & provisioning

- **First flash** of an S3 is a USB job (`pio run -t upload`, or the merged factory-bin recipe in [`flashing/README.md`](./flashing/README.md) which lands the custom bootloader + rescue slot in one shot).
- **Subsequent flashes** go over the air — from the web UI, or scripted with [`flashing/ota-flash.sh`](./flashing/ota-flash.sh) (fetches the latest staged bin, OTAs master/follower/rescue targets, reads the verdict back from `/settings`).
- The bundled unit firmware is staged into the master (and follower) `data/` dirs by `flashing/flasher/make_manifest.py stage`; CI's `gate` step enforces that it never drifts from the built Unit hex.

See [`flashing/README.md`](./flashing/README.md) for the full recipe (DIP address table, esptool factory-bin, OTA flags).

## PCB

The **currently deployed** hardware uses the original per-unit Arduino-Nano PCB (Gerbers under `PCB/`), one board per unit. Services like [JLCPCB](https://jlcpcb.com/) or [PCBWay](https://www.pcbway.com/) can fabricate and assemble them — some surface-mount parts are best flow-soldered by the fab.

A ground-up **v2 hardware redesign** is documented (not yet built) under [`PCB/v2/`](./PCB/v2/README.md): a custom STM32G030K8 unit board with an RS-485 multidrop bus, card-edge backplane, and silicon-UID addressing, plus a minimal ESP32-S3 master carrier. See `PCB/v2/OPEN_DECISIONS.md` for the locked decisions and open items.

## Common problems

- **Display width looks wrong** — check each unit's DIP-switch address and wiring; address contiguously from DIP `0000` (I2C `0x01`). The **Units: N / W** field shows detected vs. derived width.
- **A unit never homes** — when powered, the KY-003 hall sensor should only light when the magnet passes it. Check the sensor lead and magnet alignment.
- **Only `pio run -t upload`** is needed to flash — web assets and the bundled unit firmware are baked into the binary; there is no separate filesystem upload.
- **WiFi range** — user [@beroliv](https://github.com/beroliv) reported ESP8266 WiFi issues on legacy hardware, resolved by soldering an extension wire to the antenna ([writeup, German](https://www.stall.biz/project/verbesserte-wlan-konnektivitaet-mit-externen-antennen-fuer-wiffi-weatherman-und-andere-module-mit-esp8266/)). Take care if you try it.

## Going deeper

- [`CLAUDE.md`](./CLAUDE.md) — the authoritative map of the current architecture, per-mechanism ownership, and hard invariants.
- [`docs/superpowers/specs/`](./docs/superpowers/specs/) — a dated design spec per feature.
- Per-mechanism detail lives in the header comment of the file that owns it.

Assembly of the mechanical build follows the [instruction manual](./Instructions/SplitFlapInstructions.pdf).
