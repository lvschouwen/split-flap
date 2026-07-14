# ESPMaster (v1) — FROZEN

**This tree is frozen (#217 / #283, 2026-07-14). No further maintenance.**
`firmware/v2/Master` (ESP32-S3) is the master firmware stack.

- Kept in the repo as reference for legacy ESP8266/ESP-01 hardware only; it
  is out of CI and out of the unit-bundle staging flow
  (`make_manifest.py stage` writes the v2 tree only).
- `data/unit-firmware.hex` / `.rev` are a fossil of the last pre-freeze
  stage — they are no longer refreshed when the Unit firmware changes.
- If legacy hardware ever needs a build: `pio run` in this directory still
  works at this revision; stage a current unit bundle manually first if the
  Unit firmware moved (copy `firmware/v1/Unit/.pio/build/unit/firmware.hex`
  and `.rev` into `data/`).
- OTA to a running v1 master: `flashing/ota-master.sh <fw.bin> http://host`
  (stays until both live displays are migrated to S3 masters).

The sibling `firmware/v1/Unit` (Arduino Nano) firmware is **not** frozen —
the Nano units stay active under the v2 master.
