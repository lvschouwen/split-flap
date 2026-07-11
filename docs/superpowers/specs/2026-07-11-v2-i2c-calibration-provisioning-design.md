# v2 calibration + provisioning over I2C — design (slice B of the I2C port)

Approved 2026-07-11 (brainstorm with Lucas). Epic #183 / meta #58 / issue #204.
Builds on slice A (#203, `2026-07-10-v2-i2c-unit-bus-design.md`): reuses its
UnitBus module, command queue, and snapshot model. Slice C (#205, twiboot
reflash) stays separate.

## Goal

The Maintenance tab comes alive on v2: interactive calibration (offset
read/write, jog, home, reboot) and provisioning (#56: set/clear EEPROM I2C
address, identify) against unchanged v1 units, plus the display-wide
`/reset-units` and `/stop`. The already-ported v1 web UI keeps working with
a near-zero diff.

## Decisions (with rationale)

- **Queue-native port** (chosen over a sync mailbox and over full async
  result-polling): every op is a `DisplayCommand`; web handlers validate,
  enqueue, and return immediately — never wait on display work. FIFO order
  preserves the UI's chained flows (read offset → write → home) without any
  new synchronization. No blocking of the async_tcp task; no new UI
  machinery for a bench-interactive feature where the operator watches the
  flap move.
- **Offset becomes a snapshot fact** instead of an on-demand bus read:
  probe reads `CMD_GET_OFFSET` for every sketch-mode unit; displayTask
  updates the fact in place after a successful `WriteOffset`. Nothing else
  can change a unit's stored offset (jog moves the drum, not the offset),
  so the fact stays truthful between probes. `GET /unit/offset` answers
  synchronously from the snapshot copy — the v1 wire contract
  (`{"offset":N}`) survives unchanged.
- **POST responses report queueing, not wire status** (deviation from v1):
  a wire NACK on a queued op lands in the serial log and as un-mutated
  facts on the UI's existing post-mutation refresh (stale offset, unit
  missing from probe). Accepted because validation catches everything
  user-fixable *before* queueing, and calibration is physically observed.
- **`/stop` has no mid-frame abort** (deviation from v1): Stop queues
  behind an executing frame (normally seconds; up to the 30 s stuck-unit
  cap). v1's `abortCurrentShow` was exactly the volatile cross-context
  staging #203 dissolved; an abort channel can come back as its own slice
  if bench use demands it.
- **v1's `i2cBusBusy` / flash-window 503 guards dissolve** — the command
  queue is the serialization. Slice C re-introduces whatever guard the
  long-running reflash job needs, on its own terms.

## Components

- `DisplayCommand.h`: opcodes `WriteOffset`, `Jog`, `Home`, `RebootUnit`
  (soft reset, stays in sketch), `RebootToBootloader`, `Identify`,
  `SetAddress`, `ClearAddress`, `ResetUnits`, `Stop`. Two new POD params:
  `uint8_t unitAddress`, `int16_t value` (offset steps / jog steps / new
  address — the opcode disambiguates). Maker helpers per opcode.
- `MaintenancePolicy.h` (new, pure, natively tested): all web-boundary
  validation against the caller's snapshot copy — address parse + 1..126
  range, sketch-running-unit check (404), offset within
  ±`SFP_OFFSET_LIMIT_STEPS` (400), jog −127..127 (400), set-address bound
  to `UNITS_AMOUNT` (400, master can't manage beyond it) and
  target-occupied (409, burning the current address always allowed).
  Returns a small verdict struct (HTTP status + message id) so the web
  layer stays a thin translator.
- `UnitBus.cpp/.h`: thin v1-port wrappers over the existing transaction
  core — `unitBusReadOffset`, `unitBusWriteOffset`, `unitBusJog`,
  `unitBusHome`, `unitBusRebootUnit`, `unitBusRebootToBootloader`,
  `unitBusIdentify`, `unitBusSetAddress`, `unitBusClearAddress`,
  `unitBusBroadcastHome` (general-call 0x00 + `CMD_HOME`). Same 2 ms
  settle, same drain-on-short-read. Probe additionally reads
  `CMD_GET_OFFSET` per sketch unit.
- `UnitHealth.h` `UnitFacts`: `int16_t offset` + `bool offsetValid`
  (false for silent/bootloader/pre-#32 units).
- `DisplayIpc.h` `displayApplyCommand`: transitions for the new opcodes —
  count the command; `WriteOffset` updates the unit's offset fact on
  success; `Stop` clears `currentText` (the clock re-sends on its next
  tick, v1 parity); `ResetUnits` leaves `currentText` untouched (the
  re-show is part of executing it).
- `Tasks.cpp` displayTask: executes the new opcodes via UnitBus.
  `ResetUnits` runs entirely in-task: full `'-'` row frame → 2 s → `'.'`
  row frame → re-show `snapshot.currentText` (v1's blank-out sequence that
  forces the wrap-around recalibration). `Stop` = broadcast home + text
  clear.
- `WebEndpoints.cpp`: un-501 `GET/POST /unit/offset`, `POST /unit/jog`,
  `/unit/home`, `/unit/reboot`, `/unit/identify`, `/unit/set-address`,
  `/unit/clear-address`, `/reset-units`, `/stop`. Handlers: snapshot copy →
  MaintenancePolicy verdict → enqueue → v1-style response body (wording
  says "queued" where honest). `GET /unit/offset`: 502 when `offsetValid`
  is false (v1's "firmware may predate #32" case).
- `data/` UI: already the full v1 Maintenance tab; expected diff is status
  wording at most. Verify the EEPROM==DIP twiboot warning survived the
  copy (over-I2C reflash only works while EEPROM and DIP agree).

## Data flow (mutation example: set-address)

UI POST → handler copies snapshot → MaintenancePolicy validates (range,
sketch-running source, target unoccupied) → enqueue `SetAddress` → 200.
displayTask: `unitBusSetAddress` → unit burns EEPROM + reboots off the bus.
UI (existing behavior) triggers `/units/health/refresh` after a beat →
Probe re-scans, unit reappears at its new address, offset re-read.

## Error handling

- Validation failure → 400/404/409 before anything queues (v1 parity).
- Wire NACK during execution → serial log, no fact mutation; UI sees truth
  on its next refresh. Snapshot always published.
- Queue full → same handling as slice A's senders (log + drop; UI retry).

## Testing

- Native: MaintenancePolicy validation matrix (range × state × occupied ×
  limits); `displayApplyCommand` for every new opcode (incl. Stop text
  clear, WriteOffset fact update, rejection of malformed commands); maker
  helpers clamp/bake correctly. Existing suites stay green.
- Bench (E2E tier, combined session with #203's checklist, 5-unit display):
  jog moves one flap and doesn't persist; offset write survives a unit
  power-cycle; re-home parks at blank; identify blinks the right unit;
  set-address moves a unit to a new bus address and back (clear-address →
  DIP); reboot-unit drops and rejoins; reset-units runs the blank-out
  sequence and returns to the message; Stop homes all five and the clock
  repaints on the next minute.

## Out of scope

Slice C (#205 twiboot reflash + its bus guard), MQTT surfacing of
calibration/provisioning, mid-frame abort for Stop, any unit firmware
change (the wire contract is fixed).
