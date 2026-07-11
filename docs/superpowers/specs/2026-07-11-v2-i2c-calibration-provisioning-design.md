# v2 calibration + provisioning over I2C — design (slice B of the I2C port)

Approved 2026-07-11 (brainstorm with Lucas; cross-reviewed with Codex, 2
rounds, converged). Epic #183 / meta #58 / issue #204. Builds on slice A
(#203, `2026-07-10-v2-i2c-unit-bus-design.md`): reuses its UnitBus module,
command queue, and snapshot model. Slice C (#205, twiboot reflash) stays
separate.

Operational constraint honored throughout: units are NEVER USB/ICSP-flashed
in operation — every provisioning mistake must be recoverable over the bus
or by DIP switches + power cycle. No design path here requires more.

## Goal

The Maintenance tab comes alive on v2: interactive calibration (offset
read/write, jog, home), provisioning (#56: set/clear EEPROM I2C address,
identify), the bootloader-entry debug endpoint, and the display-wide
`/reset-units` and `/stop`. The already-ported v1 web UI keeps working with
a small diff (result polling for critical ops).

## Decisions (with rationale)

- **Queue-native port** (chosen over a sync mailbox and over full async
  result-polling for everything): every op is a `DisplayCommand`; web
  handlers validate, enqueue, and return immediately — never wait on
  display work. FIFO order preserves the UI's chained flows without new
  synchronization and never blocks the async_tcp task.
- **Execution feedback via a single result slot** (Codex round 1): a bare
  "queued" 200 would let a later wire NACK masquerade as success on ops
  that mutate EEPROM. Commands carry a `uint32_t seq` (atomic counter,
  stamped at enqueue); the snapshot carries ONE `MaintResult {seq, opcode,
  addr, outcome, reason}`; displayTask publishes it for every maintenance
  op. POSTs return `{"seq":N}`; `GET /unit/op-result?seq=N` answers
  `pending` (slot hasn't reached N), the outcome (slot == N), or `expired`
  (slot advanced past N — UI treats as unknown/failure). The slot is a
  best-effort acknowledgement channel for ONE active critical op, not an
  audit log: the UI awaits critical ops serially and disables the
  Maintenance controls while awaiting; concurrent operators are out of
  scope on a single-display LAN device (ring buffer rejected as
  over-engineering).
  - UI awaits results for: WriteOffset, SetAddress, ClearAddress, Stop.
  - Fire-and-forget (operator watches the hardware): Jog, Home, Identify,
    RebootToBootloader, ResetUnits.
- **Queue full → HTTP 503, never silent drop** (Codex round 1, accepted as
  stated): a dropped SetAddress reported as 200 is false operator
  confidence.
- **Offset becomes a snapshot fact**: probe reads `CMD_GET_OFFSET` for
  every sketch-mode unit; displayTask patches the fact after a successful
  `WriteOffset`. Nothing else changes a stored offset (jog moves the drum,
  not the offset). `GET /unit/offset` answers synchronously from the
  snapshot copy — v1 wire contract (`{"offset":N}`) unchanged.
- **`/unit/reboot` keeps v1 semantics**: `SFP_CMD_ENTER_BOOTLOADER`
  (twiboot, ~1 s). The soft sketch reboot (`SFP_CMD_REBOOT`) is dropped
  from slice B — v1 exposes no endpoint for it. One opcode:
  `RebootToBootloader`.
- **Compound address ops**: `SetAddress`/`ClearAddress` execute burn →
  in-task ~3 s settle → probe → publish, so topology truth never depends
  on a UI timer racing the queue.
- **twiboot pinning hazard (HARD RULE, v1 #88)**: the probe's
  bootloader-detection query (`CMD_ACCESS_MEMORY`) sets twiboot's
  `boot_timeout = 0`, pinning it alive forever. NO auto-probe after
  `RebootToBootloader` (response text says the unit is briefly in
  twiboot); the 3 s settle in the compound address ops is deliberately
  longer than the twiboot window for the same reason (same class as slice
  A's 1500 ms boot delay).
- **`/stop` regains kill-switch semantics** via one `std::atomic<bool>`
  abort signal: the handler sets it only after Stop enqueues successfully;
  every UnitBus wait loop polls it and returns early (frames between the
  abort and the queued Stop still send but skip their waits — harmless,
  Stop homes everything after); displayTask clears it when Stop executes.
  This is a cancellation signal, not bus state — consistent with #203's
  dissolution of staging flags, which was about command data.
- **v1's `i2cBusBusy` / flash-window 503 guards dissolve** — the command
  queue is the serialization. Slice C re-introduces whatever guard the
  long-running reflash job needs, on its own terms.

## Components

- `DisplayCommand.h`: opcodes `WriteOffset`, `Jog`, `Home`,
  `RebootToBootloader`, `Identify`, `SetAddress`, `ClearAddress`,
  `ResetUnits`, `Stop`. New POD params: `uint32_t seq`, `uint8_t
  unitAddress`, `int16_t value` (offset steps / jog steps / new address —
  the opcode disambiguates; maker helpers are the only constructors).
  `ResetUnits` bakes the current snapshot text into `cmd.text` at enqueue
  time (senders bake params).
- `MaintenancePolicy.h` (new, pure, natively tested): web-boundary AND
  execution-time validation — address parse + 1..126 range,
  sketch-running-unit check (404), offset ±`SFP_OFFSET_LIMIT_STEPS` (400),
  jog −127..127 (400), set-address bound to `UNITS_AMOUNT` (400) and
  target-occupied (409; burning the current address always allowed).
  Returns a verdict struct (HTTP status + message id). The occupancy check
  runs twice: web boundary (fast 409 for the user) and displayTask
  immediately before the burn against its current facts (authoritative; on
  conflict → skip burn, outcome `exec-validation-fail`).
- `UnitBus.cpp/.h`: thin v1-port wrappers over the existing transaction
  core — `unitBusReadOffset`, `unitBusWriteOffset`, `unitBusJog`,
  `unitBusHome`, `unitBusRebootToBootloader`, `unitBusIdentify`,
  `unitBusSetAddress`, `unitBusClearAddress`, `unitBusBroadcastHome`
  (general-call 0x00 + `CMD_HOME`). Same 2 ms settle, same
  drain-on-short-read. Probe additionally reads `CMD_GET_OFFSET` per
  sketch unit. Wait loops poll the abort signal.
- `UnitHealth.h` `UnitFacts`: `int16_t offset` + `bool offsetValid`.
  Lifecycle: probe rewrites all facts wholesale (offsetValid set only on a
  successful read of a sketch unit); `WriteOffset` success patches in
  place; address mutations end in a probe (fresh facts);
  `RebootToBootloader` clears that unit's `offsetValid` + `statusValid`
  until the next probe.
- `DisplayIpc.h`: `MaintResult {uint32_t seq; uint8_t opcode; uint8_t
  addr; uint8_t outcome; uint8_t reason}` in the snapshot. Outcomes:
  `ok`, `wire-fail`, `exec-validation-fail`, `postcondition-fail`.
  For compound address ops, `ok` means the post-probe postcondition was
  OBSERVED (unit answers at the expected address), not merely that the
  EEPROM burn ACKed; `postcondition-fail` reasons distinguish
  `unit-missing-after-reprobe` / `address-occupied-after-reprobe` (the
  DIP-collision case after clear-address). `displayApplyCommand` grows the
  new transitions — `Stop` clears `currentText` (the clock re-sends on its
  next tick, v1 parity).
- `Tasks.cpp` displayTask: executes the new opcodes via UnitBus; publishes
  `MaintResult` after each. `ResetUnits` runs entirely in-task: full `'-'`
  row frame → 2 s → `'.'` row frame → re-show the baked text (v1's
  blank-out sequence that forces wrap-around recalibration). `Stop` =
  broadcast home + text clear + abort-flag clear.
- `WebEndpoints.cpp`: un-501 `GET/POST /unit/offset`, `POST /unit/jog`,
  `/unit/home`, `/unit/reboot`, `/unit/identify`, `/unit/set-address`,
  `/unit/clear-address`, `/reset-units`, `/stop`; new
  `GET /unit/op-result`. Handlers: snapshot copy → MaintenancePolicy →
  enqueue (full → 503) → `{"seq":N}`. `GET /unit/offset`: 502 when
  `offsetValid` is false (v1's "firmware may predate #32" case).
- `data/` UI: `postCalibration` gains an await-result mode (poll
  `/unit/op-result` at 500 ms, ~10 s cap; `expired` treated as failure)
  used by the critical ops; Maintenance controls disabled while awaiting.
  Clear-address confirm dialog warns the unit rejoins at its DIP address —
  ensure it is free. Verify the EEPROM==DIP twiboot warning survived the
  copy (over-I2C reflash only works while EEPROM and DIP agree).

## Data flow (mutation example: set-address)

UI POST → handler copies snapshot → MaintenancePolicy validates → enqueue
`SetAddress` (full → 503) → `{"seq":N}`; UI disables controls and polls
`/unit/op-result?seq=N`. displayTask: revalidate occupancy against current
facts → `unitBusSetAddress` (unit burns EEPROM + reboots off the bus) →
3 s settle → probe → publish facts + `MaintResult` (`ok` only if the unit
answers at the new address). UI re-renders from the refreshed health facts.

## Error handling

- Validation failure → 400/404/409 before anything queues (v1 parity).
- Queue full → 503.
- Wire NACK during execution → `MaintResult` `wire-fail` + serial log; no
  fact mutation. Snapshot always published.
- Postcondition failure after address ops → `postcondition-fail` with
  reason; facts show the observed truth. Worst case (DIP collision after
  clear) is recoverable by DIP + power cycle — documented in the UI.
- Stale result (`expired`) → UI reports unknown outcome, suggests a health
  refresh.

## Testing

- Native: MaintenancePolicy validation matrix (range × state × occupied ×
  limits, both call sites); `displayApplyCommand` for every new opcode
  (Stop text clear, WriteOffset fact patch, MaintResult publication,
  malformed-command rejection); maker helpers' exact wire-byte encoding
  for negative jog/offset; op-result state machine (pending/ok/expired);
  abort-flag one-shot semantics (set → waits skip → cleared at Stop).
  Existing suites stay green.
- Bench (E2E tier, combined session with #203's checklist, 5-unit
  display): jog moves one flap and doesn't persist; offset write survives
  a unit power-cycle; re-home parks at blank; identify blinks the right
  unit; set-address moves a unit to a new bus address and back
  (clear-address → DIP), UI reporting real outcomes; /unit/reboot drops
  the unit into twiboot ~1 s and it rejoins on its own (no probe issued
  meanwhile); reset-units runs the blank-out sequence and returns to the
  message; Stop interrupts a long show promptly (abort path) and homes all
  five; clock repaints on the next minute.

## Out of scope

Slice C (#205 twiboot reflash + its bus guard), MQTT surfacing of
calibration/provisioning, soft sketch reboot (`SFP_CMD_REBOOT`, no v1
endpoint), multi-operator result tracking, any unit firmware change (the
wire contract is fixed).
