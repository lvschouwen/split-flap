# CLAUDE.md

Current state only — history lives in git and the issue tracker; per-mechanism detail lives in the header comment of the file that owns it. Issue numbers (#N) are pointers, not narrative. Mechanism maps: nested `firmware/v2/{Master,Unit,FollowerEsp01,Rescue}/CLAUDE.md`; per-area notes: `.claude/rules/*.md` (path-scoped, load with the files they cover).

## Project

Arduino-based split-flap display: a master MCU drives per-flap units over I2C. PlatformIO; every project directory has its own `platformio.ini`, run commands from there.

- **v1 is frozen** — only `firmware/v1/ESPMaster` remains, as reference. Never edit anything under `firmware/v1`.
- **v2 is the live stack:** `Master` (ESP32-S3 N16R8 devkit), `Unit` (Arduino Nano per flap), `FollowerEsp01` (ESP-01 row board, driven over the wall link). Pure-logic headers live once in `firmware/v2/shared/`, included by every tree (`-I ../shared`); `tests/test_copied_headers.py` forbids a tree shadowing one. Only deliberate trims stay per-tree, each with a note pointing at its source.

## Repository map

- `firmware/v2/Master/` — S3 master; operator page (`web/`) and operator API (`/api/v2`, listed by `GET /api`)
- `firmware/v2/Unit/` — Nano unit: stepper + hall homing, I2C slave, EEPROM offset/address
- `firmware/v2/FollowerEsp01/` — ESP-01 row board: dials its master over the wall link; serves only firmware upload, `GET /settings` and `POST /pair`
- `firmware/v2/Rescue/` — break-glass image for the factory slot · `Bootloader/` — the S3 second-stage bootloader
- `firmware/v2/shared/` — pure logic for every tree; `SplitFlapProtocol.h` is the master↔unit I2C contract + `SFP_PROTOCOL_VERSION`
- `firmware/v2/link/` (wall link proto), `buildtools/` (build python), `release/` (release fetch + trust) — outside `shared/` so an edit does not move the Unit source head
- `firmware/v2/UnitBootloader/` — vendored + patched twiboot (I2C reflash of units; see its README)
- `flashing/` — `ota-flash.sh`, `flasher/make_manifest.py` (unit bundle `stage` / `gate`), unit campaign scripts, `release.py`
- `PCB/v1/` — the unit board (no new board planned) · `docs/superpowers/specs/` — design docs · `docs/CODING_STANDARDS.md` — how code is written (Hard rules win)

## Build / check

- `pio run` · `pio test -e native` · `pio device monitor` (115200) in a project dir; Unit envs `unit` / `unit_old_bootloader`.
- **The one check is `scripts/check.sh`** (`CHECK=` in `.claude/agent-wt.conf`, run by `agent-check`): every v2 build, native and pytest suite (own uv venv on pinned versions) and the drift gates. `check.sh <tree>...` or `--touched [base]` narrows it. The pre-push hook (`scripts/pre-push`, linked by `scripts/install-hooks.sh`) skips a tree `agent-check` stamped green and otherwise runs `--touched` over the pushed range.
- Every platform, library and pip package is pinned exactly (#648; `.claude/rules/buildtools.md`).

## Per-change workflow (no local app to stop)

The firmware runs on bench hardware reached by OTA over the user's VPN.

1. **Issue first** with `effort:`/`gain:` labels; no private data (public repo).
2. **Commit direct to `master`**; the pre-push hook is the gate. A branch + PR only for work that must not land yet or a session that cannot push `master`; a PR carrying a stage commit is never squash-merged.
3. **`scripts/check.sh --touched`** before commit (shared/ and buildtools/ reach every firmware tree, release/ Master + Rescue, link/ Master + FollowerEsp01).
4. **Unit bundle:** a commit touching `firmware/v2/Unit` or `firmware/v2/shared` moves the unit source head; the drift gate fails until restaged, even when the Nano binary is unchanged. Rebuild Unit clean → `make_manifest.py stage` → rebuild Master + FollowerEsp01 → separate artifact commit (never amended in). If the image content hash is unchanged, record the deployed rev in `firmware/v2/Unit/equivalent-revs.txt` first, so fielded units do not read OUTDATED.
5. **Review only where it pays** — a reviewer for OTA / boot / flash / concurrency / credentials / wall-link / I2C bus-and-probe changes; nothing else.
6. **Bench-verify = the E2E tier** — stage the bin to `~/bench-bins/`, `ota-flash.sh` to the board, confirm on hardware. OTA the leader first (an older leader downgrades followers via the rollout).
7. **Commit + close** — `Closes #N` in the body (a `fix(#N):` scope closes nothing). Push. Update memory.

Releases: CalVer `vYYYY.MM.DD`, at most one a day; read `.claude/rules/release-policy.md` before tagging (BREAKING flag, hotfix, `--latest`).

## Hard rules

- **The partition table (S3) and EEPROM layout (Nano unit) are per-device truth** — change them only via their migration/invariant rules (Master and Unit CLAUDE.md).
- **The Home Assistant MQTT interface is an external contract** (`docs/HA_CONTRACT.md`): topic names, discovery unique ids, JSON attribute keys and enum values are append-only; a rename or removal needs a hass-config issue first.
- **S3 rules** (PSRAM GPIOs, Rescue never USB-uploaded, no v1 OTA verdict, DisplayCommand only, immutable bootloader, `UnitBus.cpp` sole Wire toucher, twiboot probe-inhibit, reflash producer gate, netTask sole `storage` writer): `.claude/rules/s3-hard-rules.md`.

## graphify

`graphify-out/` (local, gitignored, AST-only, refreshed by git hooks) is the cheapest way into the code: **query it before grepping or reading whole files** for "where does X live / how does A reach B / what calls this", especially across trees. `graphify query "<q>" --budget 1500` (a narrow question beats a raised budget), `graphify explain "Symbol"`, `graphify path "A" "B"`; then read only the lines it points at. Main only: agents cannot run it, so a brief carries what it found. Never run the semantic extraction unasked.

## Agent runs (the standard: `~/.claude/rules/common/agent-runs.md`)

- Check: `agent-check` (= `scripts/check.sh`); agents iterate with `scripts/check.sh --touched master` in their slot. Slots via `agent-wt` (`wt-N`, warm `.pio`).
- Opus for concurrency, I2C bus/probe, OTA/boot/flash, wall link, EEPROM/NVS/partition, credentials; Sonnet otherwise.
- Guards: `splitflap-implementer` / `splitflap-reviewer` carry the global deny hook plus `guard-bench.sh --agent` (ota-flash, campaign scripts, release.py, wall-API writes).
- Conflict files: `firmware/v2/shared/` and `firmware/v2/Unit/` (bundle restage, one agent at a time), `tests/test_copied_headers.py` pairs, route and test registries, the standard baseline.
