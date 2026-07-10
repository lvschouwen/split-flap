# v2 I2C unit bus — design (slice A of the I2C port)

Approved 2026-07-10 (brainstorm with Lucas). Epic #183 / meta #58. Slices:
**A** = this spec (text + probe + health). **B** = calibration/provisioning.
**C** = unit reflash over twiboot. B and C reuse A's bus module and get their
own specs when they start.

## Goal

`displayTask` drives the real, unchanged v1 units over I2C: web/clock text
physically moves flaps, the boot probe derives the true display width, and
`/units/health` serves real per-unit status. Bench target: the 5-unit v1
display with the ESP-01 unplugged and the S3 devkit jumpered onto its bus.

## Decisions (with rationale)

- **Straight port of v1's proven behavior** into the displayTask model — same
  blocking Wire transactions and timing constants (2 ms opcode settle, 30 s
  stuck-unit timeout, 1500 ms twiboot pre-probe delay). displayTask replaces
  v1's `loop()` as the blocking context; commands queue behind long
  operations by design. No async FSM redesign: it would discard bench-proven
  timing for new logic with nothing to gain while displayTask owns nothing
  else.
- **Pins SDA = GPIO 8, SCL = GPIO 9** (Arduino-ESP32 S3 defaults), 100 kHz,
  3.3 V — electrically a drop-in for the ESP-01. Clear of reserved pins
  (4 button, 19/20 USB, 35–37 PSRAM, 48 LED).
- **`SplitFlapProtocol.h` stays genuinely shared** from `firmware/v1/shared/`
  (both v2 envs already `-I` it): it is the wire contract with unchanged
  units — single source of truth is correct here, unlike master-internal
  logic which follows the v2 copy policy.
- **Snapshot carries facts, web builds JSON** (deviation from v1): v1 cached
  prebuilt health JSON in RAM (ESP-01 memory tactic). v2's `DisplaySnapshot`
  gains per-unit POD facts (~350 B: state 0 silent / 1 sketch / 2 bootloader,
  `UnitStatus`, version); the web layer renders JSON from its mutex copy via
  the pure builder. Cleaner ownership, no stale-cache invalidation.
- **v1's `volatile` staging flags dissolve** (`busReprobePending`,
  `i2cBusBusy`, …): the command queue IS the staging mechanism; bus
  exclusivity is structural (only displayTask touches Wire — standing #187
  rule).

## Components

- `UnitBus.cpp/.h` (new, target glue; the ONLY Wire toucher, called from
  displayTask exclusively): `queryUnit` write-opcode→settle→read-n→drain
  core; send letter index + speed; probe scan; `CMD_GET_STATUS` (8-byte
  UnitStatus decode); version read; `CMD_GET_LETTER` readback.
- `FlapFrame.h` (new, pure, natively tested): text → per-unit letter indices
  for a given width/alignment — uppercase/clean mapping into the
  `SFP_ALPHABET` table, unknown chars → blank, truncate/pad per alignment.
  Extracted from v1's `showText` body (which was never a pure seam); v1
  keeps its own code — no retrofit.
- Pure copies from v1 (natively tested in v2 per the copy policy):
  `UnitProtocolHelpers.h` (`letterReadbackValid`), the health-JSON +
  faulty-count builders, web→unit speed mapping (1..100 →
  `MIN_SPEED..MAX_SPEED`).
- `DisplayIpc.h`: snapshot extension (width already exists; add per-unit
  facts + busy semantics); `displayApplyCommand` grows the real transitions.
- `Tasks.cpp` displayTask: boot = Wire init → 1500 ms settle → probe →
  health poll → publish; loop = execute `ShowText` / `Probe` (re-scan +
  health refresh).
- `WebEndpoints.cpp`: un-501 `GET /units/health` (JSON from snapshot copy);
  `POST /units/health/refresh?probe=1` enqueues Probe. Async rule upheld —
  handlers enqueue, never touch the bus.

## ShowText data flow

Build frame (pure) → send to every detected unit → poll status bytes until
rotation stops (30 s cap per stuck unit) → `CMD_GET_LETTER` verify → one
resend round for mismatches → publish snapshot.

## Error handling (v1 parity)

Failed transmission → unit "silent" (state 0), rendered as a gap, never
faulty-counted. Short/invalid read → drain RX, fail without touching
outputs. Stuck unit → timeout, log, continue. Snapshot always published —
web and clock never block on a sick bus.

## Testing

- Native: FlapFrame (alignment × truncation × unknown chars × widths 1..16),
  health JSON + faulty count, speed mapping, `letterReadbackValid`.
- Bench (E2E tier, real 5-unit display): boot probe logs width 5; web
  message moves flaps; clock mode flips the display each minute;
  `/units/health` lists 5 units with revisions; a unit with SDA pulled shows
  as a gap after refresh, not a fault.

## Out of scope

Slice B (calibration/provisioning: offset/jog/home/address/IDENTIFY +
Maintenance UI), slice C (twiboot reflash — needs a long-running-job
pattern with progress, not a fire-and-forget command), MQTT health
telemetry (rides the future MQTT slice).
