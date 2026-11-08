---
paths:
  - "flashing/**"
---
# flashing/ — bench, campaign and release tools

- `ota-flash.sh`: scp-fetch a staged bin + OTA + `/settings` verdict; platform autodetect via the `plat` settings key (`-p esp01|esp32` asserts it); multi-device fan-out.
- `flasher/make_manifest.py`: `stage` writes the unit bundle into `firmware/v2/Master/data` and `firmware/v2/FollowerEsp01/data` (must run between the Unit build and those builds); `gate` = the anti-drift check in `scripts/check.sh`.
- Unit campaigns: `restore-unit-offsets.sh` (capture/replay the per-unit calibration offsets a unit EEPROM layout erase destroys) and `commission-units.sh` (gated per-unit update + acceptance run; needs `updateUnitsAtStart` false; zeroes the unit's odometer; stops at the first failure). `ext-diag-monitor.py` / `ext-diag-benchcheck.py` show the master's verdicts. All four talk to the master's `/api/v2` only, also for a row board's units (boards by id; `unit-offsets.json` names the master; the shell pair shares `wall-api.sh`). Tests: `python3 -m pytest tests/` runs the shell pair against a stand-in master.
- New-board provisioning: the esptool merged-factory-bin recipe in `flashing/README.md`.
- `release.py` publishes the staged images and a signed manifest on the `gh-pages` branch (served as the `RELEASE_HOST` of `ReleaseSource.h`), then tags and creates the GitHub release; `--channel test` publishes under `/test/` only. Every release also publishes the web flasher: `flashing/site/index.html` (at `/`, a trial release's at `/test/`) and ESP Web Tools under `/esp-web-tools/`, unpacked from the npm package pinned by version and hash in `release.py`. `make-release-key.sh` creates the signing key (it lives in `~/.config/split-flap`, never in the repository). Policy: `.claude/rules/release-policy.md`.
