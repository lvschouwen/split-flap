# CLAUDE.md

Guidance for Claude Code in this repository. Current state only — history lives in git and the issue tracker; per-mechanism detail lives in the header comment of the file that owns it.

## Project

Arduino-based split-flap display: a master MCU drives per-flap units over I2C. Firmware builds with **PlatformIO** — every project directory has its own `platformio.ini`; run commands from that directory. CI (`.github/workflows/build.yml`) builds every active firmware project (frozen v1/ESPMaster excluded) and runs every native/pytest suite plus the unit-bundle drift gate.

Two firmware stacks:

- **v1:** `firmware/v1/ESPMaster` (ESP8266 ESP-01 master, **FROZEN** #283 — out of CI and unit-bundle staging, kept as reference; see its README) + `firmware/v1/Unit` (Arduino Nano per flap, **active** — the Nanos stay under the v2 master).
- **v2 (in progress, epic #183):** `firmware/v2/Master` is the ESP32-S3 port (N16R8 devkit) speaking the same I2C protocol to unchanged v1 units. Pure-logic headers in v2 are **copies** of their v1 counterparts, not shared includes — if a bug is found while both trees are alive, fix it in both.

## Repository map

- `firmware/v1/ESPMaster/` — ESP8266 master: async web UI, WiFi portal, NTP, MQTT/HA, I2C master, OTA + recovery
- `firmware/v1/Unit/` — Nano unit: stepper + hall homing, I2C slave, EEPROM offset/address
- `firmware/v1/UnitBootloader/` — vendored+patched twiboot (I2C reflash of units; see its README)
- `firmware/v2/Master/` — S3 master port (plain `.cpp`, console on native USB-CDC)
- `firmware/v2/Rescue/` — break-glass image for the factory slot (#195)
- `firmware/v2/Bootloader/` — builds the S3 second-stage bootloader (#201; see its platformio.ini)
- `flashing/` — `ota-flash.sh` (v2: fetch latest staged bin from a build server via scp, OTA master or rescue, verdict from `/settings`) + `ota-master.sh` (legacy v1 OTA; removed at #285) + `flasher/make_manifest.py` (`stage` writes the unit bundle into `firmware/v2/Master/data` — MUST run between the Unit and v2 Master builds; `gate` = the CI anti-drift check). New-board provisioning = esptool merged-factory-bin recipe in `flashing/README.md`; the Windows flasher exe is retired (#284, exe-only modules frozen in place)
- `PCB/v2/` — design docs (unit board is the only planned custom PCB; GPIO 4 = future reset button)
- `docs/superpowers/specs/` — design docs per feature

## Build / flash / test

```bash
pio run                      # build (run in the project dir)
pio run -t upload            # USB flash (v1 first time; v2 Master devkit)
pio device monitor           # serial 115200
pio test -e native           # host-side unit tests (Unit, v2 Master, Rescue)
python -m pytest tests/      # python-side tests (v2 Master, Rescue)
```

- v1 master re-flash after first install: OTA via `flashing/ota-master.sh <fw.bin> http://host` (prints SUCCESS / EBOOT SILENT REVERT / … verdict); the retired v1.1.0 release exe remains downloadable for USB provisioning of legacy v1 hardware.
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
- **`UnitBus.cpp` is the ONLY Wire toucher on v2, and displayTask its only caller** (SDA 8 / SCL 9, 100 kHz).
- **Twiboot probe-inhibit (v1 #88): displayTask owns a deadline armed by `/unit/reboot` and address burns — every runtime probe waits it out; never bypass it.** Sole documented exception: the reflash job's internal probes (#205, by design — pinned units are immediately flashed + exited).
- **Producer gate (#205): while `reflashInProgress(snapshot.reflash)`, every display-mutating producer stands down (web/MQTT 409, clockTask skips, master OTA 409) EXCEPT `/stop`.**
- **The `storage` partition is ONE shared LittleFS and netTask is its sole flash writer** (producers stage under a mutex) — future storage tenants join it; never carve new partitions.

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

`Unit.ino` (config, globals, setup/loop) + siblings `UnitI2CProtocol.ino` (TWI ISRs + addressing) and `UnitMotion.ino` (stepper + hall calibration) — same one-TU concat model, main sketch placed first. I2C address: four DIP pins + `I2C_ADDRESS_BASE` (DIP 0000 → 0x01; 0x00 reserved for general call), or EEPROM-provisioned address (#56) with DIP fallback — twiboot still listens on the DIP-derived address, so over-I2C reflash requires EEPROM == DIP. 28BYJ-48 via ULN2003, KY-003 hall homing, per-unit EEPROM step offset clamped to ±`SFP_OFFSET_LIMIT_STEPS` on both protocol sides. Revolution odometer (#231): every drum move funnels through `stepCounted()`; ring policy pure in `UnitOdometer.h` (`pio test -e native` now runs in this dir too), EEPROM layout in the Unit.ino header. Calibration/provisioning driven from the master's web UI. Probe quirk: twiboot pinned alive by `isUnitInBootloader()` probe — 1500 ms pre-probe delay is load-bearing.

## v2 — Master (ESP32-S3, epic #183)

Ported slices, one or two lines each: what it is + issue # + the file(s) whose headers carry the mechanism. Specs in `docs/superpowers/specs/`.

- **Tasks (#187):** dual-core FreeRTOS — display domain core 1, network core 0; command queue in, mutex-copied snapshots out. Contract in `Tasks.h`/`Tasks.cpp`; PSRAM-preferred buffers via `LargeAlloc.h`.
- **Settings (#185):** NVS (`splitflap` namespace) behind the `SettingsStore` seam — policy in `Settings.h` (natively tested), `NvsSettingsStore.h` is target-only glue.
- **Web (#186):** v1's `data/` + `build_assets.py` PROGMEM bake; endpoints in `WebEndpoints.cpp` on `esp32async/ESPAsyncWebServer` (async-context rules in its header). Full IANA timezone table baked from `data/zones.csv` → `GET /tz.json` (#252).
- **WiFi (#188):** pure `WifiPolicy.h` join/portal/reboot state machine run by `WifiService.cpp` from netTask; credentials in our NVS, `WiFi.persistent(false)` everywhere — v1's foot-gun class doesn't exist here.
- **OTA (#190):** v1 wire contract (`POST /firmware/master`, mandatory `?md5=`) onto the inactive A/B slot; images boot `PENDING_VERIFY`, confirmed on first netif-up. Detail in `OtaService.h`/`.cpp` + `OtaStatus.h`.
- **Clock/NTP (#192):** `configTzTime` at boot/join/tz-POST (`ClockService.h`); pure `ClockPolicy.h` ticked 1 Hz on core 1 — one command per minute, gated on NTP sync.
- **Status LED (#199):** WS2812 on GPIO 48, pure `StatusLedPolicy.h`, ticked from netTask. (The #198 boot banner with partition diagnostics is owned by `main.cpp`.)
- **Flash log (#206):** LittleFS on the `storage` partition — persistent tee of the serial/web-log stream, served at `GET /log/flash`. Mechanism in `FlashLog.h` + `FlashLogPolicy.h`; writer discipline in Hard rules.
- **Slot confirm records (#200):** per-slot NVS record stamped on first netif-up so Rescue can rank images (the app-descriptor stamp freezes at framework-assembly time under pioarduino builds). Format + rules in `SlotRecord.h` ↔ parse-only copy `RescueSlotRecord.h`.
- **I2C unit bus (#203, slice A):** `UnitBus.cpp`/`.h` (sole Wire toucher — Hard rules) — straight port of v1's blocking transactions and timing; pure seams `FlapFrame.h`, `UnitHealth.h`, plus `UnitProtocolHelpers.h`/`TwibootProtocol.h` v1 copies.
- **Calibration + provisioning (#204, slice B):** every op is a DisplayCommand (`{"seq":N}` → `GET /unit/op-result`); validation pure in `MaintenancePolicy.h`, re-run by displayTask pre-burn; op/abort contracts in `UnitBus.h`, probe-inhibit mechanism in `Tasks.cpp`.
- **Unit reflash over twiboot (#205, slice C):** bundled unit hex (`data/unit-firmware.hex` + `.rev`; `make_manifest.py stage` writes it, CI's `gate` step enforces no drift; v1's committed copy is a frozen fossil since #283) flashed by the one inline `runReflashJob` in `Tasks.cpp`; plan + progress pure in `ReflashPlan.h`; producer gate in Hard rules. `WebAssets.h` is included by `WebEndpoints.cpp` ONLY (a second include duplicates every PROGMEM blob).
- **MQTT + Home Assistant (#224):** v1's wire contract unchanged (23 discovery entities incl. the #231 wear sensor, LWT, five command topics, same device id) on espMqttClient, owned by mqttTask — detail in `MqttService.h`/`.cpp` headers, pure logic in `MqttHelpers.h` + `MqttLifecyclePolicy.h`. Deviations from v1 (no bus polls from the MQTT loop, NVS boot counter, `MQTT_DEVICE_MODEL` build flag) in spec `2026-07-12-v2-mqtt-ha-slice.md`.
- **Transient/mode service (#219):** web transients (clock-mode messages, calibration patterns) ride #224's show-then-revert overlay — drain + producer gates in `WebEndpoints.cpp`, overlay lifecycle (broker-less capable, cross-task arm via `mqttStartNotificationDwell`) in `MqttService.cpp`.
- **Unit wear odometer (#231):** per-unit revolution count read at probe/health-poll (checksummed 5-byte reply — pre-odometer unit fw fails it gracefully); relative-wear flagging pure in `WearPolicy.h`, spliced into `/units/health` + HA wear-warning binary sensor; reset op rides the `{"seq":N}` contract. Unit-side mechanism in `firmware/v1/Unit/UnitOdometer.h`.
- **System tab (#245):** netTask samples S3 vitals dual-rate (#251: 1 s `now`, 5 s decimated history ring) into a mutex-guarded sampler (`SystemStatsPolicy.h` pure + `SystemStats.cpp` glue); `GET /system/stats` = current + ~10 min history; I2C tx/err counters in `UnitBus.cpp` (sketch-protocol traffic only — idle polls and twiboot excluded), MQTT drops, SNTP sync age (`ClockService.cpp`).
- **Identity redesign (#246):** two-leaf animated flap mirror + board-strip skin, `data/style.css`/`script.js` only — selectors preserved, no endpoint changes; mockup-gated per the design-confirm rule.
- **Live display events (#251):** SSE `GET /events` — netTask's `webDisplayEventsTick()` pushes the display text on change (detection pure in `DisplayEvents.h`); the browser mirror riffles through the drum order client-side, with the 5 s `/settings` poll as fallback.
- **Multi-display cluster (epic #270, in progress):** N-row wall of v2 masters over LAN HTTP/JSON — spec `docs/superpowers/specs/2026-07-13-multi-display-cluster-design.md`. Landed: `ClusterLayout.h` (#271) pure grid engine — member-table `{row,col,width}` validation (mirror = coincident members) + wrap/align/slice into pre-positioned segments followers render verbatim; follower side (#272) — `ClusterFollowerPolicy.h` pure state machine (Standalone→Clustered→Grace→LocalFallback, epoch/seq armor, commitAt flip-sync) run by `ClusterFollower.cpp` (NVS membership, netTask-drained renders), `/cluster/{join,render,ping,leave,health}` in `WebEndpoints.cpp`, producer gate = web text/mode + MQTT text/mode 409/drop (transients stay allowed — calibration vehicle), clockTask re-shows the held segment and stands down while a commitAt render is in flight; leader side (#273) — `ClusterLeaderPolicy.h` pure supervision (member-table NVS wire format `host|row|col|width;…` with empty host = own row, join→render→ping scheduling, backoff→degraded→re-join) run by `ClusterLeader.cpp`'s clusterTask (core 0, sole outbound `esp_http_client` caller, 1.5 s timeouts, sequential fan-out, shared commitAt clock incl. the leader's own row), producers reroute LOGICAL grid text via `clusterLeaderSubmit*()` when leading (disabled = byte-identical passthrough), `POST /cluster/config` + `GET /cluster/status`; fake-follower bench harness `tests/fake_follower.py` (#278, pytest-pinned wire contract); discovery backend (#274) — all masters advertise `_splitflap._tcp` (TXT name/rev/width, `WifiService.cpp`), staged `POST/GET /cluster/discover` browse (netTask drain, `/mqtt/discover` contract) with pure `ClusterDiscovery.h`, and the Settings-tab Cluster card (member editor + scan + live pills + rollout progress; follower collapse + Leave — wire strings are text nodes ONLY); cluster clock (#275) rides the #273 clockTask reroute — no separate mechanism; fleet firmware convergence (#276) — rev mismatch in the join handshake (BOTH directions: leader build wins; uncluster to bench-test a follower build) makes clusterTask stream the running slot (`esp_image_verify` length + MD5) into the follower's existing `POST /firmware/master?md5=&v=`, chunked per tick (writes on the 1.5 s LAN timeout; the one finalize round-trip per member = accepted bounded stall), strictly sequential + rejoin-health-gated with 3-attempt cap → `updateBlocked`, sequencing pure in `ClusterRolloutPolicy.h`, rollout state + `imageVerifyFailed` in `/cluster/status`; leader surfacing (#277) — `/events` gains `selfRow`+`rows` (pure `clusterMirrorRows` in `ClusterLayout.h` rebuilds full rows from segments, live self text overlays own slots and beats a coincident twin), pushed only when a grid-generation counter or the own text moves, rendered as the stacked wall mirror in `script.js` (tile size from the widest row, health strip under the own row; the 5 s poll only collapses the wall via `clusterLeading` in `/settings` — wall content relies on SSE reconnect+onConnect resend), and HA gets the leader-only `cluster_degraded` problem sensor (pure `ClusterMqtt.h`, outside MqttHelpers' shared enum: config published while leading, blanked/reconciled on every connect; per-member availability + #276 rollout state as json_attributes), `width` = grid capacity, `text/state` = newline-joined wall; clustered followers publish availability only (state/telemetry stand down while gated); row breaks (#290) — the composer's literal `\n` marker (and a raw newline) forces the next grid row in `clusterWrapRows`, uniformly (blank rows allowed, overflow truncates; standalone temporal paging = #291, unported).
- **Unit-count override / dummy mode (#289):** NVS `unitCount` (0 = auto) pins `displayWidth` over the probe result at every `displayApplyUnitFacts` fold (atomic knob in `Tasks.cpp`, seeded by `tasksInit`, settings drain pushes + queues a Probe to refold live); probe/health/counts stay bus truth. Maintenance-tab "Display width" card; `unitCountOverride` in `/settings`.

Partition table `partitions_splitflap_16MB.csv` is immutable over OTA. `board_upload.arduino.boot_app0` must equal the CSV's otadata offset (0x19000); layout + factory-reset invariants pinned by `tests/test_partition_table.py`.

## v2 — Rescue + factory slot + bootloader

- **Factory slot (#193):** 2 MB `factory` app partition; bootloader factory reset (GPIO 4 low 5 s through reset) erases **otadata only** — never nvs (WiFi credentials must survive).
- **Rescue app (#195):** standalone project sharing nothing compiled with Master except the partition CSV — `Rescue*.h` pure headers are trimmed, natively tested copies. Boot: NVS read-only → 30 s STA join else `<name>-rescue` AP (captive; Ap/Online terminal) → slot inventory + upload-to-app0 (same `?md5=` contract) + `/rescue/exit`. Enter via GPIO 4 or `POST /firmware/rescue-boot` (409 while an install is in flight or the factory image is invalid). Master installs it via raw `esp_partition` writes (`FactorySlot.cpp` — flash sector 0 held back in RAM until the MD5 verdict, pure `FactoryChunkPlan.h`).
- **Custom bootloader (#201):** pioarduino never builds the bootloader from `custom_sdkconfig` — it ships the prebuilt stock one (has rollback, lacks factory reset), so bootloader-side options in any `platformio.ini` are silent no-ops. `firmware/v2/Bootloader/` (framework=espidf) builds ours; config + GPIO 4 constraints in its `sdkconfig.defaults`; artifact committed under `dist/` and swapped in at 0x0 by Master's `use_custom_bootloader.py` (build fails loudly if missing). Deployed boards: one-time `esptool write_flash 0x0 <dist bin>`. Guarded by `tests/test_custom_bootloader.py`. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` stays in Master's AND Rescue's `custom_sdkconfig` — it is app-side load-bearing (esp_ota arms images as NEW). Deliberately excluded (eFuse burners, decided on #201): secure boot, flash encryption, anti-rollback.
