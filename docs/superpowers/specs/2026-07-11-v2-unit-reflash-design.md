# v2 unit reflash over twiboot — design (slice C of the I2C port)

Approved 2026-07-11 (brainstorm with Lucas). Epic #183 / meta #58 / issue
#205. Builds on slices A (#203, UnitBus + probe/health) and B (#204,
queue-native maintenance ops, `MaintResult` slot, twiboot probe-inhibit
guard). Completes the I2C port: after this slice the S3 master can fully
maintain a display without the ESP-01.

Operational constraint honored throughout: units are NEVER USB/ICSP-flashed
in operation — a failed or cancelled flash must always be recoverable over
the bus (boot auto-install) or by DIP + power cycle.

## Goal

The S3 master carries the bundled unit firmware and can push it to units,
exactly like v1: boot-time auto-install (units found in twiboot), boot-time
auto-update (units on a stale rev), and web-triggered `POST /reflash-units`
— plus what v1 never had: live per-unit progress in the Maintenance tab and
a working cancel. Unit health starts reporting real `fw` status (the
comparison rev finally exists in the v2 build).

## Decisions (with rationale)

- **Inline blocking job in displayTask** (chosen over a dedicated FreeRTOS
  job task and over a self-re-enqueueing chunked job): `ReflashUnits` is a
  normal `DisplayCommand`; displayTask runs the whole multi-minute job
  inline. Bus exclusivity stays structural (UnitBus keeps exactly one
  caller — no new locking), and the queue/snapshot/abort machinery is all
  reused. A dedicated task would need bus-ownership handoff for zero
  user-visible benefit; a chunked job would interleave foreign I2C into the
  settle windows that exist to bound homing current (v1 #138) and need
  resumable-job state for a display that looks broken mid-job anyway.
- **Producers are gated, not queued** (Lucas): while the job runs, nothing
  piles up behind it to burst-drain afterwards.
  - `reflashActive` flag in the `DisplaySnapshot`, set when the job starts
    executing, cleared at the end.
  - clockTask checks its snapshot copy and skips the whole tick while
    active — nothing marked queued, so the first tick after the job
    re-sends fresh time naturally.
  - Web and MQTT display-mutating producers (message/text, every #204
    maintenance op EXCEPT `Stop`, `/reset-units`, `/unit/reboot`) check
    the snapshot at the boundary and answer **409 "reflash in progress"**
    instead of enqueueing. The UI disables those controls while progress
    is active. `Stop` is the one deliberate exception — it is the cancel.
  - Read-only surface is untouched: the whole UI, health/progress polling,
    logs, settings pages keep working (netTask/core 0; the snapshot is
    mutex-copied as always).
  - Settings saves don't touch the display queue and keep working.
  - Race window: commands already sitting in the queue when the job starts
    are drained and dropped with a log line — except `Stop`, which is never
    dropped. (The queue is near-empty in practice; the drain closes the gap
    completely.)
- **`/stop` doubles as cancel** (chosen over a dedicated cancel endpoint
  and over not-cancellable): the existing atomic abort flag
  (`unitBusRequestAbort`, #204 order rule: set BEFORE enqueue, rolled back
  on 503) is polled between page writes and between units. On abort the
  in-flight unit stays in twiboot — v1's proven failure story; boot
  auto-install or a retry recovers it. The queued `Stop` command survives
  the start-drain and executes right after the job unwinds: broadcast home
  + retained-text clear, unchanged.
- **Master OTA is gated too**: `POST /firmware/master` answers 409 while
  `reflashActive` — rebooting the S3 mid-flash strands the in-flight unit
  in twiboot for no reason. (Recoverable, but pointless; also a step
  toward #191.)
- **Progress = per-unit counters in the snapshot** (chosen over per-page
  updates and over v1's fire-and-forget): `ReflashProgress {state, total,
  done, failed, currentAddr}` rides the `DisplaySnapshot`; displayTask
  publishes at unit boundaries and settle transitions (~every 3–5 s, a
  handful of publishes per job). Per-page would be ~250 publishes/unit of
  mutex churn for a 3-second-per-unit operation. The Maintenance tab
  already polls `/units/health`; the progress object joins that JSON — no
  new endpoint.
- **Job result via the #204 contract**: `POST /reflash-units` validates
  (409 if a job is active or queued), enqueues with a seq, answers
  `{"seq":N}`; `GET /unit/op-result?seq=N` grades the whole job (ok = every
  planned unit flashed; failed carries the failure count via the progress
  object). Queue full → 503, abort-flag rollback semantics identical to
  #204.
- **Baked re-show at job end** (same pattern as `makeResetUnitsCommand`):
  the command bakes text/alignment/speed at enqueue; after the final
  reprobe the job re-shows it, so reflashed units (which homed to blank)
  come back with content immediately. Clock mode: the baked text may be up
  to one minute stale; the next clock tick corrects it. Boot-time paths
  have nothing baked and skip the re-show — the clock/event flow populates
  the display as usual.
- **Full v1 parity on entry points** (chosen over web-only): boot
  auto-install doubles as the recovery path for any unit left in twiboot
  by a failed/cancelled flash — without it such a unit stays a gap until a
  manual click. All three paths share one orchestration function.
- **The job's internal probes bypass `settleBeforeProbe()`** — deliberate
  and documented exemption from the #88 guard: the hazard is probing
  twiboot *without* flashing (pins it alive forever); here every pinned
  unit is immediately flashed and exited properly. The guard keeps
  protecting every probe outside the job, unchanged.
- **Hex + rev sidecar are committed** (v1 pattern verbatim):
  `firmware/v2/Master/data/unit-firmware.hex` + `.rev`, embedded by
  `build_assets.py` at build time. `make_manifest.py stage` stages the
  freshly built Unit hex into BOTH `v1/ESPMaster/data/` and
  `v2/Master/data/`; the consistency + freshness gates extend to both
  trees so the two bundles cannot drift. CI never runs stage — it builds
  from the committed hex (unchanged model).

## Orchestration (one function, three callers)

1. **Plan** (pure, `ReflashPlan.h`): from the current `UnitFacts` — every
   sketch-mode unit whose rev mismatches `BUNDLED_UNIT_REV` gets
   `CMD_ENTER_BOOTLOADER`; every unit already in twiboot (state 2) joins
   the flash list directly. Units already on the bundled rev are skipped
   (v1 #114).
2. **Settle + rescan**: 500 ms twiboot startup wait
   (`TWIBOOT_STARTUP_MS`), then an internal `unitBusProbe` (exempt from
   the inhibit, see above) to confirm who is actually in twiboot.
3. **Flash, batched**: per unit — chipinfo verify (ATmega328P signature +
   page size 128) → stream `UNIT_FIRMWARE_BIN` one 128-byte page at a
   time, write-then-readback verify with ONE rewrite retry (v1 #110) →
   `CMD_SWITCH_APPLICATION` exit → post-boot ACK check → `CMD_REBOOT` for
   a clean watchdog restart (v1 #113). Batches of `REFLASH_BATCH_SIZE = 2`
   with the 15 s online-and-homed settle wait between batches (v1 #138
   brownout throttle). Abort flag polled between pages and between units.
   Per-unit failure: leave the unit in twiboot, count it, continue with
   the rest (v1 best-effort semantics).
4. **Finish**: final `unitBusProbe` + `unitBusPollHealth` (wholesale facts
   rewrite — fw status refreshes for real, no v1-style cache patching),
   baked re-show (web path only), grade `MaintResult`, clear
   `reflashActive`, publish.

Boot flow ordering: existing 1500 ms pre-probe delay → probe → health →
publish → **auto-install** (state-2 units) → **auto-update** (outdated
revs → enter-bootloader sweep → same orchestration). Both run before the
command loop starts. The boot jobs set `reflashActive` in the published
snapshot exactly like the web job: WiFi join runs in parallel and the web
server (and clockTask) can come up mid-install, so the producer gate must
already be closed — otherwise boot-time flashing recreates the queue-up
this design forbids.

## Components

- `ReflashPlan.h` (NEW, pure, native-tested): plan derivation from
  `UnitFacts` + batch grouping + progress-state transitions.
- `DisplayIpc.h`: `ReflashProgress` struct + `reflashActive` flag in the
  snapshot; progress JSON rendering; enqueue-gate predicate
  (`displayAcceptsCommands(snapshot)` — pure, native-tested).
- `DisplayCommand.h`: `ReflashUnits` opcode + maker (bakes text like
  ResetUnits).
- `UnitBus.cpp/.h` (bench tier): twiboot client verbatim from v1
  (`twibootPing/Exit/VerifyChip/ReadFlashPage/WriteFlashPage/WaitReady`),
  wrapped as `unitBusFlashUnit(addr, image, len)` with abort polling.
  Twiboot listens on the unit's own DIP/EEPROM address (patched
  bootloader), same as v1.
- `Tasks.cpp`: the orchestration function + `ReflashUnits` case + boot
  auto-install/auto-update calls + queue drain-at-start.
- `WebEndpoints.cpp`: `/reflash-units` real handler (replaces 501 stub);
  409 gates on the mutating endpoints + `/firmware/master`; progress in
  the health JSON.
- `data/script.js` + Maintenance tab: progress line ("flashing 0x05 —
  4/12, 0 failed"), controls disabled while active, Stop stays live.
- `build_assets.py`: unit-firmware emit restored from v1 (intel-hex parse,
  page pad, `UNIT_FIRMWARE_BIN`, `BUNDLED_UNIT_REV`).
- `flashing/flasher/make_manifest.py`: dual-tree stage + gates.
- `UnitHealth.h`: fw-status derivation gains the real bundled rev to
  compare against (mechanism exists from slice A; the constant was the
  missing piece).

## Testing

- Native (`pio test -e native`, v2 Master): `ReflashPlan.h` (who reboots,
  who flashes, skip-on-bundled-rev, batch grouping), progress state
  machine + JSON, enqueue-gate predicate incl. Stop passthrough, op-result
  grading for the job.
- Python (`python -m pytest`): `build_assets.py` unit-hex emit (parse,
  pad, rev sidecar), `make_manifest.py` dual-stage consistency/freshness
  gates.
- Bench (real 5-unit display, ESP-01 unplugged): full web reflash with
  progress watched, cancel mid-job then boot-recovery of the stranded
  unit, boot auto-install, batch settle observed, fw status shows 0/1
  instead of 2.

## Out of scope

- Uploading arbitrary unit hex over the web (v1 dropped it in #23; the
  PROGMEM bundle is the only source).
- Full concurrent-upload hardening (#191) — only the reflash-vs-master-OTA
  409 lands here.
- Slice-C-adjacent flasher.exe unit-flash bug (#189) — unrelated tooling.
