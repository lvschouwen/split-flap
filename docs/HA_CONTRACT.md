# Home Assistant MQTT contract

The Home Assistant configuration (the separate hass-config repository) drives
the wall and alerts on it through the MQTT interface the v2 Master publishes.
This page lists what that configuration consumes, so a change here does not
silently break it. The code is the source of truth; this page names the
places that must not drift.

`<id>` is the board's device id, `split-flap-<chipid>` (never write a real one
in this repository). Topics live under `splitflap/<id>/`; discovery configs
under `homeassistant/<component>/<id><suffix>/config`.

## The rule

**Topic names, discovery unique ids (`<id><suffix>`), JSON attribute keys,
payload words (`ON`/`OFF`, `PRESS`, `INSTALL`) and enum values (event types,
select options) are append-only.** Adding is free. A rename, removal or change
of meaning is a breaking change: open the matching hass-config issue first,
land both sides together, and flag it in the release notes. The
`MqttDiscoveryEntity` enum itself is append-only too (see its comment in
`MqttHelpers.h`).

## Commands HA sends

| Topic | Payload | Defined in |
|---|---|---|
| `splitflap/<id>/text/set` | Plain text (default dwell, priority 0), or JSON `{"text":"…","dwell":N,"hold":true,"priority":0..255,"force":true}`; `force` = priority 255, the only level that passes Quiet. Malformed JSON is shown as plain text. | `MqttHelpers.h` (`parseMqttTextRequest`), design: `docs/superpowers/specs/2026-07-05-mqtt-ha-integration-design.md` |
| `splitflap/<id>/text/clear` | any | `MqttHelpers.h` (`_text_clear` button) |
| `splitflap/<id>/quiet/set` | `ON` / `OFF` (retained by HA) | `QuietPolicy.h` |

The notify entity (`<id>_notify`) publishes to `text/set`, so
`notify.send_message` with a JSON message is the same contract.

## What HA reads

| HA entity (as hass-config names it) | Unique id | State topic / field | Attributes HA reads |
|---|---|---|---|
| `notify.splitflap_message` | `<id>_notify` | command only (`text/set`) | — |
| `button.splitflap_clear_text` | `<id>_text_clear` | command only (`text/clear`) | — |
| `switch.splitflap_quiet` | `<id>_quiet` | `quiet` (`ON`/`OFF`) | — |
| `sensor.splitflap_current_text` | `<id>_text_state` | `text/state` | — |
| `event.splitflap_text_result` | `<id>_text_result` | `text/result`, event types `shown`, `held`, `refused`, `dropped_quiet`, `dropped_busy`, `cleared` | `event_type`, `text`, `priority`, `hold`, `dwell`, `seq`, `truncated`, `unsupported` (`MqttTextResult.h`) |
| `sensor.splitflap_faulty_units` | `<id>_units_faulty` | `units_faulty` (integer) | from `units/attrs` (`buildUnitHealthJson`, `shared/UnitHealth.h`): `width`, `faulty`, `vccMin`, `units[]` with `i`, `vmin` |
| `sensor.splitflap_unit_supply_vcc_min` | `<id>_vcc_min` | `telemetry` → `vccMin` | `unit`, `board`, `since_s`, `under_floor` (from the discovery attribute template) |
| `sensor.splitflap_boot_counter` | `<id>_boots` | `diag/boots` | — |
| `sensor.splitflap_free_heap` | `<id>_heap` | `telemetry` → `heap` | — |
| `sensor.splitflap_heap_fragmentation` | `<id>_heap_frag` | `telemetry` → `heapFrag` | — |

Availability for every entity is `splitflap/<id>/availability`
(`online` / LWT `offline`). Per-board problem sensors (`<id>_board_<board>`,
attributes on `board/<board>/attrs`) and the wall problem sensor
(`<id>_wall_problem`, `wall/attrs`) carry a per-board `faulty` count
(`WallMqtt.h`); they fall under the same rule.

HA derives an entity id from the device name and the entity `name` when it
first sees a unique id, and keeps it in its registry after that. hass-config
renamed the device, which is why its ids read `splitflap_*`. Changing an
entity `name` therefore only renames fresh installs, but changing a unique id
orphans the existing entity: treat both as breaking.

## Where the contract lives in code

- `firmware/v2/Master/MqttHelpers.h` — topic builder, text payload parser,
  telemetry JSON, discovery payloads and unique-id suffixes
- `firmware/v2/Master/WallMqtt.h` — per-board and wall problem sensors
- `firmware/v2/Master/MqttTextResult.h` — the text-result event
- `firmware/v2/Master/QuietPolicy.h` — the Quiet switch
- `firmware/v2/shared/UnitHealth.h` — the `units/attrs` JSON
- `firmware/v2/Master/MqttService.cpp` — what is published when
