# Split-Flap — flashing

The master stack is **v2** (ESP32-S3, `firmware/v2/Master`). Provisioning a
new display is a two-step: flash the S3 once over USB with a merged factory
image, then let the running master flash the Nano units itself over I2C.

## Provision a new S3 master (merged factory image)

Build both images, merge them, flash once. The merged image contains the
custom bootloader @ 0x0 (#201), partition table, boot_app0, the app in
app0, and the Rescue image in the factory-adjacent `rescue` slot @ 0x830000
— everything a blank board needs.

    # 1. build (unit bundle must be staged first if Unit/ changed — see below)
    (cd firmware/v2/Master && pio run -e master)
    (cd firmware/v2/Rescue && pio run -e rescue)

    # 2. merge (pio's firmware.factory.bin lacks the rescue slot)
    python3 -m esptool --chip esp32s3 merge_bin -o splitflap-v2.factory.bin \
        --flash_size 16MB \
        0x0 firmware/v2/Master/.pio/build/master/firmware.factory.bin \
        0x830000 firmware/v2/Rescue/.pio/build/rescue/firmware.bin

    # 3. flash (any OS with esptool; use the board's native USB port)
    python3 -m esptool --chip esp32s3 --port <PORT> erase_flash
    python3 -m esptool --chip esp32s3 --port <PORT> write_flash 0x0 splitflap-v2.factory.bin

First boot on blank NVS logs one expected `slotRec1 NOT_FOUND` error (#200
record not stamped yet). Join WiFi via the `<name>-setup` portal, then all
further master updates ride web OTA (`ota-flash.sh` below or the
Maintenance tab).

**Units:** Nanos carrying only twiboot get the bundled unit firmware pushed
automatically when the master probes them; **Flash all unit(s)** /
`/reflash-units` re-pushes on demand. Only the one-time twiboot ICSP flash
per Nano happens off-master (see `firmware/v2/UnitBootloader/`).

## Only one ICSP flash per Nano — twiboot only

The Unit sketch is bundled inside the master firmware (PROGMEM) and pushed
to each Nano over I2C automatically the first time the master sees it in
bootloader mode. Unit firmware updates ride along with master OTA updates.

## DIP switch addresses

`1` = switch up. The unit firmware offsets the DIP value by 1 for I2C
(address 0x00 is reserved), so DIP 0000 → I2C 0x01. Units must be addressed
contiguously from unit 1.

| Unit | DIP  | I2C  | Unit | DIP  | I2C  |
| ---- | ---- | ---- | ---- | ---- | ---- |
| 1    | 0000 | 0x01 | 9    | 1000 | 0x09 |
| 2    | 0001 | 0x02 | 10   | 1001 | 0x0A |
| 3    | 0010 | 0x03 | 11   | 1010 | 0x0B |
| 4    | 0011 | 0x04 | 12   | 1011 | 0x0C |
| 5    | 0100 | 0x05 | 13   | 1100 | 0x0D |
| 6    | 0101 | 0x06 | 14   | 1101 | 0x0E |
| 7    | 0110 | 0x07 | 15   | 1110 | 0x0F |
| 8    | 0111 | 0x08 | 16   | 1111 | 0x10 |

## Unit-bundle staging (`make_manifest.py`)

`flasher/make_manifest.py` is the unit-bundle tool and it is load-bearing:

    cd flashing
    (cd ../firmware/v2/Unit && pio run)
    python3 flasher/make_manifest.py stage    # writes firmware/v2/Master/data
    (cd ../firmware/v2/Master && pio run)     # rebuild so the bake picks it up
    python3 flasher/make_manifest.py gate     # anti-drift check (CI runs this)

`stage` writes into the **committed** `firmware/v2/Master/data/unit-firmware.hex`
and `.rev` — after a Unit code change, commit the refreshed pair alongside
it (build clean: a `-dirty` rev fails the gate). CI's `gate` step fails the
build when any commit touches the Unit sources (`firmware/v2/Unit` minus
`test/`, plus `firmware/v2/shared`) after the staged bundle's rev, because a
drifted bundle means the master auto-pushes stale unit firmware. The gate
compares revs, not bytes — the Unit binary embeds its `GIT_REV`
(`build_version.py`), so a rebuilt hex never byte-matches the committed one.
One caveat: never squash-merge a PR that contains a stage commit — squashing
rewrites the hash the committed `.rev` points to and the gate can no longer
resolve it (fails safe, but permanently red until re-staged).

## OTA scripts

`ota-flash.sh` is the v2 (ESP32-S3) updater: it fetches the newest staged
`firmware-<rev>.bin` / `rescue-<rev>.bin` from a build server over ssh/scp
and uploads it to a running master, with the verdict read back from
`/settings` (`version` + `otaReverted` — native A/B rollback, no v1 RTC
machinery). `./ota-flash.sh -s user@buildhost <device-ip>` for the master
firmware, `-r` for the rescue image, `-a` for both, `-l file.bin` to skip
the download. Server/dir defaults come from `SPLITFLAP_BIN_HOST` /
`SPLITFLAP_BIN_DIR` (staging dir defaults to `bench-bins` in the remote
home). A network-level upload failure (connection reset, timeout) is
reported as such — distinct from a device rejection — and retried once
automatically (#248); the device aborts the stale half-session on the
next upload's start. On every server-backed run the script md5-compares
itself against `<staging-dir>/ota-flash.sh` and prints the update
one-liner when the local copy is stale (#262, warn-only) — so keep the
staged copy fresh alongside the bins.

`ota-master.sh` is the **legacy v1** (ESP8266) OTA uploader from a dev
checkout: `./ota-master.sh <fw.bin> http://host:port`. It stays until both
live displays are migrated to S3 masters (#285), then it goes too.

## Legacy: the Windows flasher exe (retired)

The guided provisioning exe (`split-flap-flasher.exe`) is retired (#284) —
it provisioned the **v1** ESP-01 master, which is frozen (#283). The
esptool recipe above is the provisioning path now.

- The v1.1.0 release exe remains downloadable for any legacy v1 hardware;
  no new exe builds (`.github/workflows/flasher.yml` is deleted).
- The exe-only modules under `flasher/` (`wizard.py`, `ota.py`, `esp.py`,
  `avr.py`, `assets.py`, `ui.py`, `ports.py`, `session.py`, `wiring.py`,
  `__main__.py`, `flasher.spec`, `assets-src/`) are frozen in place as
  reference; their pure-logic tests still run in CI. Only
  `make_manifest.py` (above) is maintained. The old `collect` subcommand
  (exe asset packaging) is gone.
