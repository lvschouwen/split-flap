# Home Assistant Mode Select over MQTT (#130) — Design

**Date:** 2026-07-05
**Issue:** [#130](https://github.com/lvschouwen/split-flap/issues/130)
**Scope:** `firmware/v1/ESPMaster` — `MqttHelpers.h`, `ServiceMqttFunctions.ino`, the web-UI mode path in `ESPMaster.ino`, `test/test_mqtt_helpers/`.

## Problem

HA can push notification text to the display but cannot switch the device mode
(text/clock). Discovered while testing HA control 2026-07-05: after sending text
from HA there is no way back to clock mode without the web UI — and even there a
still-active notification masks the switch until its dwell expires, which reads
as "mode setting broken".

## Decisions (confirmed with user 2026-07-05)

1. HA gets a **`select` entity** (`text` / `clock`) via MQTT discovery.
2. Mode state is **published retained** so HA always shows reality, whatever the
   source of the change (HA command or web UI).
3. **An explicit mode change cancels any active notification** — from both the
   HA command path and the web UI — so the display re-flaps to the new mode on
   the next 1 s tick instead of waiting out the dwell.

## Design

### Discovery entity

- `DISCOVERY_MODE` added to `MqttDiscoveryEntity` (before `DISCOVERY_ENTITY_COUNT`).
- Config topic: `homeassistant/select/<id>/config`.
- Payload (abbreviated keys, same pattern as the existing four):
  `{"name":"Mode","cmd_t":"splitflap/<id>/mode/set","stat_t":"splitflap/<id>/mode","avty_t":"splitflap/<id>/availability","uniq_id":"<id>_mode","ops":["text","clock"],<device block>}`.
- Published retained by `publishMqttDiscovery()` with the existing truncation
  guard; the rename-clear loop iterates `DISCOVERY_ENTITY_COUNT`, so a device
  rename blanks this config automatically. The clear block additionally
  publishes one empty retained message to the old `splitflap/<oldid>/mode`
  state topic so no stale retained state lingers.

### Command path (HA → device)

- `initMqtt()` builds `mqttTopicModeSet` / `mqttTopicModeState`; connect path
  subscribes to `mode/set` alongside `text/set`.
- `onMqttMessage` (LWIP context — flag only) gains minimal topic dispatch:
  if the topic equals `mqttTopicModeSet`, copy the payload into a small fixed
  buffer (16 bytes) and set `mqttModeCommandPending`; retained messages are
  dropped exactly like the text path.
- `loopMqtt()` consumes the flag: `parseModeCommand()` (pure, `MqttHelpers.h`)
  trims and matches the payload against exactly `text` / `clock` (anything else
  → logged, ignored). Valid and different from the current mode →
  `deviceMode = <new>`, `saveDeviceMode()`, `notificationCancel(mqttNotification)`.

### State path (device → HA)

- `loopMqtt()` keeps `mqttLastPublishedMode`; while connected, whenever it
  differs from `deviceMode` (first connect included), publish `deviceMode`
  retained (QoS 0) to `splitflap/<id>/mode` and update the tracker. This covers
  boot, HA commands, and web-UI changes with no coupling to any source.

### Web-UI consistency

- The `POST /` handler's mode-change branch also calls the notification cancel
  (via the existing async-safe pattern: the handler only mutates state that
  `loopMqtt()`/the 1 s tick reads — `notificationCancel()` just clears the
  struct's `active` flag and text, which is safe to do from the handler since
  the ESP8266 request handlers and loop() never preempt each other mid-String;
  same reasoning the reviewers confirmed for the discover flags).

### Pure logic + tests (TDD, `test_mqtt_helpers`)

- `parseModeCommand(const String&)` → `""` (invalid) / `"text"` / `"clock"`;
  tests: exact match, whitespace trim, case-sensitivity (reject `TEXT`),
  garbage, empty.
- `notificationCancel(MqttNotification&)` → clears `active` + `text`; test:
  cancel mid-dwell makes `notificationTick()` return false immediately.
- `buildDiscoveryTopic/Payload(DISCOVERY_MODE, …)` → shape tests like the
  existing entities, plus the entity-count constant bump.

## Error handling

- Invalid mode payloads are ignored and logged — never applied, never crash.
- Discovery truncation guard already covers the new entity.
- Mode state publish only when connected; missed publishes self-heal on the
  next loop pass because the tracker only updates after a successful call.

## Out of scope

- Any web-UI change (the select is HA-side; the web UI already has mode radios).
- Additional HA entities (speed/alignment) — separate issue if ever wanted.

## Cost

~30 lines firmware + one subscription + one retained topic; discovery payload
well inside the 512-byte buffer; no EEPROM layout change; no web-asset change.
