# Web UI visibility wave — unit wear, S3 telemetry, split-flap identity

Approved 2026-07-12. Three independently shippable slices under one design so the
UI stays coherent. Absorbs #231 (slice A); slices B and C get their own issues
under epic #226. Implementation order A → B → C (A has the firmware round-trip;
C styles the data A and B put on screen). One branch + PR per slice.

## Goals

- Deeper visibility into per-unit health and mechanical wear (new Unit firmware
  is in scope).
- Surface the S3's own vitals — today `/health` returns the literal string
  "Healthy" and nothing else is exposed.
- A distinctly more polished UI that carries the split-flap identity.

Out of scope: supply-voltage measurement (no sense divider on the devkit or the
units — PCB v2 note filed instead), WebSocket transport (reserved for #229 live
preview), exposing the full telemetry firehose to Home Assistant.

## Slice A — unit wear odometer + deeper health (absorbs #231)

### Unit firmware (Nano, `firmware/v1/Unit`)

- `uint32` **revolution odometer**: +1 per hall pass (one full drum rotation =
  45 flaps). RAM counter, persisted to EEPROM every **128 revolutions**.
- **16-slot wear-leveled EEPROM ring**: each slot holds a 4-byte count; writes
  rotate through the slots; boot loads the maximum. Endurance: 16 slots ×
  100k writes × 128 revs ≈ 200M revolutions — far past mechanical death.
  Ring lives in a new EEPROM region; existing offset/address/counter slots
  untouched.
- Two new I2C opcodes (numbered from the existing command space, sketch-side
  only — twiboot untouched):
  - `CMD_GET_ODOMETER` — replies 4 bytes LE.
  - `CMD_RESET_ODOMETER` — zeroes counter + ring (for physical rebuilds:
    flap swap, new motor).
- Old unit firmware NACKs the opcode → master records "odometer unknown";
  nothing breaks. Status-byte bit 3 stays reserved (stuck-drum earmark).
- Flash impact: a few hundred bytes on the 328P. Bundled hex restaged per the
  established flow: code commit → clean rebuild → `make_manifest.py stage`
  (writes BOTH master trees) → separate artifact commit, never amended.

### v2 master

- `UnitFacts` gains `uint32 odometer` + `bool odometerValid`, read alongside
  the status probe (probe-time fact, same lifecycle as `offset`).
- New pure header **`WearPolicy.h`** (natively tested): given the valid
  odometers, compute the display median and flag any unit above
  `max(2 × median, median + 10 000)`. The absolute floor prevents false
  alarms on young displays. Constants are `#define`s, not settings.
- `GET /units/health` JSON gains per-unit `odo` and a `wear` object
  (median, flagged unit list).
- **Reset odometer** is a maintenance op like offset/home: DisplayCommand with
  `{"seq":N}` → `GET /unit/op-result`, validated by `MaintenancePolicy`,
  re-checked by displayTask pre-execute — inherits the probe-inhibit and
  reflash gates for free.
- v1 ESPMaster is NOT ported (retirement epic #217); Unit firmware serves both
  masters but only v2 reads the new opcodes.

### UI (Maintenance tab)

- Unit table: odometer column, relative wear bar (scaled to display max),
  alert badge on flagged units.
- Summary line, e.g. "unit 7 wearing 2.3× median".
- Per-unit reset-odometer action beside the existing calibration ops.

### Home Assistant

- One `binary_sensor` **"unit wear warning"** per display; flagged unit
  numbers + ratios in attributes. Discovery entity count 22 → 23.

## Slice B — S3 telemetry (System tab)

### Sampler + transport

- `SystemStats` sampler ticked by **netTask every 5 s** into a PSRAM ring
  (`LargeAlloc`, ~10 min depth ≈ 128 samples): RSSI, free heap, min-ever free
  heap, largest free block, PSRAM free, per-core CPU load, die temperature
  (labeled die temp, not ambient), uptime, NTP sync age, MQTT drop count,
  I2C transaction + error counters.
- I2C counters are incremented in displayTask/UnitBus and travel via
  `DisplaySnapshot` — the web layer never touches Wire (hard rule).
- CPU load via FreeRTOS runtime stats if the pioarduino sdkconfig permits;
  fallback: idle-hook tick counters. Same JSON either way.
- `GET /system/stats` returns current values + ring history in one JSON.
  Plain 2 s polling from the browser (existing fetch + setTimeout pattern);
  history lives server-side so a freshly opened tab shows the last ~10 min.

### UI

- New fifth tab **System**: vitals cards with inline-SVG sparklines (no chart
  library), link-stats card (I2C / MQTT / NTP), firmware card (version, A/B
  slot + boot state from the `/debug/ota` data, reset reason).

### Home Assistant

- Three sensors: RSSI, free heap, uptime. Everything else stays web-only.

## Slice C — split-flap identity redesign

- The flap mirror becomes a real animated split-flap: CSS flip transitions
  per character.
- Departure-board typography for headings and large values; consistent card /
  chip / button system; smooth tab transitions; proper mobile layout pass.
- Structure, tabs, and endpoints untouched — `style.css` rewrite + targeted
  `index.html`/`script.js` changes only.
- **Gate: slice C starts with a static mockup for user sign-off before any
  production code** (standing design-confirm rule).

## Testing

- Pure logic native-tested per the copy policy: WearPolicy.h, EEPROM-ring
  slot selection (host-testable helper), SystemStats ring/JSON shaping,
  /units/health JSON additions.
- Hardware glue (hall increment, EEPROM persistence timing, I2C opcode
  round-trip, sampler tick) is bench-verified on the 16-unit display — the
  E2E tier for this project.
- Risk-tiered review: slice A touches EEPROM + I2C contracts → full reviewer
  gate; B and C batched per branch.

## Non-goals / declined

- Voltage sensing (hardware change; PCB v2 doc note only).
- WebSocket transport now (#229 owns that).
- Full-telemetry HA mirror (~15 mostly-noise entities).
- Absolute wear-life thresholds (no real 28BYJ-48 drum-life data; relative
  alerts instead).
