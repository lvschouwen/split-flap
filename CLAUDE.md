# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

An Arduino-based split-flap display. Firmware built with **PlatformIO** — each of the three sketches has its own `platformio.ini` and builds from the CLI. Host-side unit tests for pure-logic helpers live under `ESPMaster/test/`. CI in `.github/workflows/build.yml` builds all three envs and runs the native tests.

## Architecture

Three independent Arduino sketches that together make up the system:

- **`ESPMaster/`** — ESP8266 (ESP-01) firmware. Hosts the async web UI (served from LittleFS in `ESPMaster/data/`), connects to WiFi (via `WiFiManager` AP flow or direct credentials), runs the NTP-backed clock logic, and pushes characters out over I2C as the bus master. Entry point is `ESPMaster.ino`; feature areas live in sibling `.ino` files which the Arduino/PlatformIO preprocessor concatenates at build time (`ServiceFlapFunctions`, `ServiceWifiFunctions`, `ServiceFileSystemFunctions`, `ServiceFirmwareFunctions`, `ServiceWebLog`, `HelpersStringHandling`). `HelpersSerialHandling.h` is a header-only helper (templates can't be auto-prototyped, so they must live in a header); `ESPMaster.h` declares prototypes for functions with `fs::FS&` signatures that the Arduino preprocessor can't auto-prototype. The web UI (`data/index.html`, `script.js`, `style.css`) uploads separately via `pio run -t uploadfs`.
- **`Unit/Unit.ino`** — Per-flap Arduino Nano firmware. I2C slave whose address is read from four DIP-switch pins (`ADRESSSW1..4`) and offset by `I2C_ADDRESS_BASE` (currently 1) so DIP 0000 → I2C 0x01 (address 0x00 is reserved for general call). Drives a 28BYJ-48 stepper via ULN2003 (`STEPPERPIN1..4`), homes on a KY-003 hall sensor at `HALLPIN`, and applies a per-unit step offset stored in EEPROM. The character alphabet is a fixed `letters[]` array in the sketch — the master sends an index, not the character.
- **`EEPROM_Write_Offset/EEPROM_Write_Offset.ino`** — One-off utility sketch flashed per Nano to calibrate and persist the hall-sensor-to-blank-flap step offset. Re-flash `Unit.ino` afterwards.
- **`UnitBootloader/`** — Vendored + patched [twiboot](https://github.com/orempel/twiboot), an I2C bootloader for the Nanos. Once installed (one-time ICSP flash per unit), the master will be able to push new unit firmware over I2C instead of requiring physical re-flashing. See `UnitBootloader/README.md`. In-progress project tracked in issue #10.

Master/unit contract: I2C index in the `letters[]` table + speed byte, with `ANSWER_SIZE = 1` status byte back. Number of units is a compile-time constant (`UNITS_AMOUNT` in `ESPMaster.ino`, default 10) and must match physical hardware.

## Build / Flash / Test Workflow (PlatformIO)

Each sketch has its own `platformio.ini`. Run all commands from the sketch folder.

```bash
pio run                      # build
pio run -t upload            # flash sketch
pio run -t uploadfs          # flash LittleFS (ESPMaster only)
pio device monitor           # serial at 115200
pio test -e native           # host-side unit tests (ESPMaster only)
```

- **ESPMaster** — env `espmaster`, board `esp01_1m`. Library versions pinned in `platformio.ini`. Framework-builtin libs (`EEPROM`, `ArduinoOTA`) are added to `lib_deps` explicitly because PIO's LDF doesn't surface them by default; `build_flags` add `-I` paths to the ESP8266 core's `EEPROM/` and `ESP8266WiFi/src/` so `ezTime` compiles (it `#include`s them without declaring them as deps).
- **Unit** — env `unit` (board `nanoatmega328new`, new bootloader) or `unit_old_bootloader` (board `nanoatmega328`, old bootloader fallback).
- **EEPROM_Write_Offset** — same pattern as Unit; two envs for new/old bootloader.

Native test env (`ESPMaster/platformio.ini` → `[env:native]`): compiles pure-logic files for the host using `fabiobatsilva/ArduinoFake` (provides `String`, `Print`, etc.) plus the LinkedList library. Tests live in `ESPMaster/test/test_*/test_main.cpp` and are discovered automatically. `ArduinoFake` stubs `map()` as a fakeit mock — each test's `setUp()` must wire in the real Arduino formula via `When(Method(ArduinoFake(Function), map)).AlwaysDo(...)`, otherwise calling `map()` aborts. Other ArduinoFake-mocked globals (e.g. `EEPROM`) are accessed via the `ArduinoFake(EEPROM)` helper macro and should be re-wired in `setUp()` with `When(Method(...)).AlwaysDo([](...) { ... })`.

Python-side tests live in `ESPMaster/tests/` and cover `build_assets.py` helpers. Run with `python -m pytest tests/` from the `ESPMaster` directory.

CI: `.github/workflows/build.yml` matrix-builds all three envs + runs the native test env.

## Configuration Knobs Worth Knowing

All live as `#define`s at the top of `ESPMaster.ino`:

- `UNITS_AMOUNT` — **must match physical unit count**; mismatches are the #1 cause of "master can't talk to units".
- `SERIAL_ENABLE` — toggling this to `true` **disables I2C** on the ESP-01 (shared pins). Keep `false` unless deliberately debugging over serial with units disconnected.
- `UNIT_CALLS_DISABLE` — lets the ESP run standalone for web-UI work.
- `WIFI_USE_DIRECT` — `false` = WiFiManager captive portal (default); `true` uses the hard-coded `wifiDirectSsid` / `wifiDirectPassword`.
- `USE_MULTICAST` — enables mDNS at `<mdnsName>.local` (default `split-flap.local`). Pick a unique `mdnsName` per display if running multiple.
- `WIFI_STATIC_IP` — experimental; prefer DHCP reservation on the router.
- `OTA_ENABLE` — enables Arduino OTA; set `otaPassword` if used.
- `FLAP_AMOUNT` / `AMOUNTFLAPS` — hardware-coupled (45). The master's `FLAP_AMOUNT` and the unit's `letters[]` length must agree.

Unit-side: `SERIAL_ENABLE` and `TEST_ENABLE` (cycles a fixed character sequence for homing validation) are commented out by default in `Unit.ino`.

## Gotchas

- Editing `.ino` siblings in `ESPMaster/` is editing one translation unit — declarations and `#define`s from `ESPMaster.ino` are visible everywhere, and order of concatenation is alphabetical by file name. The Arduino preprocessor auto-prototypes plain functions but falls over on templates (`<typeprefixerror>`) and on signatures with namespace-qualified references like `fs::FS&`; add those prototypes manually (header file) rather than relying on auto-prototyping.
- The LittleFS upload is a **separate step** from the sketch upload; forgetting it leaves the web UI serving nothing.
- The ESP-01 has very little RAM; be conservative adding libraries or large JSON payloads. `ArduinoJson` is already pinned to 7.4.3.
- `#define WEBSERVER_H` before `<WiFiManager.h>` is a deliberate workaround — don't "clean up" that include order.
- Test files `#include` the `.ino` sources directly to exercise real code. This means any sibling `.ino` must be compilable standalone in the native env — add explicit forward declarations at the top of a file (e.g. `String cleanString(String);` in `HelpersStringHandling.ino`) if functions are used before their definition in the same file.
