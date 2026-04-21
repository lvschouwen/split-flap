# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

An Arduino-based split-flap display. Firmware built with **PlatformIO** — each of the three sketches has its own `platformio.ini` and builds from the CLI. Host-side unit tests for pure-logic helpers live under `ESPMaster/test/`. CI in `.github/workflows/build.yml` builds all three envs and runs the native tests.

## Architecture

Three independent Arduino sketches that together make up the system:

- **`ESPMaster/`** — ESP8266 (ESP-01) firmware. Hosts the async web UI (baked into `WebAssets.h` PROGMEM at build time by `build_assets.py` — no filesystem on the device), connects to WiFi (via `ESPAsyncWiFiManager` AP flow or direct credentials), runs the NTP-backed clock logic, optionally publishes state to MQTT / Home Assistant (issue #50), and pushes characters out over I2C as the bus master. Entry point is `ESPMaster.ino`; feature areas live in sibling `.ino` files which the Arduino/PlatformIO preprocessor concatenates at build time (`ServiceFlapFunctions`, `ServiceWifiFunctions`, `ServiceFileSystemFunctions`, `ServiceFirmwareFunctions`, `ServiceMqttFunctions`, `ServiceWebLog`, `HelpersStringHandling`). Header-only helpers: `HelpersSerialHandling.h` (templates — can't be auto-prototyped), `MqttLogTap.h` (Print subclass streaming log lines to MQTT), `MqttTopicDecoder.h` (pure-logic cmd/* parser + payload validator, host-testable). `ESPMaster.h` declares prototypes for functions with types the Arduino preprocessor can't auto-prototype (namespace-qualified refs like `fs::FS&`).
- **`Unit/Unit.ino`** — Per-flap Arduino Nano firmware. I2C slave whose address is read from four DIP-switch pins (`ADRESSSW1..4`) and offset by `I2C_ADDRESS_BASE` (currently 1) so DIP 0000 → I2C 0x01 (address 0x00 is reserved for general call). Drives a 28BYJ-48 stepper via ULN2003 (`STEPPERPIN1..4`), homes on a KY-003 hall sensor at `HALLPIN`, and applies a per-unit step offset stored in EEPROM. The character alphabet is a fixed `letters[]` array in the sketch — the master sends an index, not the character. Calibration (offset / jog / home) is driven from the master's web UI over I2C opcodes.
- **`UnitBootloader/`** — Vendored + patched [twiboot](https://github.com/orempel/twiboot), an I2C bootloader for the Nanos. Once installed (one-time ICSP flash per unit), the master will be able to push new unit firmware over I2C instead of requiring physical re-flashing. See `UnitBootloader/README.md`. In-progress project tracked in issue #10.

Master/unit contract: I2C index in the `letters[]` table + speed byte, with `ANSWER_SIZE = 1` status byte back. `UNITS_AMOUNT` in `ESPMaster.ino` (default 16) bounds per-unit state arrays on the master — it's set to the DIP-switch hardware max, so users with fewer physical units work unchanged (probe only acts on addresses that actually reply).

## Build / Flash / Test Workflow (PlatformIO)

Each sketch has its own `platformio.ini`. Run all commands from the sketch folder.

```bash
pio run                      # build
pio run -t upload            # flash sketch (USB, one-time)
pio device monitor           # serial at 115200
pio test -e native           # host-side unit tests (ESPMaster only)
```

Subsequent flashes use OTA via the web UI or `flashing/ota-master.sh <fw.bin> http://host:port`.

- **ESPMaster** — env `espmaster`, board `esp01_1m`. Library versions pinned in `platformio.ini`. Builtin `EEPROM` is in `lib_deps` because PIO's LDF doesn't surface it by default. `lib_ignore = AsyncTCP` in `platformio.ini` is required so AsyncMqttClient's ESP32-twin transitive dep (which needs `sdkconfig.h`) doesn't break the ESP8266 build — the real TCP stack is `dvarrel/ESPAsyncTCP`, which every async component on this platform shares.
- **Unit** — env `unit` (board `nanoatmega328new`, new bootloader) or `unit_old_bootloader` (board `nanoatmega328`, old bootloader fallback).

Native test env (`ESPMaster/platformio.ini` → `[env:native]`): compiles pure-logic files for the host using `fabiobatsilva/ArduinoFake` (provides `String`, `Print`, etc.) plus the LinkedList library. Tests live in `ESPMaster/test/test_*/test_main.cpp` and are discovered automatically. `ArduinoFake` stubs `map()` as a fakeit mock — each test's `setUp()` must wire in the real Arduino formula via `When(Method(ArduinoFake(Function), map)).AlwaysDo(...)`, otherwise calling `map()` aborts. Other ArduinoFake-mocked globals (e.g. `EEPROM`) are accessed via the `ArduinoFake(EEPROM)` helper macro and should be re-wired in `setUp()` with `When(Method(...)).AlwaysDo([](...) { ... })`.

Python-side tests live in `ESPMaster/tests/` and cover `build_assets.py` helpers. Run with `python -m pytest tests/` from the `ESPMaster` directory.

CI: `.github/workflows/build.yml` matrix-builds both firmware projects (ESPMaster, Unit) + runs the native test env.

## Configuration Knobs Worth Knowing

All live as `#define`s at the top of `ESPMaster.ino`:

- `UNITS_AMOUNT` — max units the master will track. Default 16 = DIP-switch hardware ceiling. Safe to leave at 16 even with fewer physical units; the probe ignores non-responders.
- `SERIAL_ENABLE` — toggling this to `true` **disables I2C** on the ESP-01 (shared pins). Keep `false` unless deliberately debugging over serial with units disconnected.
- `UNIT_CALLS_DISABLE` — lets the ESP run standalone for web-UI work.
- `WIFI_USE_DIRECT` — `false` = WiFiManager captive portal (default); `true` uses the hard-coded `wifiDirectSsid` / `wifiDirectPassword`.
- `USE_MULTICAST` — enables mDNS at `<mdnsName>.local` (default `split-flap.local`). Pick a unique `mdnsName` per display if running multiple.
- `WIFI_STATIC_IP` — experimental; prefer DHCP reservation on the router.
- `OTA_ENABLE` — enables Arduino OTA; set `otaPassword` if used.
- `FLAP_AMOUNT` / `AMOUNTFLAPS` — hardware-coupled (45). The master's `FLAP_AMOUNT` and the unit's `letters[]` length must agree.

Unit-side: `SERIAL_ENABLE` and `TEST_ENABLE` (cycles a fixed character sequence for homing validation) are commented out by default in `Unit.ino`.

## EEPROM layout versioning

`SettingsEepromLayout.h` carves the master's ESP8266 EEPROM region into named slots. Every time a new slot is added, bump `SETTINGS_VERSION` and extend the `ver < N` migration ladder in `initialiseFileSystem()` so existing blobs upgrade in place (no user-visible settings loss). Reserved slots hold the future slots — shrinking `LEN_RESERVED_2` is how new fields are carved. Current history:
- v1: initial blob (alignment, flapSpeed, deviceMode).
- v2: `timezonePosix` carved from RESERVED_2 (#48).
- v3: MQTT config (`host`, `port`, `user`, `pass`) carved from RESERVED_2 (#50).

Native tests (`test/test_eeprom_settings/test_main.cpp`) enforce layout invariants (contiguity, bounds, round-trip) so a malformed edit fails `pio test -e native` before it ships.

## MQTT architecture invariants

- **Only the master is on MQTT.** Units have no WiFi; per-unit MQTT commands (`cmd/unit/<N>/home` etc.) are master-scoped operations — the master receives them and speaks I2C on behalf of the named unit. `cmd/unit/<N>/bootloader` is deliberately NOT exposed over MQTT (destructive state transition).
- `AsyncMqttClientMessageProperties` can't be auto-prototyped by the Arduino preprocessor, so the `onMessage` callback is bound as a lambda at the registration site (`mqttInit()`) — the preprocessor ignores lambdas.
- Topic parsing + payload validation live in header-only `MqttTopicDecoder.h` so the native test env covers the external-input dispatch path without pulling AsyncMqttClient.
- MQTT is fully opt-in: empty `mqttHostSetting` means no connection attempts and zero runtime cost. Users who don't care about HA see no behaviour change.
- The pre-connect log ring (`WebLog.h`, 512 B) is the fallback for lines emitted before WiFi/MQTT come up. `mqttFlushLogRing()` publishes it as one blob on every reconnect; after that, `MqttLogTap` streams lines live.

## Gotchas

- Editing `.ino` siblings in `ESPMaster/` is editing one translation unit — declarations and `#define`s from `ESPMaster.ino` are visible everywhere, and order of concatenation is alphabetical by file name. The Arduino preprocessor auto-prototypes plain functions but falls over on templates (`<typeprefixerror>`), on signatures with namespace-qualified references like `fs::FS&`, and on library-specific types like `AsyncMqttClientMessageProperties`; add those prototypes manually (header file) or use a lambda at the bind site rather than relying on auto-prototyping.
- The web UI is in PROGMEM (`WebAssets.h`, regenerated by `build_assets.py` on every build), NOT in LittleFS. No separate `uploadfs` step — editing `data/*.html|js|css` is picked up on the next `pio run`.
- The ESP-01 has very little RAM (~42 KB static free at rest). Be conservative adding libraries or large JSON payloads. AsyncMqttClient shares the existing ESPAsyncTCP stack — that's why it fits.
- On the async library choice: `dvarrel` forks of `ESPAsyncTCP` + `ESPAsyncWebSrv` are the maintained ESP8266-focused path (not abandonware — receives real fixes). `mathieucarbou` is the ESP32 successor; wrong target for ESP-01. Don't "modernize" without measuring.
- `#define WEBSERVER_H` before `<ESPAsyncWiFiManager.h>` is a deliberate workaround for the include-order dance between the async WiFi manager and the sync WebServer header — don't "clean up" that include order.
- Test files `#include` the `.ino` sources directly to exercise real code. This means any sibling `.ino` must be compilable standalone in the native env — add explicit forward declarations at the top of a file (e.g. `String cleanString(String);` in `HelpersStringHandling.ino`) if functions are used before their definition in the same file.
