# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

An Arduino-based split-flap display. Firmware built with **PlatformIO** — each sketch has its own `platformio.ini` and builds from the CLI. Host-side unit tests for pure-logic helpers live under `firmware/v1/ESPMaster/test/`. CI in `.github/workflows/build.yml` builds every firmware project and runs the native test suite.

**Firmware status:** the shipping firmware is the v1 stack under `firmware/v1/` — `ESPMaster/` (ESP8266 master) + `Unit/` (Arduino Nano per-flap). This is the actively-maintained firmware. A previous ESP32-S3 + ESP32-H2 firmware port was removed pending a fresh v2 design; the `firmware/v1/` prefix is kept so a future v2 firmware stack can slot in alongside. The v2 **PCB** design docs under `PCB/v2/` are unaffected.

## Architecture

Sketches that make up the system:

- **`firmware/v1/ESPMaster/`** — ESP8266 (ESP-01) master firmware. Hosts the async web UI (baked into `WebAssets.h` PROGMEM at build time by `build_assets.py` — no filesystem on the device), connects to WiFi (via `ESPAsyncWiFiManager` AP flow or direct credentials from `WifiCredentials.h`), runs the NTP-backed clock logic, and pushes characters out over I2C as the bus master. Entry point is `ESPMaster.ino`; feature areas live in sibling `.ino` files which the Arduino/PlatformIO preprocessor concatenates at build time (`ServiceFlapFunctions`, `ServiceWifiFunctions`, `ServiceFileSystemFunctions`, `ServiceFirmwareFunctions`, `ServiceWebLog`, `ServiceMqttFunctions`, `HelpersStringHandling`). Header-only helpers: `HelpersSerialHandling.h` (templates — can't be auto-prototyped), `WebLog.h` (Print-subclass log ring + API), `MqttHelpers.h` (pure MQTT payload/discovery logic, natively tested). `ESPMaster.h` declares prototypes for functions with types the Arduino preprocessor can't auto-prototype.
- **`firmware/v1/Unit/Unit.ino`** — Per-flap Arduino Nano firmware. I2C slave whose address is read from four DIP-switch pins (`ADRESSSW1..4`) and offset by `I2C_ADDRESS_BASE` (currently 1) so DIP 0000 → I2C 0x01 (address 0x00 is reserved for general call). Drives a 28BYJ-48 stepper via ULN2003 (`STEPPERPIN1..4`), homes on a KY-003 hall sensor at `HALLPIN`, and applies a per-unit step offset stored in EEPROM. The character alphabet is a fixed `letters[]` array in the sketch — the master sends an index, not the character. Calibration (offset / jog / home) is driven from the master's web UI over I2C opcodes.
- **`firmware/v1/UnitBootloader/`** — Vendored + patched [twiboot](https://github.com/orempel/twiboot), an I2C bootloader for the Nanos. Once installed (one-time ICSP flash per unit), the master pushes new unit firmware over I2C from a PROGMEM-bundled hex (no more ICSP cables). See `firmware/v1/UnitBootloader/README.md`.

Master/unit contract: I2C index in the `letters[]` table + speed byte, with `ANSWER_SIZE = 1` status byte back. `UNITS_AMOUNT` in `ESPMaster.ino` bounds per-unit state arrays on the master — probe only acts on addresses that actually reply, so setting it higher than the physical unit count is safe.

## Build / Flash / Test Workflow (PlatformIO)

Each sketch has its own `platformio.ini`. Run all commands from the sketch folder.

```bash
pio run                      # build
pio run -e espmaster_mqtt    # MQTT-enabled variant (#121)
pio run -t upload            # flash sketch (USB, one-time)
pio device monitor           # serial at 115200
pio test -e native           # host-side unit tests (ESPMaster only)
```

Subsequent master flashes happen via OTA — from this repo: `flashing/ota-master.sh <fw.bin> http://host:port`. The script computes MD5 locally, POSTs to `/firmware/master`, then polls `/settings` for the `sketchMd5` + `lastFlashResult` verdict and prints SUCCESS, EBOOT SILENT REVERT, FLASH CONFIG MISMATCH, or UPLOAD DID NOT REACH HANDLER. Physical re-flash falls back to `esptool` — see issue #53 for the Windows walkthrough.

- **ESPMaster** — env `espmaster`, board `esp01_1m`. Library versions pinned in `platformio.ini`. Builtin `EEPROM` is in `lib_deps` because PIO's LDF doesn't surface it by default.
- **Unit** — env `unit` (board `nanoatmega328new`, new bootloader) or `unit_old_bootloader` (board `nanoatmega328`, old bootloader fallback).

Host-side native test env:

- `firmware/v1/ESPMaster/platformio.ini` → `[env:native]` — ESP8266 sketch's pure-logic helpers. Uses `fabiobatsilva/ArduinoFake` to stub `String`, `Print`, etc. Tests in `firmware/v1/ESPMaster/test/test_*/test_main.cpp`.

The ESPMaster env: `ArduinoFake` stubs `map()` as a fakeit mock — each test's `setUp()` must wire in the real Arduino formula via `When(Method(ArduinoFake(Function), map)).AlwaysDo(...)`, otherwise calling `map()` aborts. Other ArduinoFake-mocked globals (e.g. `EEPROM`) are accessed via the `ArduinoFake(EEPROM)` helper macro and should be re-wired in `setUp()` with `When(Method(...)).AlwaysDo([](...) { ... })`.

Python-side tests live in `firmware/v1/ESPMaster/tests/` and cover `build_assets.py` helpers. Run with `python -m pytest tests/` from the `firmware/v1/ESPMaster` directory.

CI: `.github/workflows/build.yml` matrix-builds the firmware projects (ESPMaster, Unit) + runs the ESPMaster native test env.

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
- `MQTT_ENABLE` — MQTT / Home Assistant integration (#121). Default `false` (fully compiled out). Build the `espmaster_mqtt` env and copy `MqttCredentials.h.example` → `MqttCredentials.h` (gitignored) to enable. Notification-only semantics: HA text shows for a dwell (default 60 s, clamp 5–3600), then reverts; health telemetry every 60 s; zero EEPROM/RTC interaction.

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
- `flashing/ota-master.sh` turns these into a verdict: SUCCESS / EBOOT SILENT REVERT / FLASH CONFIG MISMATCH / UPLOAD DID NOT REACH HANDLER / INCONSISTENT.

**Recovery mode** activates on 3 consecutive unhealthy boots (RTC counter in `RtcBootState`, reset after 30 s of clean uptime). It also accepts a remote trigger: `POST /firmware/recover-mark` writes the counter to the threshold and reboots, so the next boot drops into recovery without physical access. In recovery:

- `WIFI_USE_DIRECT == true` → join hardcoded WiFi, serve a minimal upload form + `/firmware/master` on the normal LAN IP.
- Otherwise → bring up SoftAP `split-flap-recovery` and serve the same endpoints.

The `RtcBootState` struct holds `magic`, `bootCounter`, `bootMode` (quiet OTA mode, #117), `cookieKind` (#118), and `preFlashSketchMd5[36]`. The magic (`RTC_BOOT_MAGIC`) is checked on read; an unknown magic zero-inits the state, but the V3 magic (98ec681-era layout, no `cookieKind`) is migrated in place so a flash performed by pre-#118 firmware still gets a correct verdict on the first post-upgrade boot (#119). On an "ok" verdict the boot check also rewrites `intendedVersion` to the running rev, healing stale values left by environments that couldn't persist `?v=`.

## Gotchas

- Editing `.ino` siblings in `firmware/v1/ESPMaster/` is editing one translation unit — declarations and `#define`s from `ESPMaster.ino` are visible everywhere, and order of concatenation is alphabetical by file name. The Arduino preprocessor auto-prototypes plain functions but falls over on templates (`<typeprefixerror>`) and on signatures with namespace-qualified references like `fs::FS&`; add those prototypes manually (header file) rather than relying on auto-prototyping.
- The `.ino`→`.cpp` auto-prototype scanner is **preprocessor-blind**: it generates prototypes even for functions inside a disabled `#if` block, so a gated function whose signature uses library types (e.g. the `#if MQTT_ENABLE` AsyncMqttClient callbacks) needs unconditional forward declarations of those types near the top of `ESPMaster.ino` — see the comment block above them there.
- The web UI is in PROGMEM (`WebAssets.h`, regenerated by `build_assets.py` on every build), NOT in LittleFS. No separate `uploadfs` step — editing `data/*.html|js|css` is picked up on the next `pio run`.
- The ESP-01 has very little RAM (~42 KB static free at rest). Be conservative adding libraries or large JSON payloads.
- On the async library choice: `dvarrel` forks of `ESPAsyncTCP` + `ESPAsyncWebSrv` are the maintained ESP8266-focused path. `mathieucarbou` is the ESP32 successor; wrong target for ESP-01. Don't "modernize" without measuring.
- `#define WEBSERVER_H` before `<ESPAsyncWiFiManager.h>` is a deliberate workaround for the include-order dance between the async WiFi manager and the sync WebServer header — don't "clean up" that include order.
- Test files `#include` the `.ino` sources directly to exercise real code. This means any sibling `.ino` must be compilable standalone in the native env — add explicit forward declarations at the top of a file (e.g. `String cleanString(String);` in `HelpersStringHandling.ino`) if functions are used before their definition in the same file.
- Eboot (the ESP8266 first-stage bootloader) copies from the OTA staging slot into the run slot on reboot. A silent revert — staging write OK, `Update.end(true)` returns success, but the post-reboot sketch is still the old one — is almost always ESP-01 power sag during the erase/write cycle. The `lastFlashResult="reverted"` signal surfaces it; fixing it needs hardware intervention (decoupling caps, beefier supply), not more firmware.
