# Design: Unique per-device network identity (#125)

**Date:** 2026-07-05
**Issue:** #125
**Status:** Approved

## Motivation

Two displays (5-unit, incoming 16-unit) will share one LAN running the same firmware
image. Today every network-facing name is a compile-time constant, so two devices on
the same image collide on mDNS, DHCP hostname, MQTT client id / HA discovery
`uniq_id`s, and (in rare windows) the recovery / quiet-OTA / captive-portal SSIDs.
This change derives a unique default identity from the ESP8266 chip id and layers an
EEPROM-backed, web-UI-editable pretty name on top — both phases ship together.

## Decisions (settled during brainstorm)

- **Phases 1+2 together** — one branch, one bench test.
- **The device name is the complete identity** (not a suffix on a fixed prefix).
  Typing `kitchen` yields `kitchen.local`, hostname `kitchen`, MQTT id `kitchen`,
  recovery SSID `kitchen-rec`.
- **Name limit is 24 chars**; AP suffixes are sized so every composed SSID fits the
  32-byte SSID limit: `-rec` (recovery, was `-recovery`), `-ota` (quiet OTA),
  `-setup` (captive portal). Worst case 30 bytes.
- **HA rename cleanup happens at save time** — while the firmware still *is* the old
  identity, it clears the old retained discovery configs, then prompts reboot.
- **`mqttDeviceId` compile-time const is deleted** — MQTT identity always follows the
  effective device name.
- **`WIFI_STATIC_IP` and its config block are deleted** (`ESPMaster.ino:75`,
  `:189–199`, `ServiceWifiFunctions.ino:10/44`) — DHCP always; IP pinning is the
  router's job (DHCP reservation by MAC). Two devices hardcoding the same static IP
  would be fatal on a shared LAN.

## Section 1 — Identity resolver and consumers

New pure helper header `DeviceIdentity.h` (natively testable, same pattern as
`MqttHelpers.h`) plus one global resolved at boot:

```cpp
String effectiveDeviceName;  // resolved once, read everywhere
```

Resolution order:

1. EEPROM `deviceName` — only if the settings **magic/version check passes** and the
   slot holds a non-empty, valid name.
2. Fallback: `"split-flap-" + String(ESP.getChipId(), HEX)` (e.g. `split-flap-9a3c1f`,
   ≤19 chars).

Boot order (revised in review follow-up): `initialiseFileSystem()` was hoisted to
the top of `setup()` — after the RTC boot-counter increment (so a crash in it still
counts toward the recovery threshold), before the quiet-OTA/recovery dispatch. That
gives one `EEPROM.begin` + one migration point for every boot path, and the resolver
runs immediately after it on an already-migrated blob. The magic/version guard stays
as protection against a corrupt or foreign blob: invalid EEPROM → chip-id default,
always safe, still unique.

Consumers (constants → resolved name):

| Call site | Today | Becomes |
|---|---|---|
| `ServiceWifiFunctions.ino:18` | `WiFi.hostname("Split-Flap")` | `effectiveDeviceName` |
| `ESPMaster.ino:898` | `MDNS.begin(mdnsName)` | `effectiveDeviceName` |
| Recovery AP `ESPMaster.ino:592/599` | `"split-flap-recovery"` | `effectiveDeviceName + "-rec"` |
| Quiet-OTA AP `ESPMaster.ino:645/652` | `"split-flap-ota"` | `effectiveDeviceName + "-ota"` (not in issue; same collision class) |
| Captive portal `ServiceWifiFunctions.ino:31` | `"Split-Flap-AP"` | `effectiveDeviceName + "-setup"` (inherited by #126) |
| MQTT `ServiceMqttFunctions.ino:116` | `mqttDeviceId` const → `mdnsName` fallback | `effectiveDeviceName`; `mqttDeviceId` const deleted |

`mdnsName` const remains only as the `"split-flap"` prefix seed for the chip-id
default. `/settings` gains `deviceName` (raw EEPROM value, possibly empty) and
`effectiveDeviceName`. The web UI header shows the effective name.

## Section 2 — EEPROM slot and migration

Carved from `RESERVED_2` in `SettingsEepromLayout.h`, existing string-slot convention
(NUL-terminated, zero-padded):

- `OFF_DEVICE_NAME 137`, `LEN_DEVICE_NAME 25` (24 chars + NUL).
- `RESERVED_2` becomes offset 162, length 1870.
- `SETTINGS_VERSION 4 → 5`; `ver < 5` migration rung zero-fills the slot. Existing
  devices upgrade in place; empty slot = chip-id default; no user-visible change.

## Section 3 — Web UI, API, validation

- Settings form gains a **Device name** field, pre-filled with the raw EEPROM value;
  the placeholder shows the chip-id default so "unset" is visible.
- Server-side validation in `DeviceIdentity.h`: lowercase `[a-z0-9-]`, no
  leading/trailing hyphen, 1–24 chars; input lowercased before validation; **empty is
  valid and means "reset to chip-id default"** (slot zero-filled). Invalid input →
  400 + message (same pattern as the timezone field).
- Save response signals reboot-needed; UI shows the existing "reboot to apply"
  prompt. No live re-init in v1.

## Section 4 — MQTT rename cleanup

In the settings save handler, when the name actually changes AND the build has
`MQTT_ENABLE` AND the client is connected: **before** writing EEPROM, publish empty
retained payloads to all HA discovery config topics for the current (old) identity —
loop over the entity list using the existing topic builders in `MqttHelpers.h`. Then
write EEPROM and prompt reboot. If MQTT is disconnected or compiled out, skip
silently; the orphaned-HA-device case and manual cleanup get a paragraph in the MQTT
docs.

## Section 5 — Testing, builds, docs

- **Native tests** — new `test_device_identity` suite:
  - validation matrix: valid / invalid chars / uppercase→lowercased / empty /
    25-char reject / hyphen edges;
  - resolver fallback: valid EEPROM name / empty slot / bad magic → chip default;
  - composed-SSID length invariant: `len(name) + len(suffix) ≤ 32` for all suffixes.
- **Extend `test_eeprom_settings`**: new-slot contiguity, bounds, v4→v5 migration
  zero-fill, round-trip.
- **Builds**: both `espmaster` and `espmaster_mqtt` envs must build (cleanup code is
  `#if MQTT_ENABLE` gated).
- **Docs**: CLAUDE.md EEPROM layout section; MQTT docs (rename caveat); README note —
  prefer DHCP reservations for multi-display LANs; update any `flashing/` walkthrough
  text that mentions the `split-flap-recovery` SSID (now `<name>-rec`).

## Out of scope

- Live re-init of mDNS/hostname/MQTT on rename (reboot-to-apply is v1).
- Runtime MQTT broker configuration (declined 2026-07-05; promotion path noted in
  #121).
- Dynamic unit count (#123) and captive-portal WiFi fallback (#126) — next in the
  release arc, both build on this identity work.
