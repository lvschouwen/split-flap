# v2: factory rescue slot + check-for-update — design

Date: 2026-07-10
Status: approved (brainstorm with user)

## Problem

The v2 master's A/B rollback (#190) auto-recovers from an OTA image that fails
its health gate. It does not cover: an image that passes the gate but is broken
in ways the gate doesn't see (and is then joined by a second bad OTA — both
slots bad), NVS-induced bricks, or recovering a wall-mounted display without a
USB cable. Separately, every routine update today needs a PC running
`flashing/ota-master.sh`.

Two constraints are write-once: the partition table and the second-stage
bootloader are only ever written over USB. Anything that touches them must be
decided before a board is soldered into a display.

## Decision summary (user-approved)

| Slice | What | Priority |
|-------|------|----------|
| 1 | Reserve `factory` partition + bootloader factory-reset config | Do soon — only time-sensitive part |
| 2 | "Check for update" (GitHub release pull) in normal firmware | On the board — after devkit bench + I2C slice |
| 3 | Rescue app image for the factory slot | Parked indefinitely — revisit only if double-brick occurs in practice |

Rationale for parking slice 3: the both-slots-bad failure class is rare by
construction (health gate), USB access always exists for this device, and a
rescue image that is never booted rots silently. Slice 1 keeps the door open at
zero cost.

## Slice 1 — reserve the escape hatch

`partitions_splitflap_16MB.csv`: add a `factory` app slot carved from
`storage`, placed after `coredump` so nvs/otadata/app0/app1 offsets are
unchanged:

```
factory,  app,  factory,  ,  0x200000   # 2 MB rescue slot (empty until slice 3)
storage,  data, spiffs,   ,  0x5D0000   # was 0x7D0000
```

An empty factory slot is safe: with valid otadata the bootloader boots the
selected OTA slot as today; if directed at an empty factory it falls back to a
bootable OTA slot.

Bootloader sdkconfig (`custom_sdkconfig`):

- `CONFIG_BOOTLOADER_FACTORY_RESET=y`
- otadata-erase only (`CONFIG_BOOTLOADER_OTA_DATA_ERASE=y`); do NOT list `nvs`
  in the data-erase set — rescue must keep WiFi credentials.
- Reset pin: chosen during implementation on the bench. Constraints: not
  GPIO 0 (holding it through reset enters ROM download mode first), not
  35/36/37 (octal PSRAM), must be a pin we can commit to exposing as a button
  on the future PCB. Document the chosen pin in the CSV header comment and in
  `PCB/v2/` notes.

Verification: build, flash devkit, confirm `partitions.bin` placement matches
the header comment, confirm normal boot + OTA cycle unaffected, confirm
holding the chosen pin erases otadata (device then falls back to an OTA slot
since factory is empty — expected until slice 3).

## Slice 2 — check-for-update in normal firmware

UX (user-decided): **manual only**. No background polling, no auto-install —
a wall display must never reboot itself.

Release side:

- The release workflow attaches `firmware-v2-master.bin` to each GitHub
  release, built with a `RELEASE_TAG` define alongside the existing `GIT_REV`
  (`build_assets.py` / CI). Dev builds have no tag.

Firmware side:

- Maintenance-tab card "Firmware update": *Check* fetches
  `https://api.github.com/repos/lvschouwen/split-flap/releases/latest`
  (HTTPS via `esp_crt_bundle` — Mozilla roots baked in, no cert babysitting;
  repo hardcoded; unauthenticated API is fine at 60 req/h/IP for a manual
  button). UI shows running tag/rev vs latest tag. *Install* streams the
  matching asset with `esp_https_ota` into the inactive slot and reboots into
  the existing `PENDING_VERIFY` → health-gate → confirm path from #190. No new
  rollback machinery.
- Async-context rule holds: the handler only stages a request; fetch and
  install run from a task. JSON is read bounded/streamed via `largeAlloc`.
- Pure logic in a natively-tested header (`ReleaseCheck.h`): release-JSON
  field extraction (tag, asset URL for the expected asset name) and
  running-vs-latest comparison (dev builds → "dev build, latest is X").
  TDD as usual.
- `/settings` gains the running-tag field; `intendedVersion` NVS behavior from
  #190 is unchanged (the install passes `?v=<tag>` semantics internally).

## Slice 3 — rescue app (parked)

Sketch only, for when/if evidence justifies it: a minimal separate image
(own PlatformIO env) in the factory slot — join WiFi from NVS, fallback AP
`<name>-rescue`, serve upload-a-bin plus "fetch latest release" (reusing
slice 2's `ReleaseCheck.h` + `esp_https_ota`), write to app0, boot it. Entry
paths: the slice-1 bootloader pin, plus `POST /firmware/rescue-boot` in normal
firmware (erase otadata + reboot) — which doubles as the periodic "prove the
rescue image still boots" test. Gets its own brainstorm before any code.

## Security notes

- All downloads TLS-verified against the built-in root bundle; source repo
  hardcoded — no user-configurable URL, no injection surface.
- Image integrity: `esp_https_ota` validates the app image header; a corrupt
  or wrong image fails the health gate and rolls back. No new persistent
  secrets.
