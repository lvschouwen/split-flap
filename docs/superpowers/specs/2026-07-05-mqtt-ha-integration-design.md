# MQTT / Home Assistant Integration — ESPMaster v1

**Date:** 2026-07-05
**Status:** Approved design, pending implementation plan
**Target:** `firmware/v1/ESPMaster` (ESP8266 ESP-01, 1 MB flash, ~42 KB free heap at rest)

## Goal

Let Home Assistant (or any MQTT client) push notification-style text to the
split-flap display and monitor its health, without touching the OTA path,
the EEPROM settings layout, or RTC boot state in any way.

## Scope

In scope:

- **Inbound text**: HA publishes to a command topic → display shows the text
  for a dwell time, then reverts to whatever was showing before
  (notification semantics).
- **Health telemetry**: periodic publish of free heap, WiFi RSSI, and count
  of units whose last I2C transaction failed.
- **Availability**: retained `online` + MQTT Last Will `offline`.
- **HA MQTT Discovery**: display announces itself; HA auto-creates one
  device with a Text entity and three sensors. No HA-side YAML.

Out of scope (explicitly rejected):

- Mode/settings control over MQTT (alignment, speed, clock/text switching).
- Publishing the currently displayed text as state.
- TLS — not feasible in ESP-01 RAM. Plain MQTT on the trusted LAN only.
- Runtime (web UI / EEPROM) broker configuration — compile-time only.
  May be promoted to EEPROM-backed config in a future issue if broker
  changes turn out to be frequent.

## Architecture

- New sibling sketch file `ServiceMqttFunctions.ino` (part of the single
  ESPMaster translation unit). Prototypes that the Arduino preprocessor
  cannot auto-generate go in `ESPMaster.h`.
- Library: **marvinroger/async-mqtt-client**, pinned to an exact commit in
  `platformio.ini` (same convention as the dvarrel async libs). It runs on
  ESPAsyncTCP — the stack the firmware already uses — so connect, publish,
  and receive never block the main loop.
- Everything is guarded by `#define MQTT_ENABLE`, **default `false`** in
  committed source (same pattern as `SERIAL_ENABLE`). With the flag off the
  feature is compiled out entirely: zero flash, zero RAM, zero behavior
  change.
- Credentials in `MqttCredentials.h` (gitignored;
  `MqttCredentials.h.example` committed): broker host, port, username,
  password, and a device id defaulting to `mdnsName`.

## Topics

| Topic | Direction | Payload |
|---|---|---|
| `splitflap/<id>/text/set` | subscribe | Plain text → show with default dwell (`MQTT_TEXT_DWELL_S`, 60 s). Payload starting with `{` → minimal parse of `{"text":"…","dwell":N}` for per-message dwell. |
| `splitflap/<id>/availability` | publish, retained | `online` on connect; LWT `offline`. |
| `splitflap/<id>/telemetry` | publish, every 60 s | One JSON object: `{"heap":…,"rssi":…,"unitErrors":…}`. HA sensors extract fields via `value_json` templates — one packet serves all three sensors. |
| `homeassistant/text/<id>/config` etc. | publish, retained | Discovery payloads for 1 Text entity + 3 sensors sharing one device block. Built with `snprintf_P` from PROGMEM templates on each connect; transient RAM only. |

No ArduinoJson dependency: the `{"text":…,"dwell":N}` parser is a ~30-line
hand-rolled helper using the existing string handling, covered by native
tests. Malformed JSON falls back to treating the payload as plain text.

## Show-then-revert semantics

1. Message arrives → save current device mode and currently displayed text
   in RAM, show the new text through the existing flap-write path.
2. A `millis()` deadline restores the saved state when the dwell expires
   (clock mode resumes ticking; text mode gets its previous text back).
3. A new message while one is showing replaces it and resets the deadline.
4. **No EEPROM writes, ever.** MQTT text is ephemeral: no settings churn,
   no flash wear, no interaction with OTA-relevant slots.
5. Dwell is clamped to 5–3600 s.

## Lifecycle and OTA protection (non-negotiable)

- MQTT initializes **only in a normal boot** — never in quiet-OTA mode or
  recovery mode.
- When a `/firmware/master` upload starts, MQTT disconnects cleanly,
  alongside the existing #116 display freeze.
- Reconnect is event-driven with exponential backoff (2 s → 30 s cap) and
  only attempted while WiFi is connected. Broker down = silent retry;
  display remains fully functional.
- async-mqtt-client callbacks run in LWIP context: handlers only copy the
  payload into a static buffer and set a flag. All display/I2C work happens
  in the main loop.
- **Zero new EEPROM slots, zero RTC memory use.** `SettingsEepromLayout.h`
  and `RtcBootState` are byte-for-byte untouched.

## Input handling

- Incoming text goes through the same `cleanString`/width handling as web
  UI input (uppercase/charset cleaning, truncation to `UNITS_AMOUNT`).
- Oversized or garbage payloads can never reach the I2C layer unclamped.

## Size budget (measured gates, not estimates)

Estimated cost: ~10–12 KB flash, ~1 KB RAM. Before merge, two measurements
are mandatory:

1. **Heap gate**: free heap at rest with `MQTT_ENABLE` on, compared against
   the current ~42 KB baseline. Regression must be small and understood.
2. **OTA staging gate**: sketch size from the build output must still fit
   the 1 MB flash split (sketch + OTA staging). A sketch that outgrows
   staging bricks web-OTA — the failure mode this project just spent a week
   eliminating.

## Testing

Native (`pio test -e native`):

- Payload parser: plain text, valid JSON, malformed JSON fallback, dwell
  clamping bounds.
- Show-then-revert state machine with injectable `millis()`: revert timing,
  replacement resets deadline, clock/text restore paths.
- Topic and discovery-JSON assembly from PROGMEM templates (valid JSON,
  correct ids).

Hardware (against live HA + mosquitto on LAN):

- Discovery: device + 4 entities auto-appear in HA.
- Text shows, reverts after dwell; second message replaces first.
- LWT: pull power → HA shows unavailable.
- Telemetry updates every 60 s.
- **Acceptance test:** a full web-OTA flash of a `MQTT_ENABLE`-on build
  returns SUCCESS via `flashing/ota-master.sh`, twice in a row.

## Rejected alternatives

- **PubSubClient**: synchronous `connect()` blocks on TCP timeout when the
  broker is down → multi-second main-loop stalls that freeze flap animation
  and delay I2C. Backoff mitigates but never removes the stall.
- **Hand-rolled MQTT over ESPAsyncTCP**: reimplementing keepalive/LWT/QoS
  for a solved problem is pure risk.
- **ArduinoJson**: overkill for one two-field object on a RAM-starved chip.
