# CLAUDE.md

Current state only — history lives in git and the issue tracker; per-mechanism detail lives in the header comment of the file that owns it. Issue numbers (#N) are pointers into that history, not narrative. Per-project mechanism maps live in nested `CLAUDE.md` files (`firmware/v2/{Master,Unit,FollowerEsp01,Rescue}/CLAUDE.md`), loaded when you work in that tree.

## Project

Arduino-based split-flap display: a master MCU drives per-flap units over I2C. Firmware builds with PlatformIO — every project directory has its own `platformio.ini`; run commands from that directory. CI (`.github/workflows/build.yml`) builds every active firmware project (v1 excluded) and runs every native/pytest suite plus the unit-bundle and header-sharing gates (`tests/test_copied_headers.py` — no tree may shadow a `shared/` header, and the deliberate trims must keep their copy notes).

- **v1 is frozen** — only `firmware/v1/ESPMaster` remains, as reference. Never edit anything under `firmware/v1`.
- **v2 is the live stack:** `Master` (ESP32-S3 N16R8 devkit), `Unit` (Arduino Nano per flap), `FollowerEsp01` (ESP-01 row board under an S3 master, driven over the wall link). Pure-logic headers live once in `firmware/v2/shared/` and are included by every project (target and native envs, `-I ../shared`) — fix a bug there and all trees get it. Only deliberate trims (a reduced derivative, not a copy) stay per-tree, each with a note pointing at its source.

## Repository map

- `firmware/v2/Master/` — S3 master (plain `.cpp`, console on native USB-CDC)
- `firmware/v2/Unit/` — Nano unit: stepper + hall homing, I2C slave, EEPROM offset/address
- `firmware/v2/shared/` — pure-logic headers shared across v2 trees; `SplitFlapProtocol.h` is the master↔unit I2C contract + `SFP_PROTOCOL_VERSION`
- `firmware/v2/link/` — the master↔row-board link (#559): `wall_link.proto` + `.options` (every message, one envelope per direction; nanopb generates the C at build time via each tree's `custom_nanopb_protos`, `buildtools/ensure_nanopb_deps.py` installs the generator's Python packages where the toolchain has no pip) and `WallLinkStream.h` (collects length-delimited messages from a polled socket; unit facts as the `/units/health` JSON in pieces). Deliberately outside `shared/` (no unit compiles it, so an edit must not move the Unit source head). Tests: `pio test -e native -f test_wall_link` in Master; `cd firmware/v2/link && python -m pytest tests/` (needs `grpcio-tools`) has stock protobuf decode the bytes the native test pins; `fake_master.py` / `fake_row.py` are the bench stand-ins for either end, run against each other by `tests/test_fake_pair.py`
- `firmware/v2/buildtools/` — python shared by every tree's build: `fwbuild.py` (Intel-HEX → image, rev stamping, PROGMEM arrays; imported by each `build_assets.py`, `make_manifest.py` and the twiboot builder) and the one `patch_asyncweb.py`. Deliberately outside `shared/` (a build-script edit must not move the Unit source head). Tests: `cd firmware/v2/buildtools && python -m pytest tests/`
- `firmware/v2/UnitBootloader/` — vendored+patched twiboot (I2C reflash of units; see its README)
- `firmware/v2/FollowerEsp01/` — ESP-01 row board: dials its master over the wall link; serves only firmware upload, identity (`GET /settings`) and `POST /pair`
- `firmware/v2/Rescue/` — break-glass image for the factory slot
- `firmware/v2/Bootloader/` — builds the S3 second-stage bootloader (see its platformio.ini)
- `firmware/v1/ESPMaster/` — frozen ESP8266 master reference
- `flashing/` — `ota-flash.sh` (scp-fetch staged bin + OTA + `/settings` verdict; platform autodetect via the `plat` settings key, `-p esp01|esp32` to assert it; multi-device fan-out) and `flasher/make_manifest.py` (`stage` writes the unit bundle into `firmware/v2/Master/data` and `firmware/v2/FollowerEsp01/data` — must run between the Unit build and those builds; `gate` = the CI anti-drift check). Unit campaigns: `restore-unit-offsets.sh` (capture/replay per-unit calibration offsets that a unit EEPROM layout erase destroys) and `commission-units.sh` (gated per-unit reflash + acceptance run — needs `reflashOnBoot=false` and `/reflash-units?address=N`; stops at the first failure). Both talk to a board that serves the unit routes: the S3 master, not an ESP-01 row. New-board provisioning: esptool merged-factory-bin recipe in `flashing/README.md`.
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
- v2 first build on a clean machine is slow (pioarduino hybrid compile downloads IDF). The pioarduino platform needs PlatformIO Core ≥ 6.2.0 on the host (CI pins it in `.github/workflows/build.yml`). `managed_components/`, `sdkconfig.*`, `.dummy/` in v2 project dirs are generated artifacts (gitignored; exception: `Bootloader/sdkconfig.defaults` is a source file kept by a negation).

## Per-change workflow (overrides the global "stop the app" flow — there is no local app)

The firmware runs on bench hardware reached by OTA over the user's VPN; there is nothing to stop before editing. Per change:

1. **Issue first** with `effort:`/`gain:` labels; no private data (public repo).
2. **Commit direct to `master`.** No branch, no PR, no waiting on CI: CI runs on the push as a safety net, and a red run is fixed forward. A branch + PR is the exception — work that must not land yet (unfinished across sessions, not bench-proven), or a background session that cannot push `master`. A PR carrying a stage commit is never squash-merged (bundle drift is history).
3. **Build + tests for the trees you touched** before commit: `pio run`, `pio test -e native`, `python -m pytest tests/` in each touched project dir. An edit under `firmware/v2/shared` counts as touching Master, FollowerEsp01 and Unit.
4. **Unit bundle:** any commit touching `firmware/v2/Unit` or `firmware/v2/shared` moves the unit source head, and the drift gate then fails until the bundle is restaged — even when the Nano binary is unchanged. Rebuild Unit clean → `make_manifest.py stage` → rebuild Master + FollowerEsp01 → separate artifact commit (never amend the bundle in). If the image content hash is unchanged, record the deployed rev in `firmware/v2/Unit/equivalent-revs.txt` first, so fielded units do not read OUTDATED.
5. **Review only where it pays** — cpp-reviewer for OTA / boot / flash / concurrency / credentials / cluster-wire / I2C bus-and-probe changes. Nothing else needs a reviewer, and there is no end-of-arc combined gate or cross-model review unless asked for.
6. **Bench-verify = the E2E tier** — stage the bin to `~/bench-bins/`, `ota-flash.sh` to the board, confirm on hardware. Host tests cover pure logic only. OTA the leader first (an older-build leader downgrades followers via the rollout).
7. **Commit + close issue** — conventional message with `Closes #N` in the commit body (a `fix(#N):` scope closes nothing). Push.
8. **Update memory.**

## Release policy (CalVer `vYYYY.MM.DD`, ≤1/day)

The fleet converges on the git REV (`git describe`/short SHA baked into the binary), never on the tag — a tag is a human changelog + rollback anchor only. There is no version file: a release = an annotated git tag + a GitHub release with notes.

- At most one release per day — the date is the version. Fix-only days accumulate untagged on `master`; cut a dated release only when the day's diff is worth announcing or anchoring. Never tag inside a feature/fix commit.
- CalVer has no MAJOR signal, so flag breaking changes in the release: title `vYYYY.MM.DD — BREAKING` plus a `## Breaking / Operator action required` section, for any change to the I2C/cluster wire, NVS/EEPROM layout, partition table, OTA contract, config semantics, or minimum bootloader.
- Same-day critical fix after a release: OTA-deploy the fixed REV now and cut the dated release the next day. `vYYYY.MM.DD-hotfix.N` is break-glass only.
- Frozen v1 tags (`v1.x`) stay semver; mixed tags don't version-sort, so always pass `gh release create --latest`. Never parse meaning from the tag, and never retag/rewrite a shipped release.

## Hard rules

- **GPIO 35/36/37 are used by octal PSRAM on the S3 — never assign them.** GPIO 4 is reserved (factory-reset button), 19/20 are native USB, 48 is the devkit WS2812.
- **Never `pio run -t upload` the Rescue project.** Install it via Master's `POST /firmware/rescue` or `esptool write-flash 0x830000`.
- **Don't port v1's OTA verdict machinery to v2** (RTC cookie / sketchMd5 compare) — the S3's A/B boot makes it obsolete; use `esp_ota_*` state APIs and the core's weak `verifyRollbackLater()`/`verifyOta()` hooks.
- **v2 web/MQTT code never touches display state directly** — enqueue a `DisplayCommand` (params baked in by the sender), read back mutex-copied `DisplaySnapshot`s.
- **The partition table (S3) and EEPROM layout (Nano unit) are per-device truth** — change them only via their documented migration/invariant rules (Master and Unit CLAUDE.md).
- **The v2 bootloader is immutable over OTA** and has no A/B slot — never write it from the running app; changes are per-board USB flashes.
- **`UnitBus.cpp` is the only Wire toucher on v2 (through its bus adapter — the unit protocol itself is `shared/UnitBusCore.h`), and displayTask its only caller** (SDA 8 / SCL 9, 100 kHz — 400 kHz was tried and reverted for twiboot reflash + wall signal integrity; Nano TWI slaves follow the master clock).
- **Twiboot probe-inhibit: displayTask owns a deadline armed by `/unit/reboot` and address burns — every runtime probe waits it out; never bypass it.** Sole exception: the reflash job's internal probes (pinned units are immediately flashed + exited).
- **Producer gate: while `reflashInProgress(snapshot.reflash)`, every display-mutating producer stands down (web/MQTT 409, clockTask skips, master OTA 409) except `/stop`.**
- **The `storage` partition is one shared LittleFS and netTask is its sole flash writer** (producers stage under a mutex) — future storage tenants join it; never carve new partitions.
