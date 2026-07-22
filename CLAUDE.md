# CLAUDE.md

Guidance for Claude Code in this repository. Current state only — history lives in git and the issue tracker; per-mechanism detail lives in the header comment of the file that owns it. Issue numbers (#N) are pointers into that history, not narrative.

## Project

Arduino-based split-flap display: a master MCU drives per-flap units over I2C. Firmware builds with **PlatformIO** — every project directory has its own `platformio.ini`; run commands from that directory. CI (`.github/workflows/build.yml`) builds every active firmware project (frozen v1/ESPMaster excluded) and runs every native/pytest suite plus the unit-bundle and copied-header drift gates (`tests/test_copied_headers.py` — its manifest and the copied-header lists here must stay in sync).

- **v1 is frozen** — only `firmware/v1/ESPMaster` remains, as reference. **Never edit anything under `firmware/v1`.**
- **v2 is the live stack:** `Master` (ESP32-S3 N16R8 devkit), `Unit` (Arduino Nano per flap), `FollowerEsp01` (ESP-01 "dumb row" under an S3 leader). Pure-logic headers are **copies** across the v2 projects, not shared includes — fix a copied-header bug in every tree that carries it.

## Repository map

- `firmware/v2/Master/` — S3 master (plain `.cpp`, console on native USB-CDC)
- `firmware/v2/Unit/` — Nano unit: stepper + hall homing, I2C slave, EEPROM offset/address
- `firmware/v2/shared/` — `SplitFlapProtocol.h`, the master↔unit I2C contract (`-I ../shared` from `firmware/v2/Unit`)
- `firmware/v2/UnitBootloader/` — vendored+patched twiboot (I2C reflash of units; see its README)
- `firmware/v2/FollowerEsp01/` — ESP-01 cluster follower "dumb row" (#298; own section below)
- `firmware/v2/Rescue/` — break-glass image for the factory slot (#195)
- `firmware/v2/Bootloader/` — builds the S3 second-stage bootloader (#201; see its platformio.ini)
- `firmware/v1/ESPMaster/` — FROZEN ESP8266 master reference
- `flashing/` — `ota-flash.sh` (scp-fetch staged bin + OTA + `/settings` verdict; platform autodetect via the `plat` settings key, `-p esp01|esp32` to assert it; multi-device fan-out) and `flasher/make_manifest.py` (`stage` writes the unit bundle into `firmware/v2/Master/data` AND `firmware/v2/FollowerEsp01/data` — MUST run between the Unit build and those builds; `gate` = the CI anti-drift check). New-board provisioning: esptool merged-factory-bin recipe in `flashing/README.md`.
- `PCB/v2/` — design docs (unit board is the only planned custom PCB; GPIO 4 = future reset button)
- `docs/superpowers/specs/` — design docs per feature
- `docs/CODING_STANDARDS.md` — how code must be written (architecture, C++ style, concurrency, security, testing, tooling gates); CLAUDE.md Hard rules win on conflict

## Build / flash / test

```bash
pio run                      # build (run in the project dir)
pio run -t upload            # USB flash (Nano unit first time; v2 Master devkit)
pio device monitor           # serial 115200
pio test -e native           # host-side unit tests (Unit, v2 Master, Rescue, FollowerEsp01)
python -m pytest tests/      # python-side tests (v2 Master, Rescue, FollowerEsp01)
```

- Unit envs (`firmware/v2/Unit`): `unit` (new Nano bootloader) / `unit_old_bootloader` (fallback).
- Native env uses ArduinoFake: `map()` is a fakeit mock — wire the real formula in each test's `setUp()` or calls abort; `EEPROM` etc. re-wire via `ArduinoFake(EEPROM)`.
- v2 first build on a clean machine is slow (pioarduino hybrid compile downloads IDF). `managed_components/`, `sdkconfig.*`, `.dummy/` in v2 project dirs are generated artifacts (gitignored; exception: `Bootloader/sdkconfig.defaults` is a source file kept by a negation).

## Per-change workflow (overrides the global "stop the app" flow — there is no local app)

The firmware runs on bench hardware reached by OTA over the user's VPN; there is nothing to "stop" before editing. Per change:

1. **Issue first** with `effort:`/`gain:` labels; no private data (public repo). Interactive one-liners may go direct to `master`.
2. **One branch + PR per session arc**, not per issue — batch related issues onto it. (A stage-commit PR is never squash-merged; bundle drift is history.)
3. **Build + native tests green** before commit: `pio run` in each touched project dir, `pio test -e native`, `python -m pytest tests/`. Editing v2 firmware source auto-triggers that project's native suite via the local PostToolUse hook (`.claude/hooks/firmware-native-test.sh`). If Unit fw changed: rebuild Unit clean → `make_manifest.py stage` → rebuild Master + FollowerEsp01 (drift gate) → **separate** artifact commit (never amend the bundle in).
4. **Risk-tiered review** — cpp-reviewer over the combined branch diff; ALWAYS for OTA / boot / flash / concurrency / credentials / cluster-wire, batched otherwise.
5. **Bench-verify = the E2E tier** — stage the bin to `~/bench-bins/`, `ota-flash.sh` to the board, confirm on hardware. Host tests cover pure logic only; hardware glue is proven on the bench. OTA the LEADER first (an older-build leader downgrades followers via the rollout).
6. **Commit + close issue** — conventional message referencing it; push; confirm the GitHub auto-close fired (it has silently failed — close manually if not).
7. **Update memory.**

## Release policy (CalVer `vYYYY.MM.DD`, ≤1/day)

The fleet converges on the git **REV** (`git describe`/short SHA baked into the binary), never on the tag — so a tag has no operational role; it is a human changelog + rollback anchor only. There is **no version file**: a "release" = an annotated git tag + a GitHub release with notes.

- **Versioning is CalVer `vYYYY.MM.DD`** (`v`-prefixed for continuity with the old `v2.3.0` tags), **at most one release per day** — the date IS the version, so no within-day counter. Fix-only days accumulate untagged on `master` (the fleet still runs specific REVs); cut a dated release only when the day's diff is worth announcing or anchoring. Never tag inside a feature/fix commit.
- **Breaking changes aren't in the version string** (CalVer has no MAJOR signal) — flag them in the release: title `vYYYY.MM.DD — BREAKING`, plus a `## Breaking / Operator action required` notes section, for any change to the I2C/cluster wire, NVS/EEPROM layout, partition table, OTA contract, config semantics, or minimum bootloader.
- **Same-day critical fix** (a release already shipped today): OTA-deploy the fixed REV now (the fleet needs no tag) and cut the dated release the next day. `vYYYY.MM.DD-hotfix.N` is break-glass only, when a same-day rollback anchor is genuinely needed.
- The v2 release line was retro-converted to CalVer on 2026-07-22 (v2.0.0→`v2026.07.18`, v2.1.0→`v2026.07.20`, v2.2.0→`v2026.07.20-hotfix.1`, v2.3.0→`v2026.07.22`, commits unchanged); the frozen v1 tags (`v1.x`) stay semver. Mixed CalVer/semver tags don't version-sort, so always set the GitHub release latest explicitly (`gh release create --latest`). Firmware identity stays the baked REV — never parse meaning from the tag. Going forward, don't retag/rewrite a shipped release — that migration was a deliberate one-time exception.

## Hard rules

- **GPIO 35/36/37 are eaten by octal PSRAM on the S3 — never assign them.** GPIO 4 is reserved (factory-reset button), 19/20 are native USB, 48 is the devkit WS2812.
- **Never `pio run -t upload` the Rescue project.** Install it via Master's `POST /firmware/rescue` or `esptool write_flash 0x830000`.
- **Do NOT port v1's OTA verdict machinery to v2** (RTC cookie / sketchMd5 compare) — the S3's A/B boot makes it obsolete; use `esp_ota_*` state APIs and the core's weak `verifyRollbackLater()`/`verifyOta()` hooks.
- **v2 web/MQTT code never touches display state directly** — enqueue a `DisplayCommand` (params baked in by the sender), read back mutex-copied `DisplaySnapshot`s.
- **The partition table (S3) and EEPROM layout (Nano unit) are per-device truth** — change them only via their documented migration/invariant rules (below).
- Bootloader (v2) is immutable over OTA and has no A/B slot — never write it from the running app; changes are per-board USB flashes.
- **`UnitBus.cpp` is the ONLY Wire toucher on v2, and displayTask its only caller** (SDA 8 / SCL 9, 100 kHz).
- **Twiboot probe-inhibit (v1 #88): displayTask owns a deadline armed by `/unit/reboot` and address burns — every runtime probe waits it out; never bypass it.** Sole documented exception: the reflash job's internal probes (#205, by design — pinned units are immediately flashed + exited).
- **Producer gate (#205): while `reflashInProgress(snapshot.reflash)`, every display-mutating producer stands down (web/MQTT 409, clockTask skips, master OTA 409) EXCEPT `/stop`.**
- **The `storage` partition is ONE shared LittleFS and netTask is its sole flash writer** (producers stage under a mutex) — future storage tenants join it; never carve new partitions.

## Unit (Nano — `firmware/v2/Unit`)

`Unit.ino` (config, globals, setup/loop) + siblings `UnitI2CProtocol.ino` (TWI ISRs + addressing) and `UnitMotion.ino` (stepper + hall calibration) — a one-TU concat model, main sketch placed first. I2C address: four DIP pins + `I2C_ADDRESS_BASE` (DIP 0000 → 0x01; 0x00 reserved for general call), or EEPROM-provisioned address with DIP fallback — twiboot still listens on the DIP-derived address, so over-I2C reflash requires EEPROM == DIP. 28BYJ-48 via ULN2003, KY-003 hall homing, per-unit EEPROM step offset clamped to ±`SFP_OFFSET_LIMIT_STEPS` on both protocol sides. Revolution odometer (#231): every drum move funnels through `stepCounted()`; ring policy pure in `UnitOdometer.h` (`pio test -e native` runs here too), EEPROM layout in the Unit.ino header. Calibration/provisioning driven from the master's web UI. Probe quirk: twiboot pinned alive by `isUnitInBootloader()` probe — the 1500 ms pre-probe delay is load-bearing.

## v2 — Master (ESP32-S3)

One line per mechanism: what it is + issue # + the file(s) whose headers carry the detail. Specs in `docs/superpowers/specs/`.

- **Tasks (#187):** dual-core FreeRTOS — display domain core 1, network core 0; command queue in, mutex-copied snapshots out. Contract in `Tasks.h`; TU family (#352): `Tasks.cpp` (IPC + lifecycle + core-0 wrappers), `DisplayTask.cpp` (core-1 display domain, per-opcode exec* helpers), `ClockTask.cpp` (1 Hz ticker), seams in `TasksInternal.h` (family-only include). PSRAM-preferred buffers via `LargeAlloc.h`.
- **Settings (#185):** NVS (`splitflap` namespace) behind the `SettingsStore` seam — policy in `Settings.h` (natively tested), `NvsSettingsStore.h` is target-only glue.
- **Web (#186/#338):** `data/` + `build_assets.py` PROGMEM bake on `esp32async/ESPAsyncWebServer`. TU family: `WebEndpoints.cpp` = shared state + init/loop core (async-context rules in its header; drain order in `webEndpointsLoop` is load-bearing), route handlers in `WebContent`/`WebSettings`/`WebSystem`/`WebFirmware`/`WebMaintenance`/`WebCluster`.cpp, internals via `WebEndpointsInternal.h` (include from Web*.cpp ONLY). `WebAssets.h` is included by `WebContent.cpp` ONLY (a second include duplicates every PROGMEM blob). IANA tz table baked from `data/zones.csv` → `GET /tz.json`.
- **WiFi (#188):** pure `WifiPolicy.h` join/portal/reboot state machine run by `WifiService.cpp` from netTask; credentials in our NVS, `WiFi.persistent(false)` everywhere.
- **OTA (#190/#305):** `POST /firmware/master` with mandatory `?md5=` onto the inactive A/B slot; images boot `PENDING_VERIFY`, confirmed **pre-inrush at the end of `setup()`** (netif-up is a fallback bar only — a later confirm loses to the boot-inrush brownout). Detail in `OtaService.h`/`.cpp` + `OtaStatus.h`.
- **Clock/NTP (#192):** `configTzTime` at boot/join/tz-POST (`ClockService.h`); pure `ClockPolicy.h` ticked 1 Hz on core 1 — one command per minute, gated on NTP sync.
- **Status LED (#199):** WS2812 on GPIO 48, pure `StatusLedPolicy.h`, ticked from netTask. Boot banner with partition diagnostics owned by `main.cpp`.
- **Flash log (#206):** LittleFS tee of the serial/web-log stream on the `storage` partition, served at `GET /log/flash` — `FlashLog.h` + `FlashLogPolicy.h`; writer discipline in Hard rules.
- **Slot confirm records (#200):** per-slot NVS record stamped on first netif-up so Rescue can rank images (the app-descriptor stamp freezes at framework-assembly time under pioarduino). `SlotRecord.h` ↔ parse-only copy `RescueSlotRecord.h`.
- **I2C unit bus (#203):** `UnitBus.cpp`/`.h` (sole Wire toucher — Hard rules), blocking transactions; pure seams `FlapFrame.h`, `UnitHealth.h`, `UnitProtocolHelpers.h`, `TwibootProtocol.h`.
- **Calibration + provisioning (#204):** every op is a DisplayCommand (`{"seq":N}` → `GET /unit/op-result`); validation pure in `MaintenancePolicy.h`, re-run by displayTask pre-burn; op/abort contracts in `UnitBus.h`; probe-inhibit in `DisplayTask.cpp`.
- **Unit reflash over twiboot (#205):** bundled unit hex (`data/unit-firmware.hex` + `.rev`; staged by `make_manifest.py`, CI drift-gated) flashed by `runReflashJob` in `DisplayTask.cpp`; plan + progress pure in `ReflashPlan.h`; producer gate in Hard rules.
- **MQTT + Home Assistant (#224):** 23 discovery entities, LWT, five command topics on espMqttClient, owned by mqttTask — `MqttService.h`/`.cpp`, pure logic in `MqttHelpers.h` + `MqttLifecyclePolicy.h`; spec `2026-07-12-v2-mqtt-ha-slice.md`.
- **Transient/mode service (#219):** web transients ride the MQTT show-then-revert overlay — drain in `WebEndpoints.cpp`, producer gates in `WebSettings.cpp`, overlay lifecycle (broker-less capable, cross-task arm via `mqttStartNotificationDwell`) in `MqttService.cpp`.
- **Unit wear odometer (#231):** per-unit revolution count read at probe/health-poll (checksummed 5-byte reply — pre-odometer unit fw fails it gracefully); relative-wear flagging pure in `WearPolicy.h` → `/units/health` + HA wear sensor; reset rides the `{"seq":N}` contract.
- **System tab (#245/#251):** netTask samples S3 vitals dual-rate (1 s now, 5 s history ring) — `SystemStatsPolicy.h` pure + `SystemStats.cpp` glue; `GET /system/stats`; I2C tx/err counters in `UnitBus.cpp` (sketch-protocol traffic only — idle polls and twiboot excluded).
- **Web identity (#246):** two-leaf animated flap mirror + board-strip skin, `data/style.css`/`script.js` only. UI changes are mockup-gated (design-confirm rule).
- **Live display events (#251):** SSE `GET /events` — netTask pushes display text on change (pure `DisplayEvents.h`); browser riffles client-side; 5 s `/settings` poll is the fallback.
- **Unit-count override / dummy mode (#289):** NVS `unitCount` (0 = auto) pins `displayWidth` over the probe result at every `displayApplyUnitFacts` fold (atomic knob in `Tasks.cpp`, folds in `DisplayTask.cpp`); probe/health/counts stay bus truth. `unitCountOverride` in `/settings`.
- **Headless mode (#329):** NVS `deviceRole` forces `displayWidth` 0 for headless roles — vocabulary in `HeadlessPolicy.h`, fold in `DisplayTask.cpp`; width-0 off-grid cluster members, mode-surviving failover, vitals in `/settings`.
- **Watchdogs (#314/#328):** 30 s TWDT, all app tasks subscribe + FEED-INSIDE long ops (never unsubscribe) — `TaskWatchdog.h`; WiFi reconnect watchdog pure in `WifiPolicy.h`.

Partition table `partitions_splitflap_16MB.csv` is immutable over OTA. `board_upload.arduino.boot_app0` must equal the CSV's otadata offset (0x19000); layout + factory-reset invariants pinned by `tests/test_partition_table.py`.

### Cluster (epic #270)

N-row wall of v2 masters over LAN HTTP/JSON — spec `2026-07-13-multi-display-cluster-design.md`; single pane spec `2026-07-14-cluster-single-pane-design.md`; follower relay spec `2026-07-14-v2-esp01-follower-firmware-relay.md`.

- **Grid engine (#271/#290):** pure `ClusterLayout.h` — member-table `{row,col,width}` validation (mirror = coincident members), wrap/align/slice into pre-positioned segments followers render verbatim; a literal `\n` marker (or raw newline) forces the next grid row in `clusterWrapRows` (blank rows allowed, overflow truncates).
- **S3 follower side (#272):** pure `ClusterFollowerPolicy.h` phase machine (Standalone→Clustered→Grace→LocalFallback; epoch/seq armor, commitAt flip-sync) run by `ClusterFollower.cpp` (NVS membership, netTask-drained renders); `/cluster/*` routes in `WebCluster.cpp`; producer gate = web/MQTT text+mode 409/drop (transients stay allowed); clockTask stands down while a commitAt render is in flight.
- **Leader side (#273/#275):** pure `ClusterLeaderPolicy.h` supervision (NVS member-table `host|row|col|width;…`, empty host = own row; join→render→ping scheduling, backoff→degraded→re-join) run by the `ClusterLeader*.cpp` TU family (#352: core/Grid/Fanout/Rollout/FollowerPush/Maintenance/Status, seams in `ClusterLeaderInternal.h`, family-only include) on clusterTask — core 0, sole outbound `esp_http_client` caller, 1.5 s LAN timeouts, sequential fan-out, shared commitAt clock incl. the leader's own row. Producers reroute LOGICAL grid text via `clusterLeaderSubmit*()` when leading (disabled = byte-identical passthrough); the clock rides the same reroute. `POST /cluster/config`, `GET /cluster/status`.
- **Discovery (#274):** every master advertises `_splitflap._tcp` (TXT name/rev/width/plat, `WifiService.cpp`); staged `POST/GET /cluster/discover` browse, pure `ClusterDiscovery.h`; Settings-tab Cluster card (member editor + scan + pills; wire strings are text nodes ONLY).
- **Fleet firmware convergence (#276/#297/#340/#344):** a member rev ≠ the image rev converges it, BOTH directions — the leader build wins; uncluster to bench-test a follower build. Sequencing pure in `ClusterRolloutPolicy.h`: strictly sequential, rejoin-health-gated, 3-attempt cap → `updateBlocked` (cleared by config swap, leader reboot, or the member's reported rev changing). Chunk writes use the dedicated 8 s stream timeout (`CLUSTER_ROLLOUT_STREAM_TIMEOUT_MS`; ping/render keep 1.5 s), the pump wdtFeeds between writes and wall-clock-bounds each tick. Platform guard: foreign-plat members never get the S3 slot (`clusterMemberPlatForeign`); esp01 rows converge from the STORED follower image instead — same machine, `rolloutFollowerSource` routes the file-source stream, the rejoin gate compares the stored rev, additive `rollout.src:"esp01"` in status.
- **Wall surfacing (#277):** `/events` carries `selfRow`+`rows` (pure `clusterMirrorRows` rebuilds full rows; live self text overlays own slots) → stacked wall mirror in `script.js`; the 5 s poll only collapses the wall via `clusterLeading` — wall content relies on SSE resend. HA: leader-only `cluster_degraded` problem sensor (pure `ClusterMqtt.h`; per-member availability + rollout state as attributes); clustered followers publish availability only.
- **Single pane (#294):** the ping piggybacks BOTH ways (pure `ClusterDigest.h`) — per-row unit health up on join/ping replies (`faultMask` hex bitmap + wear + rev refresh → `clusterStatusJson`, health strips, HA attrs), the cluster digest down on the ping body (`digest=`+`you=`; follower stores raw, serves `GET /cluster/digest`, renders the wall read-only; promote-critical table+selfIndex persist to NVS on change). Member pills/⚙ = browser fan-out straight to the member's `/settings`+`/units/health`.
- **Security (#313 + HMAC):** CORS/CSRF middleware ENFORCES — a mutating POST with a non-LAN `Origin` is 403'd (`clusterCsrfRejectPost`, copy `followerCsrfRejectPost`; upload routes that write flash in `onUpload` gate INLINE at `index==0` via `webUploadCsrfRejected`); leader-wire endpoints are source-IP-bound to the leader (`/cluster/leave` = leader-IP-OR-LAN-browser so the local button works); member `host` must be a LAN target (`clusterHostIsLanTarget` — SSRF guard) + `disable_auto_redirect` on every client; 30 s OTA stall watchdog on `/firmware/master`; join persists membership only on real change (flash-flood guard). HMAC layer above the IP binding: per-member 256-bit key minted at join (`key=`), then every leader-wire request REQUIRES `ts`+`mac` = HMAC-SHA256(key, canonical) — render signs content, ping signs ts+sha256(digest)+`you`, leave signs ts; auto-negotiates by key presence; replay bounded by ±30 s NTP window AND a monotonic per-member ts mark (persisted ~1/h via `clusterHmacMarkNeedsPersist`, mark-before-key ordering on the S3); key rotates on leader reboot. Crypto = one portable SHA-256 in `ClusterHmac.h`, byte-identical copy in FollowerEsp01, vector-tested by `test_cluster_hmac`; the S3 HW SHA accelerator is deliberately NOT used (single testable path).
- **Failover (#295/#332):** `POST /cluster/promote` on a LocalFallback follower runs the pure promote table transform (self↔dead-leader swap). Sticky leadership: a fresh-contact clustered follower answers a foreign join 409 `other-leader`; a leader collecting that marker demotes (table wipe, leave fan-out suppressed). Automatic takeover stays a declined design. Succession ranking: additive `role` join/ping key → `clusterSuccessorTier()` in `ClusterDigest.h` (backup > rendering > spare > monitor).
- **Follower management + image relay (#304):** the member ⚙ panel drives the follower's own `{"seq":N}` ops and `/reflash-units` (rung-3 CORS surface on BOTH copies). The S3 stores ONE `follower-*.bin` on the shared LittleFS (`POST /cluster/follower-firmware` → PSRAM accumulate + mandatory MD5 → netTask writes `/follower-fw.bin`+`.rev`) and streams it to an esp01 row via `POST /cluster/member/update` — mutually exclusive with the auto-rollout (shared `rolloutBuf`; `followerImageTryClaimRelay` makes the file claim atomic vs the netTask flush). Pure guards in `FollowerImagePolicy.h` (esp01-only — never the S3 slot at a follower), store glue `FollowerImageStore.*`, state in `/cluster/status`. `/firmware/*` stays CLOSED to the browser (the relay is server-to-server).
- **Boot-rescue re-push (#343):** a member reply carrying `rescue:1` makes it a follower-image candidate even at the stored rev (`ClusterMemberRuntime.rescue`). A rescue-triggered push burns its attempt AT START (`clusterRolloutStartRescue`) and its convergence does NOT clear the counter — a poisoned image can join looking healthy and crash after the handshake, so only the cap accumulating across whole rescue cycles stops an endless re-push; a rejoin still beaconing burns again (`RescueLooping`). Forgiveness: a NEW stored image (`clusterRolloutForgiveFollowerTargets`), 10 min continuous joined+non-rescue health (`clusterRolloutHealthyForgiveDue` — window resets at every re-join, so a crash loop never accumulates it), a genuine member rev change, config swap, or leader reboot. `rescue:true` in status members[] + a red member pill.
- **Bench harnesses:** `firmware/v2/Master/tests/fake_follower.py` (pytest-pinned wire twin; variants: `--plat esp01`, `--role`, `--rescue`, `--rollback`) ↔ `firmware/v2/FollowerEsp01/tests/fake_leader.py` (drives the follower wire; pytest runs it against the fake_follower esp01 variant so the twins can't drift).

## v2 — FollowerEsp01 (ESP-01 dumb row, #298)

Minimal ESP8266 firmware (spec `2026-07-14-v2-esp01-follower-design.md`) turning v1 master hardware (ESP-01 + Nano row) into a cheap wall row under one S3 leader. Plain `.cpp` on the unified ESP32Async stack (#356: `esp32async/ESPAsyncWebServer` at the SAME exact version as Master/Rescue + `esp32async/ESPAsyncTCP` as its ESP8266 TCP side, `eagle.flash.1m.ld`); single-core superloop — **async handlers stage, `loop()` mutates and is the only I2C toucher** (`SERIAL_ENABLE` in `FollowerConfig.h` trades I2C for serial, shared pins `Wire.begin(1, 3)`).

- Endpoint surface is EXHAUSTIVE (spec table): `/cluster/{join,render,ping,leave,health}` (join/ping replies carry `plat:"esp01"` + `heap`/`rssi`/`up`), `POST /firmware/master?md5=` (ESP8266 Update flow; ota-flash.sh's rev compare is the revert detector), `/reflash-units` (bundled hex, twiboot machinery, progress in `/units/health`), tiny `GET /settings`, `/units/health(+refresh)`, the `{"seq":N}` op subset, `POST /reboot`. No HTML, no MQTT; `/cluster/{digest,promote,config,discover}` are NOT served by design (surfaced in `GET /api`'s `notServed` array, drift-gated by `tests/test_api_index.py`) — never a takeover candidate. Refused foreign-leader contacts are counted in `/cluster/health`'s `foreign` block (#358, `ClusterForeign.h` on both follower sides).
- Phases (pure `FollowerPolicy.h`, trimmed ClusterFollowerPolicy copy): Standalone → Clustered → Grace (holds last text) → **Blank** (~2 min silence); membership persists in EEPROM (`FollowerSettings.h`, magic `4FFS`) so a reboot lands in Grace. SNTP epoch-only for commitAt flips; unsynced = render immediately.
- **Clock fallback (#342):** the leader's POSIX tz rides the join (`tz=` additive) and persists with the membership; a Blank row with a held membership + synced time renders centered HH:MM (pure `FollowerClock.h`) instead of going dark. Leader content replaces it; leave drops the zone; no tz or no sync = blank as before.
- **Boot-rescue beacon (#343):** RTC-memory bad-boot counter (pure `FollowerRescue.h`, word offset 32; zeroed by 60 s uptime or the deliberate-reboot path — which OTA completion takes; a power cycle wipes it). 3 consecutive early deaths boot a minimal beacon: WiFi + cluster wire + OTA only, no I2C/probe/boot-home/render, unit ops 409, `rescue:1` in join/ping replies → the leader re-pushes the stored image.
- Copied pure headers (fix bugs in both trees): `UnitHealth.h`, `UnitProtocolHelpers.h`, `WearPolicy.h`, `TwibootProtocol.h`, `DisplayWidth.h`, `FollowerCors.h`, `ClusterHmac.h`, `ClusterForeign.h`, `UnitVitals.h`, `HeartbeatPolicy.h`, `BootHomePlan.h`, plus `FollowerOps.h` (reflash batch size 2 — bench-tuned, NOT the S3's 4).
- `build_assets.py` bakes `data/unit-firmware.hex` (staged by `make_manifest.py stage`, CI-gated) into `UnitAssets.h` and stamps `follower-<rev>.bin` — the prefix ota-flash.sh keys the platform on.
- Tests: `pio test -e native` (policy/settings/json/ops/clock/rescue) + the fake_leader/fake_follower twin pytest (see Cluster bullet).

## v2 — Rescue + factory slot + bootloader

- **Factory slot (#193):** 2 MB `factory` app partition; bootloader factory reset (GPIO 4 low 5 s through reset) erases **otadata only** — never nvs (WiFi credentials must survive).
- **Rescue app (#195):** standalone project sharing nothing compiled with Master except the partition CSV — `Rescue*.h` pure headers are trimmed, natively tested copies. Boot: NVS read-only → 30 s STA join else `<name>-rescue` AP (captive) → slot inventory + upload-to-app0 (same `?md5=` contract) + `/rescue/exit`. Enter via GPIO 4 or `POST /firmware/rescue-boot` (409 while an install is in flight or the factory image is invalid). Master installs it via raw `esp_partition` writes (`FactorySlot.cpp` — flash sector 0 held back until the MD5 verdict, pure `FactoryChunkPlan.h`).
- **Custom bootloader (#201):** pioarduino ships the prebuilt stock bootloader and silently ignores bootloader-side `custom_sdkconfig` options — `firmware/v2/Bootloader/` (framework=espidf) builds ours; config + GPIO 4 constraints in its `sdkconfig.defaults`; artifact committed under `dist/` and swapped in at 0x0 by Master's `use_custom_bootloader.py` (build fails loudly if missing). Deployed boards get it once via `esptool write_flash 0x0`. Guarded by `tests/test_custom_bootloader.py`. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` stays in Master's AND Rescue's `custom_sdkconfig` — app-side load-bearing (esp_ota arms images as NEW). Deliberately excluded (eFuse burners): secure boot, flash encryption, anti-rollback.
