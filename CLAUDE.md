# CLAUDE.md

Guidance for Claude Code in this repository. Current state only — history lives in git and the issue tracker; per-mechanism detail lives in the header comment of the file that owns it.

## Project

Arduino-based split-flap display: a master MCU drives per-flap units over I2C. Firmware builds with **PlatformIO** — every project directory has its own `platformio.ini`; run commands from that directory. CI (`.github/workflows/build.yml`) builds all firmware projects and runs every native/pytest suite.

Two firmware stacks:

- **v1 (shipping, actively maintained):** `firmware/v1/ESPMaster` (ESP8266 ESP-01 master) + `firmware/v1/Unit` (Arduino Nano per flap). Kept stable for OTA maintenance.
- **v2 (in progress, epic #183):** `firmware/v2/Master` is the ESP32-S3 port (N16R8 devkit) speaking the same I2C protocol to unchanged v1 units. Pure-logic headers in v2 are **copies** of their v1 counterparts, not shared includes — if a bug is found while both trees are alive, fix it in both.

## Repository map

- `firmware/v1/ESPMaster/` — ESP8266 master: async web UI, WiFi portal, NTP, MQTT/HA, I2C master, OTA + recovery
- `firmware/v1/Unit/` — Nano unit: stepper + hall homing, I2C slave, EEPROM offset/address
- `firmware/v1/UnitBootloader/` — vendored+patched twiboot (I2C reflash of units; see its README)
- `firmware/v2/Master/` — S3 master port (plain `.cpp`, console on native USB-CDC)
- `firmware/v2/Rescue/` — break-glass image for the factory slot (#195)
- `firmware/v2/Bootloader/` — builds the S3 second-stage bootloader (#201; see its platformio.ini)
- `flashing/` — `ota-master.sh` (v1 OTA with verdict) + `flasher/` (provisioning exe, built by `flasher.yml`; dev: `python -m flasher`, `make_manifest.py stage` MUST run between the Unit and ESPMaster builds)
- `PCB/v2/` — design docs (unit board is the only planned custom PCB; GPIO 4 = future reset button)
- `docs/superpowers/specs/` — design docs per feature

## Build / flash / test

```bash
pio run                      # build (run in the project dir)
pio run -t upload            # USB flash (v1 first time; v2 Master devkit)
pio device monitor           # serial 115200
pio test -e native           # host-side unit tests (ESPMaster, v2 Master, Rescue)
python -m pytest tests/      # python-side tests (ESPMaster, v2 Master, Rescue)
```

- v1 master re-flash after first install: OTA via `flashing/ota-master.sh <fw.bin> http://host` (prints SUCCESS / EBOOT SILENT REVERT / … verdict) or `split-flap-flasher.exe` over USB.
- v1 Unit envs: `unit` (new Nano bootloader) / `unit_old_bootloader` (fallback).
- Native env uses ArduinoFake: `map()` is a fakeit mock — wire the real formula in each test's `setUp()` or calls abort; `EEPROM` etc. re-wire via `ArduinoFake(EEPROM)`.
- v2 first build on a clean machine is slow (pioarduino hybrid compile downloads IDF). `managed_components/`, `sdkconfig.*`, `.dummy/` in v2 project dirs are generated artifacts (gitignored; exception: `Bootloader/sdkconfig.defaults` is a source file kept by a negation).

## Hard rules

- **GPIO 35/36/37 are eaten by octal PSRAM on the S3 — never assign them.** GPIO 4 is reserved (factory-reset button), 19/20 are native USB, 48 is the devkit WS2812.
- **Never `pio run -t upload` the Rescue project.** Install it via Master's `POST /firmware/rescue` or `esptool write_flash 0x830000`.
- **Do NOT port v1's OTA verdict machinery to v2** (RTC cookie / sketchMd5 compare) — the S3's A/B boot makes it obsolete; use `esp_ota_*` state APIs and the core's weak `verifyRollbackLater()`/`verifyOta()` hooks.
- **v2 web/MQTT code never touches display state directly** — enqueue a `DisplayCommand` (params baked in by the sender), read back mutex-copied `DisplaySnapshot`s.
- **The partition table (v2) and EEPROM layout (v1) are per-device truth** — change them only via their documented migration/invariant rules (below).
- Bootloader (v2) is immutable over OTA and has no A/B slot — never write it from the running app; changes are per-board USB flashes.

## v1 — ESPMaster (ESP8266)

Entry point `ESPMaster.ino`; sibling `.ino` files concatenate into ONE translation unit, alphabetically. Consequences:

- Declarations/`#define`s from `ESPMaster.ino` are visible everywhere; manual prototypes go in `ESPMaster.h` (the Arduino auto-prototyper fails on templates and namespace-qualified refs like `fs::FS&`).
- The auto-prototype block lands before any sibling's `#include` — types used in function signatures from later includes (e.g. AsyncMqttClient callbacks) need forward declarations near the top of `ESPMaster.ino`.
- The `<DNSServer.h>` + `<ESPAsyncWiFiManager.h>` includes sit deliberately first in the include block — don't reorder.
- Native tests `#include` the `.ino` sources directly — every sibling must compile standalone (add forward declarations at file top where needed).

Web UI is baked into PROGMEM (`WebAssets.h`, regenerated from `data/` by `build_assets.py` each build — no filesystem, no uploadfs step). Three hash-routed tabs; per-card saves POST only their own fields with `ajax=1`, and the handler gates every field on a "provided" flag so partial posts can't clobber absent ones.

Context rules: async handlers stage, `loop()` mutates (e.g. `/mqtt/discover` arms a flag; blocking `MDNS.queryService()` runs from `loop()`). MQTT callbacks are LWIP-context: copy + flag only, all work in `loopMqtt()`. `initMqtt()` must copy config into stable Strings (AsyncMqttClient stores raw pointers); MQTT password is write-only in `/settings`.

WiFi: SDK-persisted credentials are the single store (no compiled-in credentials; gitignored `WifiCredentials.h` is a migration seed only). Normal boot tries them 30 s, then opens the `<deviceName>-setup` portal. `/reset-wifi` erases the sector. Static IPs unsupported — DHCP reservation. **Gotcha:** `WiFi.persistent(false)` must precede `WiFi.disconnect()` or the stored config gets zeroed.

Identity: every network name derives from EEPROM `deviceName`, else `split-flap-<hex chip id>`; resolved in `resolveDeviceIdentity()` right after the single `initialiseSettings()` at the top of `setup()`.

Settings/EEPROM: `SettingsEepromLayout.h` documents the slots; native `test_eeprom_settings` enforces the invariants. New slot ⇒ bump `SETTINGS_VERSION`, extend the migration ladder in `initialiseSettings()` (carve space from `RESERVED_2`), end the migration step **before** the version write. Every `save*()` calls `updateSettingsCrc()` before `EEPROM.commit()`. Boot-time blob handling is the pure `assessSettingsBlob()` decision table; the CRC range is deliberately version-independent.

OTA + recovery: `/firmware/master` (multipart + `?md5=`) → staging slot → pre-flash sketch MD5 stashed in RTC (`RtcBootState`, magic-checked) → next boot writes `lastFlashResult` `"ok"`/`"reverted"`. A silent revert with a good upload is almost always ESP-01 power sag — hardware fix, not firmware. Recovery mode: 3 unhealthy boots (RTC counter) or `POST /firmware/recover-mark`; serves upload-only endpoints on known WiFi or `<deviceName>-rec` AP.

Knobs (top of `ESPMaster.ino`): `UNITS_AMOUNT` (16, DIP ceiling — array bound only; runtime width comes from the boot probe, `DisplayWidth.h`), `SERIAL_ENABLE` (**disables I2C** — shared pins; doubles as ESP-standalone web debug), `USE_MULTICAST` (mDNS), `OTA_ENABLE` (ArduinoOTA), `FLAP_AMOUNT`/`AMOUNTFLAPS` (45, must match the unit's `letters[]` length). ESP-01 has ~37 KB free RAM at rest — be conservative; the `dvarrel` async forks are the right ESP8266 stack (don't "modernize").

Master/unit I2C contract: `letters[]` index + speed byte out, 1 status byte back. Units must be DIP-addressed contiguously from 0x01; `showMessage()` snapshots the width per call because `/reflash-units` can re-probe mid-call.

## v1 — Unit (Nano)

`Unit.ino` (config, globals, setup/loop) + siblings `UnitI2CProtocol.ino` (TWI ISRs + addressing) and `UnitMotion.ino` (stepper + hall calibration) — same one-TU concat model, main sketch placed first. I2C address: four DIP pins + `I2C_ADDRESS_BASE` (DIP 0000 → 0x01; 0x00 reserved for general call), or EEPROM-provisioned address (#56) with DIP fallback — twiboot still listens on the DIP-derived address, so over-I2C reflash requires EEPROM == DIP. 28BYJ-48 via ULN2003, KY-003 hall homing, per-unit EEPROM step offset clamped to ±`SFP_OFFSET_LIMIT_STEPS` on both protocol sides. Calibration/provisioning driven from the master's web UI. Probe quirk: twiboot pinned alive by `isUnitInBootloader()` probe — 1500 ms pre-probe delay is load-bearing.

## v2 — Master (ESP32-S3, epic #183)

Ported slices (state, not story — specs in `docs/superpowers/specs/`, detail in file headers):

- **Tasks (#187):** dual-core FreeRTOS — display domain core 1 (`displayTask` will own I2C), network core 0. Command queue in, snapshot out (see Hard rules). Large buffers via `largeAlloc()` (PSRAM-preferred).
- **Settings (#185):** NVS (`splitflap` namespace) behind the `SettingsStore` seam; policy in `Settings.h` (natively tested), `NvsSettingsStore.h` is target-only glue.
- **Web (#186):** v1's `data/` + `build_assets.py` PROGMEM bake; endpoints in `WebEndpoints.cpp` on `esp32async/ESPAsyncWebServer`; unported services answer 501.
- **WiFi (#188):** credentials in our NVS (`wifiSsid`/`wifiPass`); esp_wifi storage stays RAM (`WiFi.persistent(false)` everywhere — v1's foot-gun class doesn't exist here). Pure `WifiPolicy.h` state machine (30 s join → 300 s `<name>-setup` portal → reboot-retry) run by `WifiService.cpp` from netTask; hand-rolled portal (`data/portal.html`, DNS catch-all); `webEndpointsStart()` idempotent, called by whichever netif is first; mDNS on join.
- **OTA (#190):** `POST /firmware/master` keeps the v1 wire contract (multipart + mandatory `?md5=`); `Update` writes the inactive A/B slot; images boot `PENDING_VERIFY` (strong `verifyRollbackLater()` override in `OtaService.cpp`); `otaHealthConfirm()` on first netif-up cancels rollback. `/settings` verdict fields are synthesized per-boot from `esp_ota` state (`OtaStatus.h`) — only `intendedVersion` persists.
- **Clock/NTP (#192):** `configTzTime` at boot + on join + rebootless on tz POST; `clockTask` (core 1, 1 Hz) with pure `ClockPolicy.h` — dedups against the display snapshot + `lastQueued` (one command per minute), gated on `time() >= 1e9` so an unsynced clock holds content.
- **Status LED (#199):** WS2812 on GPIO 48, pure `StatusLedPolicy.h`, ticked from netTask. Boot banner prints partition diagnostics (#198).
- **Flash log (#206):** the `storage` partition is LittleFS-mounted by `FlashLog.cpp` — persistent tee of the SerialPrint/web-log stream (`log.txt`/`log.prev.txt`, 1 MB each; per-line stamps `[HH:MM:SS]` after NTP else `[+secs.ms]` since boot; day-marker lines; boot marker written synchronously in setup). Producers stage under a mutex; **netTask is the only flash writer** (`flashLogTick` from `webEndpointsLoop`, open→append→close per flush; handlers never write flash — clear is staged). Served at `GET /log/flash` (+`?prev=1`) on the Logs tab. Future storage tenants share this LittleFS — never carve new partitions.
- **Slot confirm records (#200):** the app-descriptor version/date/time **freeze at framework-assembly time** under pioarduino hybrid builds — useless for ordering images. Master stamps a per-slot NVS record on first netif-up (`slotRec0`/`slotRec1`, format + rules in `SlotRecord.h` ↔ parse-only copy `RescueSlotRecord.h`; wire-contract-like, version it). Rescue ranks `/rescue/exit` by it after sha256-matching the slot image.
- **I2C unit bus (#203, slice A of the I2C port):** `UnitBus.cpp` is the ONLY Wire toucher (SDA 8 / SCL 9, 100 kHz), called from displayTask exclusively — straight port of v1's blocking transactions and timing (2 ms opcode settle, 30 s stuck-unit timeout, 1500 ms twiboot pre-probe delay before the boot scan). Pure seams: `FlapFrame.h` (text→letter indices; deviation from v1: unknown char → blank instead of skip), `UnitHealth.h` (`UnitFacts` live in the `DisplaySnapshot`; web renders health JSON from its mutex copy — no v1-style cached JSON), `UnitProtocolHelpers.h`/`TwibootProtocol.h` v1 copies. `/units/health/refresh` always enqueues Probe (re-scan + health poll in one pass; `?probe=1` accepted, changes nothing).
- **Calibration + provisioning (#204, slice B):** every op is a DisplayCommand (queue-native; POSTs answer `{"seq":N}`, `GET /unit/op-result?seq=` serves the snapshot's single `MaintResult` slot: pending/ok/failed/expired — one awaited critical op at a time, Stop deliberately supersedes). Offset is a probe-time `UnitFact` (`GET /unit/offset` serves from the snapshot; 502 until probed). Validation is pure `MaintenancePolicy.h`, run at the web boundary AND re-run by displayTask pre-burn. Set/clear-address are compound ops: burn → settle → reprobe → postcondition-graded (`ok` = observed on the bus). **Twiboot guard (v1 #88): displayTask owns a probe-inhibit deadline armed by `/unit/reboot` and the address burns — every runtime probe waits it out; never bypass it (sole documented exception: the reflash job's internal probes, slice C below).** `/stop` = atomic abort flag in UnitBus (set BEFORE enqueue, rolled back on 503 — order is load-bearing) + broadcast home + retained-text clear; settle windows are bus safety, never abort-shortened. `/reset-units` bakes text/alignment/speed at enqueue.
- **Unit reflash over twiboot (#205, slice C):** the master bundles the unit firmware (`data/unit-firmware.hex` + `.rev`, committed; `make_manifest.py stage` writes BOTH master trees, gates enforce no drift) as `UNIT_FIRMWARE_BIN`/`BUNDLED_UNIT_REV` — health grades `fw` for real. `ReflashUnits` is the one long-running job, run INLINE by displayTask (`runReflashJob` in `Tasks.cpp`; plan + progress pure in `ReflashPlan.h`): sweep → 500 ms twiboot settle → rescan → batched flash (2 at a time, 15 s homing settle, v1 #138) → final reprobe → baked re-show. **Producer gate:** while `reflashInProgress(snapshot.reflash)` every display-mutating producer stands down (web/MQTT 409, clockTask skips, master OTA 409) EXCEPT `/stop` — the cancel (abort polled between pages/units; aborted/failed units stay in twiboot deliberately: pinned, can't boot a torn image, recovered by boot auto-install or retry). The job's internal probes bypass the probe-inhibit deadline BY DESIGN — every pinned unit is immediately flashed + exited. Boot runs the same job with the narrower outdated-only sweep (unknown revs are web-job-only, v1 #114). Progress rides `/units/health` (`reflash` key); job outcome via `/unit/op-result`. `WebAssets.h` is included by WebEndpoints.cpp ONLY (a second include duplicates every PROGMEM blob) — displayTask uses the `webUnitFirmwareBin()` accessors.

Partition table `partitions_splitflap_16MB.csv` is immutable over OTA. `board_upload.arduino.boot_app0` must equal the CSV's otadata offset (0x19000); layout + factory-reset invariants pinned by `tests/test_partition_table.py`.

## v2 — Rescue + factory slot + bootloader

- **Factory slot (#193):** 2 MB `factory` app partition; bootloader factory reset (GPIO 4 low 5 s through reset) erases **otadata only** — never nvs (WiFi credentials must survive).
- **Rescue app (#195):** standalone project sharing nothing compiled with Master except the partition CSV — `Rescue*.h` pure headers are trimmed, natively tested copies. Boot: NVS read-only → 30 s STA join else `<name>-rescue` AP (captive; Ap/Online terminal) → slot inventory + upload-to-app0 (same `?md5=` contract) + `/rescue/exit`. Enter via GPIO 4 or `POST /firmware/rescue-boot` (409 while an install is in flight or the factory image is invalid). Master installs it via raw `esp_partition` writes (`FactorySlot.cpp` — flash sector 0 held back in RAM until the MD5 verdict, pure `FactoryChunkPlan.h`).
- **Custom bootloader (#201):** pioarduino never builds the bootloader from `custom_sdkconfig` — it ships the prebuilt stock one (has rollback, lacks factory reset), so bootloader-side options in any `platformio.ini` are silent no-ops. `firmware/v2/Bootloader/` (framework=espidf) builds ours; config + GPIO 4 constraints in its `sdkconfig.defaults`; artifact committed under `dist/` and swapped in at 0x0 by Master's `use_custom_bootloader.py` (build fails loudly if missing). Deployed boards: one-time `esptool write_flash 0x0 <dist bin>`. Guarded by `tests/test_custom_bootloader.py`. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` stays in Master's AND Rescue's `custom_sdkconfig` — it is app-side load-bearing (esp_ota arms images as NEW). Deliberately excluded (eFuse burners, decided on #201): secure boot, flash encryption, anti-rollback.
