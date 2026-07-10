# v2: rescue app for the factory slot (#195) — design

Date: 2026-07-10
Status: approved (brainstorm with user)
Parent: `2026-07-10-v2-rescue-update-design.md` (slice 3, un-parked)

## Why now

#195 was parked pending evidence of a double-brick. The un-parking trigger is
different: the board will be soldered into a wall-mounted display soon, after
which USB access becomes expensive. The escape hatch must be real — installed,
bench-proven, and refreshable over WiFi — before that happens.

## Scope decisions (user-approved)

- **Upload-a-bin only.** No "fetch latest release" in the rescue image — that
  depends on #194 (`ReleaseCheck.h` + a release workflow attaching v2 bins),
  neither of which exists. Fetch-latest can be added to the rescue image after
  #194 ships.
- **Approach A: standalone project.** `firmware/v2/Rescue/` with its own
  `platformio.ini` (same S3 board config, same partition CSV, same
  `esp32async/ESPAsyncWebServer` stack), plain `.cpp`. Helpers it needs are
  **copied** in (repo convention), never `#include`d from `Master/` — once
  bench-proven, Master refactors must have zero ability to change the rescue
  image. Rejected: `#ifdef RESCUE_BUILD` inside Master (Master churn flows
  into the break-glass image), bare ESP-IDF image (second framework for size
  savings the 2 MB slot doesn't need).
- **Both normal-firmware companions.** `POST /firmware/rescue` (populate and
  refresh the factory slot over WiFi) and `POST /firmware/rescue-boot`
  (software entry; doubles as the periodic "prove it still boots" test).

## Rescue image (`firmware/v2/Rescue/`)

Boot sequence:

1. Read `deviceName`, `wifiSsid`, `wifiPass` from NVS — **read-only**; rescue
   never writes settings.
2. Try STA join for 30 s; on failure open AP `<name>-rescue` (open AP,
   matching the `<name>-setup` convention — rescue is only reachable after
   deliberate entry via button or authenticated-by-LAN endpoint).
3. mDNS `<name>.local` on STA join.

Endpoints (single self-contained page, no PROGMEM asset pipeline needed):

- `GET /` — rescue build rev, per-slot app descriptions
  (`esp_app_get_description` for ota0/ota1/factory) so what's installed is
  visible, upload form, "boot normal firmware" button.
- `POST /firmware/master` — **same wire contract as normal firmware**
  (multipart + mandatory `?md5=`): stream to `ota0` via `esp_ota_begin/write/
  end`, verify MD5, `esp_ota_set_boot_partition(ota0)`, reboot. The image
  boots `PENDING_VERIFY` and rides the existing #190 health gate — a bad
  upload rolls back (worst case to rescue again). No new rollback machinery.
- `POST /rescue/exit` — set boot partition to a valid OTA slot and reboot
  (escape from accidental entry, no upload required).

Not in the image: display/I2C, MQTT, clock, settings UI, setup portal,
OTA-of-itself. If it isn't needed to get a working image onto the board, it
doesn't exist.

## Normal-firmware companions (Master)

- `POST /firmware/rescue` — same multipart + `?md5=` contract, target = the
  `factory` partition. Factory is not an OTA slot, so the `Update` path won't
  target it: raw `esp_partition_erase_range` + `esp_partition_write`, then MD5
  read-back verify. Does not touch otadata — installing a rescue image never
  changes what boots next.
- `POST /firmware/rescue-boot` — erase both otadata sectors + reboot; the
  bootloader then boots the factory slot. Guard: **409 if the factory slot has
  no valid app image** (image-header check) so a wall-mounted device can't be
  told to boot an empty slot. (With factory empty the bootloader would fall
  back to an OTA slot anyway, but the guard keeps the endpoint honest as the
  "prove the rescue image still boots" test.)

## Pure logic + tests (TDD, native env mirroring Master's)

- Rescue WiFi decision: trimmed copy of `WifiPolicy.h` (join-or-AP only — no
  portal, no reboot-retry).
- Slot logic as pure functions: exit-slot selection (newest valid OTA slot),
  factory-slot validity from image-header bytes (shared by rescue's `GET /`
  and Master's 409 guard — copied, per convention).
- Upload gating (md5 param present/valid, length bounds) — same shape as
  Master's OTA tests.
- `firmware/v2/Master/tests/test_partition_table.py` already pins the factory
  slot layout; CI matrix gains `v2/Rescue`.

## Bench verification (the E2E tier)

First pass flashes rescue to factory over USB, then the full field loop must
work without USB:

1. `POST /firmware/rescue` from a browser/scripted client refreshes the
   factory slot over WiFi.
2. `POST /firmware/rescue-boot` → device comes back as rescue; page shows
   slot inventory. Verify both STA-join and `-rescue` AP paths.
3. Upload a known-good Master bin → health gate confirms → normal firmware
   back, WiFi credentials intact (NVS untouched, per #193's otadata-only
   erase).
4. GPIO-4 hold through reset → rescue boots (hardware entry path).
5. `POST /rescue/exit` from accidental entry → normal firmware boots, no
   flash writes beyond otadata.

## Security notes

- The `-rescue` AP is open and can accept firmware — same trust model as the
  existing `<name>-setup` portal and every unauthenticated endpoint in the
  project (LAN-trust). It only exists while rescue is deliberately booted.
- Upload integrity: MD5 over the wire contract plus `esp_ota_end`'s image
  validation; a wrong/corrupt image fails the health gate and rolls back.
- `rescue-boot` and `rescue` endpoints add no new secrets and write nothing
  to NVS.
