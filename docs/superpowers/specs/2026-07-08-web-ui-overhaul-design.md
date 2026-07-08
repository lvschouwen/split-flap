# Web UI Overhaul — "Departure Board"

Full rethink of the ESPMaster web UI. Mockup (approved): https://claude.ai/code/artifact/02f0cc00-3ddd-459d-9ab4-14dc9166f8bc

## Goals

- The UI's identity is a **live mirror** of the physical display: a row of `displayWidth` flap tiles showing the current content, animated on change, visible on every view.
- Departure-board aesthetic: charcoal ground, black flap cells with a split line, signage amber accents, system mono for flaps/numerals/labels, system sans for UI text. Single dark theme.
- Same device capabilities as today, re-housed; four additions: animated mirror, unit-health strip, transient "show for N then revert" messages, first-run setup flow.

## Structure

Persistent board header (mirror + health strip) above hash-routed panels:

- **Home** — composer (message + duration select + Send), mode/alignment/speed controls (speed applied live).
- **Settings** — Device (name, timezone), MQTT broker (host/port/user/password, detect, connected pill, "applies on reboot"), WiFi (current network/IP, forget-and-portal action).
- **Maintenance** — calibration (transient test letter, re-home, per-unit expect/reality/offset table with fw revision), firmware (master upload with client-side MD5, unit reflash, flash diagnostics), log viewer.
- **First-run wizard** — replaces Home when `/settings` reports empty `deviceName` AND empty `mqttHost`: ① welcome/verify display, ② name the device, ③ MQTT or skip. Skip always lands on Home; never blocks.

## Layout system

Tiles are fluid: `flex: 1 1 0`, `max-width: 54px`, `aspect-ratio: 25/36`, glyph sized in container-query units — one layout fits any display width 1..16 with no media queries and no horizontal scroll. Health strip segments flex identically so they align with their tiles.

Palette tokens: ground `#17181C`, surface `#1E2025`, cell `#0B0C0E`, flap text `#F2F3F0`, ink `#D5D8DE`, muted `#9AA0AB`, amber `#FFB000`, ok `#3FBF6F`, warn `#E05B4B`, line `#2E3138`.

## Data flow

- Mirror + status: poll `GET /settings` every 5 s (existing endpoint, no new fields required — uses `lastWrittenText`, `deviceMode`, `unitCount`, `deviceName`, `mqttConnected`, flash diagnostics).
- Health strip: poll `GET /units/health` every 30 s and after unit actions.
- All writes go through the existing endpoints unchanged (`POST /` with per-field provided-flag semantics, calibration opcodes, `/firmware/master`, `/mqtt/discover`, …). Recovery/OTA-mode minimal pages and the flasher are untouched.

## Firmware change (the only one)

Generalize the transient-text mechanism: POST field `transientText` + optional `transientDwell` (seconds, valid 5–3600, default 600). Replaces the `calibrationText` field; calibration posts `transientText` with the default dwell, the composer posts user-chosen dwells (5 min / 15 min / 1 h). Rides the notification show-then-revert state; explicit mode change or plain message send cancels early. Dwell validation lives in `SettingsValidation.h` (natively tested).

## Constraints

- Vanilla JS, no webfonts, no external resources; assets PROGMEM-gzipped by `build_assets.py`.
- **Size budget: total gzipped assets ≤ 24 KB** (today: 20.2 KB). Amended post-spec: the #56 provisioning panel (out of this spec's scope) adds ~1.3 KB; ceiling for the combined UI is 25.5 KB. Sketch must stay comfortably under the ~502 KB OTA ceiling; flag at 490 KB.
- `CALIBRATION_LETTERS` in script.js stays byte-synced with `SFP_ALPHABET` (build-time verify).
- `/settings` JSON shape unchanged (external consumers: flasher, ota-master.sh).
- Accessibility: visible focus states, `prefers-reduced-motion` disables flips, semantic controls.

## Testing & acceptance

- Firmware: TDD in the native suite (dwell validator, transient param staging semantics via existing patterns).
- JS: node DOM-mock flow checks for the send paths, wizard gating and mirror padding; alphabet-sync enforced by existing pytest.
- Build gate: two-bake reproducibility and gzip size printed per asset; budget asserted manually per phase.
- Hardware acceptance before soak flash: mirror tracks physical display, transient reverts on a clock display with MQTT unconfigured, calibration round-trip, firmware upload from the new Maintenance view.

## Phases

1. Design system + board header (mirror, health strip) + Home (composer/transient, mode/alignment/speed) + firmware `transientText`/`transientDwell`.
2. Settings + Maintenance ports (calibration table, firmware flows, log).
3. First-run wizard + polish + size/accessibility pass.

Each phase ends green (native + pytest + build) and committable; bench acceptance after phase 3.
