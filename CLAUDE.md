# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

An Arduino-based split-flap display. Firmware built with **PlatformIO** — each sketch has its own `platformio.ini` and builds from the CLI. Host-side unit tests for pure-logic helpers live under `ESPMaster/test/`. CI in `.github/workflows/build.yml` builds all envs and runs the native tests.

**Firmware status (2026-04-22):** `ESPMaster/` (ESP8266) is **frozen** at tag `v-esp8266-final`. Active firmware development moves to the ESP32-S3 + ESP32-H2 stack on the Master v2 Rev B PCB — `MasterS3/` and `MasterH2/` (both in progress). See meta issue #58 for the migration plan and `PCB/MASTER_V2/` for the hardware spec.

## Architecture

Sketches that make up the system:

- **`MasterS3/`** — *In progress (#58, scaffold landed in #63).* ESP32-S3-WROOM-1-N16R8 firmware on the Master v2 Rev B PCB. Standard PlatformIO layout (`src/main.cpp`, no Arduino preprocessor magic). Uses pioarduino fork of `platform-espressif32` for arduino-esp32 v3.x. USB-CDC on boot. Wokwi config + custom TCA9548A chip live at `MasterS3/wokwi.toml`, `MasterS3/diagram.json`, `MasterS3/chips/tca9548a/`.
- **`MasterH2/`** — *In progress (#58, scaffold landed in #63).* ESP32-H2-MINI-1-N4 radio coprocessor firmware. Same pioarduino fork. Single sketch, UART link to S3 lands in Phase 3.
- **`lib/common/`** — Shared pure-logic helpers consumed by both ESP32 firmwares + native tests (COBS+CRC, protocol, settings, web log). Empty in Phase 2; populated from Phase 3 onward.
- **`ESPMaster/`** — *Frozen at `v-esp8266-final`.* ESP8266 (ESP-01) firmware. Hosts the async web UI (baked into `WebAssets.h` PROGMEM at build time by `build_assets.py` — no filesystem on the device), connects to WiFi (via `ESPAsyncWiFiManager` AP flow or direct credentials from `WifiCredentials.h`), runs the NTP-backed clock logic, and pushes characters out over I2C as the bus master. Entry point is `ESPMaster.ino`; feature areas live in sibling `.ino` files which the Arduino/PlatformIO preprocessor concatenates at build time (`ServiceFlapFunctions`, `ServiceWifiFunctions`, `ServiceFileSystemFunctions`, `ServiceFirmwareFunctions`, `ServiceWebLog`, `HelpersStringHandling`). Header-only helpers: `HelpersSerialHandling.h` (templates — can't be auto-prototyped), `WebLog.h` (Print-subclass log ring + API). `ESPMaster.h` declares prototypes for functions with types the Arduino preprocessor can't auto-prototype.
- **`Unit/Unit.ino`** — Per-flap Arduino Nano firmware. I2C slave whose address is read from four DIP-switch pins (`ADRESSSW1..4`) and offset by `I2C_ADDRESS_BASE` (currently 1) so DIP 0000 → I2C 0x01 (address 0x00 is reserved for general call). Drives a 28BYJ-48 stepper via ULN2003 (`STEPPERPIN1..4`), homes on a KY-003 hall sensor at `HALLPIN`, and applies a per-unit step offset stored in EEPROM. The character alphabet is a fixed `letters[]` array in the sketch — the master sends an index, not the character. Calibration (offset / jog / home) is driven from the master's web UI over I2C opcodes.
- **`UnitBootloader/`** — Vendored + patched [twiboot](https://github.com/orempel/twiboot), an I2C bootloader for the Nanos. Once installed (one-time ICSP flash per unit), the master pushes new unit firmware over I2C from a PROGMEM-bundled hex (no more ICSP cables). See `UnitBootloader/README.md`.

Master/unit contract: I2C index in the `letters[]` table + speed byte, with `ANSWER_SIZE = 1` status byte back. `UNITS_AMOUNT` in `ESPMaster.ino` bounds per-unit state arrays on the master — probe only acts on addresses that actually reply, so setting it higher than the physical unit count is safe.

## Build / Flash / Test Workflow (PlatformIO)

Each sketch has its own `platformio.ini`. Run all commands from the sketch folder.

```bash
pio run                      # build
pio run -t upload            # flash sketch (USB, one-time)
pio device monitor           # serial at 115200
pio test -e native           # host-side unit tests (ESPMaster only)
```

Subsequent master flashes happen via OTA — from this repo: `flashing/ota-master.sh <fw.bin> http://host:port`. The script computes MD5 locally, POSTs to `/firmware/master`, then polls `/settings` for the `sketchMd5` + `lastFlashResult` verdict and prints SUCCESS, EBOOT SILENT REVERT, or UPLOAD DID NOT REACH HANDLER. Physical re-flash falls back to `esptool` — see issue #53 for the Windows walkthrough.

- **ESPMaster** — env `espmaster`, board `esp01_1m`. Library versions pinned in `platformio.ini`. Builtin `EEPROM` is in `lib_deps` because PIO's LDF doesn't surface it by default.
- **Unit** — env `unit` (board `nanoatmega328new`, new bootloader) or `unit_old_bootloader` (board `nanoatmega328`, old bootloader fallback).

Native test env (`ESPMaster/platformio.ini` → `[env:native]`): compiles pure-logic headers/helpers for the host using `fabiobatsilva/ArduinoFake` (provides `String`, `Print`, etc.). Tests live in `ESPMaster/test/test_*/test_main.cpp` and are discovered automatically. `ArduinoFake` stubs `map()` as a fakeit mock — each test's `setUp()` must wire in the real Arduino formula via `When(Method(ArduinoFake(Function), map)).AlwaysDo(...)`, otherwise calling `map()` aborts. Other ArduinoFake-mocked globals (e.g. `EEPROM`) are accessed via the `ArduinoFake(EEPROM)` helper macro and should be re-wired in `setUp()` with `When(Method(...)).AlwaysDo([](...) { ... })`.

Python-side tests live in `ESPMaster/tests/` and cover `build_assets.py` helpers. Run with `python -m pytest tests/` from the `ESPMaster` directory.

CI: `.github/workflows/build.yml` matrix-builds both firmware projects (ESPMaster, Unit) + runs the native test env.

## Configuration Knobs Worth Knowing

All live as `#define`s at the top of `ESPMaster.ino`:

- `UNITS_AMOUNT` — max units the master will track. Sized to the DIP-switch hardware ceiling; safe to leave at the max even with fewer physical units (probe ignores non-responders).
- `SERIAL_ENABLE` — toggling this to `true` **disables I2C** on the ESP-01 (shared pins). Keep `false` unless deliberately debugging over serial with units disconnected.
- `UNIT_CALLS_DISABLE` — lets the ESP run standalone for web-UI work.
- `WIFI_USE_DIRECT` — `false` = WiFiManager captive portal; `true` uses the hard-coded `wifiDirectSsid` / `wifiDirectPassword` from `WifiCredentials.h`. When `true`, recovery mode (see below) also joins the known WiFi instead of bringing up a SoftAP.
- `USE_MULTICAST` — enables mDNS at `<mdnsName>.local`. Pick a unique `mdnsName` per display if running multiple.
- `WIFI_STATIC_IP` — experimental; prefer DHCP reservation on the router.
- `OTA_ENABLE` — enables Arduino OTA (separate from the web `/firmware/master` path); set `otaPassword` if used.
- `FLAP_AMOUNT` / `AMOUNTFLAPS` — hardware-coupled (45). The master's `FLAP_AMOUNT` and the unit's `letters[]` length must agree.

Unit-side: `SERIAL_ENABLE` and `TEST_ENABLE` (cycles a fixed character sequence for homing validation) are commented out by default in `Unit.ino`.

## EEPROM layout

`SettingsEepromLayout.h` carves the master's ESP8266 EEPROM region into named slots. Every time a new slot is added, bump `SETTINGS_VERSION` and extend the `ver < N` migration ladder in `initialiseFileSystem()` so existing blobs upgrade in place (no user-visible settings loss). The `RESERVED_2` region holds headroom for future slots — shrinking its `LEN` is how new fields are carved. Current slots:

- `MAGIC` / `VERSION` — marker + schema version (`SETTINGS_VERSION`).
- `alignment` — `"left"` / `"center"` / `"right"`.
- `flapSpeed` — decimal int as string.
- `deviceMode` — `"text"` / `"clock"`.
- `timezonePosix` — POSIX TZ string. Fresh-init default is `"CET-1CEST,M3.5.0,M10.5.0/3"` so a wipe+reflash lands on CE(S)T. The web UI overrides at runtime.
- `intendedVersion` — `GIT_REV` of the most-recently-uploaded firmware, written by `/firmware/master` from its `?v=` query param. Read at boot; if non-empty and ≠ running `GIT_REV`, the `otaReverted` flag is set and surfaced in `/settings`.
- `lastFlashResult` — `""` / `"ok"` / `"reverted"`, written by the boot-time RTC-cookie check (see OTA section below). Surfaced in `/settings` so remote flashers can decode the outcome of a prior OTA attempt.

Native tests (`test/test_eeprom_settings/test_main.cpp`) enforce layout invariants (contiguity, bounds, round-trip, migration zero-fill) so a malformed edit fails `pio test -e native` before it ships.

## OTA + recovery

Master flashes go through `/firmware/master` (HTTP POST `multipart/form-data` + `?md5=`). The handler verifies MD5 via `Update.setMD5`, streams to the staging slot, calls `Update.end(true)`, then — if clean — stashes the **pre-flash sketch MD5** in RTC memory and reboots. On the next boot:

- The new firmware reads RTC, compares its own `ESP.getSketchMD5()` against the cookie, and writes `"ok"` (new bits running) or `"reverted"` (same bits → eboot rejected the copy) to the EEPROM `lastFlashResult` slot.
- `/settings` exposes `sketchMd5`, `lastFlashResult`, `intendedVersion`, `otaReverted`, `lastResetReason`, `bootCounter`, `recoveryMode`.
- `flashing/ota-master.sh` turns these into a verdict: SUCCESS / EBOOT SILENT REVERT / UPLOAD DID NOT REACH HANDLER / INCONSISTENT.

**Recovery mode** activates on 3 consecutive unhealthy boots (RTC counter in `RtcBootState`, reset after 30 s of clean uptime). It also accepts a remote trigger: `POST /firmware/recover-mark` writes the counter to the threshold and reboots, so the next boot drops into recovery without physical access. In recovery:

- `WIFI_USE_DIRECT == true` → join hardcoded WiFi, serve a minimal upload form + `/firmware/master` on the normal LAN IP.
- Otherwise → bring up SoftAP `split-flap-recovery` and serve the same endpoints.

The `RtcBootState` struct holds `magic`, `bootCounter`, and `preFlashSketchMd5[36]`. The magic (`RTC_BOOT_MAGIC`) is checked on read; a mismatch (cold boot, struct change) zero-inits the state.

## Gotchas

- Editing `.ino` siblings in `ESPMaster/` is editing one translation unit — declarations and `#define`s from `ESPMaster.ino` are visible everywhere, and order of concatenation is alphabetical by file name. The Arduino preprocessor auto-prototypes plain functions but falls over on templates (`<typeprefixerror>`) and on signatures with namespace-qualified references like `fs::FS&`; add those prototypes manually (header file) rather than relying on auto-prototyping.
- The web UI is in PROGMEM (`WebAssets.h`, regenerated by `build_assets.py` on every build), NOT in LittleFS. No separate `uploadfs` step — editing `data/*.html|js|css` is picked up on the next `pio run`.
- The ESP-01 has very little RAM (~42 KB static free at rest). Be conservative adding libraries or large JSON payloads.
- On the async library choice: `dvarrel` forks of `ESPAsyncTCP` + `ESPAsyncWebSrv` are the maintained ESP8266-focused path. `mathieucarbou` is the ESP32 successor; wrong target for ESP-01. Don't "modernize" without measuring.
- `#define WEBSERVER_H` before `<ESPAsyncWiFiManager.h>` is a deliberate workaround for the include-order dance between the async WiFi manager and the sync WebServer header — don't "clean up" that include order.
- Test files `#include` the `.ino` sources directly to exercise real code. This means any sibling `.ino` must be compilable standalone in the native env — add explicit forward declarations at the top of a file (e.g. `String cleanString(String);` in `HelpersStringHandling.ino`) if functions are used before their definition in the same file.
- Eboot (the ESP8266 first-stage bootloader) copies from the OTA staging slot into the run slot on reboot. A silent revert — staging write OK, `Update.end(true)` returns success, but the post-reboot sketch is still the old one — is almost always ESP-01 power sag during the erase/write cycle. The `lastFlashResult="reverted"` signal surfaces it; fixing it needs hardware intervention (decoupling caps, beefier supply), not more firmware.
