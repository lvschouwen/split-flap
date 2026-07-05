# Web UI Restructure (#128) + MQTT Broker Auto-Detect (#129) — Design

**Date:** 2026-07-05
**Issues:** [#128](https://github.com/lvschouwen/split-flap/issues/128), [#129](https://github.com/lvschouwen/split-flap/issues/129)
**Scope:** `firmware/v1/ESPMaster` web assets (`data/index.html`, `data/script.js`, `data/style.css`) + small backend additions in `ESPMaster.ino` / `ServiceMqttFunctions.ino`.

## Problem

The web UI grew feature by feature into one stacked page: text input, mode, alignment,
speed, timezone, device name, MQTT config, actions bar, calibration, firmware upload,
log. The single Submit button under the General card is ambiguous about what it saves
and sits far from fields it applies to. Since #57, the MQTT broker must be typed in by
hand even when Home Assistant is discoverable on the LAN.

## Decisions (confirmed with user 2026-07-05)

1. **Structure:** three tabs on one page — Display / Settings / Maintenance.
2. **Save model:** per-card Save buttons on the Settings tab, with inline
   reboot-required badge + "Reboot now" button where applicable.
3. **Live apply:** alignment and flap speed apply immediately on change from the
   Display tab; "Send to display" submits only the message (text + mode).

## 1. Tab structure

One HTML document (unchanged PROGMEM model). A tab bar sits below the orange header;
vanilla JS toggles a `hidden` class on three `<section>` elements. `location.hash`
(`#display`, `#settings`, `#maintenance`) deep-links a tab and survives refresh;
default (no/unknown hash) is Display. The header strip (device, version, units, last
received) stays global above the tab bar. No page reloads; all tab state is DOM-local.

Log polling remains visibility-aware (`visibilitychange`) and additionally pauses
whenever the Maintenance tab is not the active tab.

## 2. Tab contents

### Display (daily use)

- **Message card:** Clock/Text mode radios, text input with character/line counters
  and "+ Newline", **Send to display** button. Posts only `deviceMode` + `inputText`.
- **Presentation card:** alignment radios (POST on click) and speed slider (POST on
  release — `change` event, not `input`), each showing a transient "saved" hint.
  One EEPROM write per deliberate change.
- **Stop** action button (existing `POST /stop`).

### Settings (set-once)

- **Device card:** device name + timezone, **Save Device** button. After a save that
  changed the device name: inline "applies after reboot" badge + **Reboot now**.
  Timezone continues to apply immediately (existing `applyTimezoneAndNtp()` path) —
  no reboot badge for timezone-only saves.
- **MQTT broker card:** host / port / user / password (password stays write-only,
  empty = keep stored), a connection status indicator driven by `mqttConnected` +
  `mqttHost` from `/settings` (connected / disabled / not connected), **Detect
  broker** button (#129, section 4), **Save MQTT** button + reboot badge on change.
- **WiFi card:** the **Reset WiFi** action (existing `/reset-wifi`) with its
  explanation text; shown/hidden via `wifiSettingsResettable` as today.

### Maintenance

- **Units card:** Reset Unit Calibration + Flash all units actions, then the entire
  existing Calibration UI unchanged (test-letter flow + advanced jog/offset
  `<details>`). The calibration rows remain fully dynamic: they are built from
  `detectedUnitAddresses` in `/settings`, so a 16-unit display renders 16 rows with
  no UI change. (Pre-#28 units that don't answer `CMD_GET_VERSION` stay filtered
  out, as today.)
- **Master firmware card:** existing `.bin` upload form, plus the **OTA Update
  mode** and **Reboot** actions.
- **Log card:** existing log panel (`<details>` + polling).

The old Actions bar dissolves into these homes; every existing action keeps a home:
Stop → Display; Reset WiFi → Settings; OTA Update, Reboot, Reset Unit Calibration,
Flash all units → Maintenance.

## 3. Backend changes (small, backward compatible)

- **Provided-gating for the remaining `POST /` params.** `alignment`, `flapSpeed`,
  `deviceMode`, `inputText` gain `xProvided` flags, mirroring the existing pattern
  for timezone / deviceName / MQTT, so a partial per-card POST cannot clobber
  absent fields. `inputText` is applied only when provided and the effective mode is
  text. A full form post (all fields, as any old client sends) behaves exactly as
  before.
- **Ajax response mode.** Card saves use `fetch()` and include `ajax=1`. When set,
  the handler answers `200` (body distinguishes plain save vs reboot-required, e.g.
  `ok` / `ok-reboot`) or `400 invalid` instead of the redirect. Without `ajax=1` the
  redirect behavior is unchanged.
- `/settings` JSON is unchanged (additions only if needed); `flashing/ota-master.sh`
  and any bookmarks keep working.

## 4. #129 — Detect broker (mDNS)

`MDNS.queryService()` blocks ~1 s per query, which must not run inside an async
web handler (async_tcp context). The query is deferred to `loop()`:

- **`POST /mqtt/discover`** — sets a pending flag, returns `202` immediately.
  Rejected with `409` while a discovery is already pending/running.
- **`loop()`** — when the flag is set (and WiFi is in STA mode with mDNS running):
  query `_mqtt._tcp`; if zero answers, query `_home-assistant._tcp` (candidates from
  that source get suggested port 1883 — Mosquitto add-on on the HA host is the
  common case). Cache a compact JSON result. Both queries bounded; answers freed
  after the JSON is built (verify RAM headroom on ESP-01 during bench test).
- **`GET /mqtt/discover`** — returns `{"status":"pending"}` or
  `{"status":"done","candidates":[{"host":"…","ip":"…","port":1883,"source":"mqtt"}]}`
  (`source` ∈ `mqtt` | `home-assistant`). `{"status":"idle"}` before any run.
- **JS flow:** Detect click → POST → poll GET every 500 ms, 10 s budget. Exactly one
  candidate → prefill host + port fields (editable; nothing persists until Save
  MQTT). Multiple → render clickable suggestions under the host field. None/timeout
  → "No broker found on the LAN" hint. Button disabled while pending.
- **Pure logic:** candidate → JSON formatting lives in a new header
  (`MdnsDiscovery.h`) so it is natively testable without the mDNS stack.
- **Credentials stay manual** by design; README gains a note recommending a
  dedicated `splitflap` Home Assistant user.

Non-goal (from #129): zero-touch pairing.

## 5. Size budget

Assets are gzipped into PROGMEM by `build_assets.py`. Expected delta ≈ +1.5–2 KB
gzipped total (tab bar + restructured markup, per-card fetch saves, discover
polling, small CSS additions). Measure gzipped sizes before/after and report in the
PR; firmware-side additions (~discover endpoint + gating) are a few hundred bytes.

## 6. Error handling

- Failed card save (network or `400`): red inline badge on that card with a short
  message; never silent.
- Discover timeout/error: inline hint; UI never blocks; Detect button re-enables.
- Invalid field values keep being rejected server-side exactly as today (#95-style
  validation retained).

## 7. Testing

- **Native (`pio test -e native`):** new test for `MdnsDiscovery.h` (JSON shape,
  empty/one/many candidates, source port defaulting); existing suites stay green.
- **Python (`pytest tests/`):** unchanged, must stay green.
- **Manual browser pass** on the device (or `SERIAL_ENABLE` standalone mode): each
  tab renders; deep-link hashes; live alignment/speed apply; each per-card save +
  reboot badge; write-only password behavior; detect flow (HA present + absent);
  calibration + firmware + log unchanged on Maintenance; old-style full form POST
  (backward-compat check).

## Out of scope

- Zero-touch MQTT pairing / anonymous broker access (#129 non-goal).
- Units health panel (#45) — the Maintenance tab gives it a natural home later, but
  it is not part of this change.
- Any visual redesign beyond the restructure (colors/typography stay as-is).
