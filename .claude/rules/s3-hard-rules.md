---
paths:
  - "firmware/v2/Master/**"
  - "firmware/v2/Rescue/**"
  - "firmware/v2/Bootloader/**"
  - "firmware/v2/shared/**"
  - "firmware/v2/release/**"
---
# S3 hard rules (moved from root CLAUDE.md, #656)

- **GPIO 35/36/37 are used by octal PSRAM on the S3 — never assign them.** GPIO 4 is reserved (factory-reset button), 19/20 are native USB, 48 is the devkit WS2812.
- **Never `pio run -t upload` the Rescue project.** Install it via Master's `POST /firmware/rescue` or `esptool write-flash 0x830000`.
- **Don't port v1's OTA verdict machinery to v2** (RTC cookie / sketchMd5 compare) — the S3's A/B boot makes it obsolete; use `esp_ota_*` state APIs and the core's weak `verifyRollbackLater()`/`verifyOta()` hooks.
- **v2 web/MQTT code never touches display state directly** — enqueue a `DisplayCommand` (params baked in by the sender), read back mutex-copied `DisplaySnapshot`s.
- **The v2 bootloader is immutable over OTA** and has no A/B slot — never write it from the running app; changes are per-board USB flashes.
- **`UnitBus.cpp` is the only Wire toucher on v2 (through its bus adapter — the unit protocol itself is `shared/UnitBusCore.h`), and displayTask its only caller** — one exception: `unitBusCatchAtPowerOn`, which `setup()` runs before any task exists (Master CLAUDE.md, power-on catch) (SDA 8 / SCL 9, 100 kHz — 400 kHz was tried and reverted for twiboot reflash + wall signal integrity; Nano TWI slaves follow the master clock).
- **Twiboot probe-inhibit: displayTask owns a deadline armed by a unit restart and address burns — every runtime probe waits it out; never bypass it.** Sole exception: the reflash job's internal probes (pinned units are immediately flashed + exited).
- **Producer gate: while `reflashInProgress(snapshot.reflash)`, every display-mutating producer stands down (web/MQTT 409, clockTask skips, master OTA 409) except the `stop` action.**
- **The `storage` partition is one shared LittleFS and netTask is its sole flash writer** (producers stage under a mutex) — future storage tenants join it; never carve new partitions.
