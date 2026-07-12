# v2 MQTT + Home Assistant slice (#224, #58 slice)

Port of v1's `ServiceMqttFunctions.ino` — the hardware-accepted #121/#132
behavior (22 HA discovery entities, availability, telemetry, five command
topics, notification show-then-revert) — onto the v2 task model. The wire
contract (topics, discovery ids, payload shapes) is unchanged, so a display
whose master migrates from v1 to v2 keeps its HA entities and history.

## Library

v1's AsyncMqttClient is abandoned upstream (last push Sep 2024; no dvarrel
fork exists for MQTT). v2 uses **bertmelis/espMqttClient ^1.7.3**: actively
maintained, dependency-pinned against the same ESP32Async stack we already
use, MQTT 3.1.1, QoS 0/1/2, retained + LWT. We run the client from
`mqttTask` (the skeleton task from #187) and keep the v1 copy+flag
discipline by routing every inbound message through the existing
`MqttInboxMessage` queue regardless of which context the library fires
callbacks in.

## Layout

- `MqttLifecyclePolicy.h` (new, pure, natively tested): reconnect backoff
  (2 s doubling, 30 s cap, reset on connect), wraparound-safe due check,
  command-topic classification (the strcmp dispatch from v1's onMqttMessage,
  including "text/set matched last, unknown topics fail closed").
- `MqttService.h/.cpp` (new, target glue): owns the espMqttClient instance +
  stable String copies of host/credentials/client id/topics (the settings
  Strings can be reassigned by a web POST mid-connection); WiFi-gated
  connect; on-connect sequence (subscribe ×5, retained `online`
  availability, 22 retained discovery configs, retained diagnostics,
  publish-tracker reset); LWT retained `offline`; inbox drain + command
  application; on-change state publishers; 60 s telemetry; notification
  dwell tick; rename discovery-clear; OTA freeze/resume.
- `Tasks.cpp`: `mqttTaskMain` gets its real body (drain + tick loop);
  `MqttInboxMessage` grows to fit the wire (topic 64 B — longest command
  topic is `splitflap/<24-char id>/alignment/set` = 48 chars; payload 260 B —
  `MQTT_MAX_TEXT_LEN` 256 + NUL), sizes static_asserted against the topic
  builder; clockTask's tick gates on the notification overlay.
- `WebEndpoints.cpp`: real `/mqtt/discover` POST/GET replacing the 501 stubs
  (POST arms a staged flag, netTask runs the blocking `MDNS.queryService`
  pass and caches JSON from the already-ported `buildDiscoverJson`; GET
  answers 202 while pending, 200 with the cached JSON — same contract the
  shipped script.js already speaks); mutex-guarded content setters for the
  MQTT commands; `mqttConnected` in `/settings` wired to the service;
  device-rename POST stages the old id for a discovery clear; web message
  send / mode switch cancels an active notification (v1 parity).

## Command application (all through existing seams)

Commands never touch display state directly (hard rule): mode/speed/
alignment go through new mutex-guarded setters that update the web content
state and persist via the settings store; restart stages the existing
`pendingReboot`; `text/set` enqueues a baked `ShowText` DisplayCommand.
While `reflashInProgress(snapshot.reflash)` every MQTT display mutation is
dropped with a log line (producer gate, #205; HA command topics are
live-only — retained commands are already discarded on receipt, v1 parity).

## Notification show-then-revert

`text/set` starts the already-ported `MqttNotification` state (dwell default
60 s, clamp 5..3600) and enqueues the text; the state lives in MqttService,
exposed as an atomic `mqttNotificationActive()` that clockTask checks before
enqueueing clock content. Expiry in text mode re-enqueues the retained input
text; in clock mode expiry just releases the gate (clock re-shows within
1 s). Explicit mode/text changes (web or MQTT) cancel the overlay first.
Web transient text stays dropped — that is #219's scope; it will ride the
same overlay when ported.

## Deliberate deviations from v1

1. **No 60 s blocking unit-health poll from the MQTT loop.** displayTask
   owns the bus (#203). `unitErrors` telemetry rides the per-show write
   errors threaded into the snapshot; `units_faulty`/`units/attrs` update
   when probes run. HA sees probe-time facts, not a private 60 s poll.
2. **`diag/boots`** — v2 has no RTC boot counter; a u32 NVS counter
   incremented once in setup() replaces it.
3. **Discovery `mdl`** — the shared MqttHelpers.h default stays
   `"v1 ESPMaster"`; v2 overrides via a `MQTT_DEVICE_MODEL` define
   (`"v2 Master"`) so the header copies stay mergeable.
4. **Overlay gate placement** — v1 gated loop()'s mode block via
   `mqttNotificationTick()`; v2 gates clockTask's enqueue decision, same
   observable behavior.

## Out of scope

Web transient text + mode service (#219), TLS, QoS 2 flows, any new
entities beyond the v1 set.
