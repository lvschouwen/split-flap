# v2 operator HTTP API — design

> **Status: SHELVED (owner decision 2026-08-06).** Designed 2026-07-27, deferred
> before implementation: the rewrite was a means to the ground-up Web UI rebuild,
> which is not currently wanted, and the existing 52-route surface has proven to
> accrete cheaply (the 2026-08-05 forensics arc added six capabilities without
> friction). Committed as a design record. The two durable findings survive
> independently of the rewrite: readable keys are only affordable COLUMNAR
> (measured, §5), and a proxied op must be keyed on (member, bootId, remoteSeq)
> because the follower's seq counter resets every boot (§6). Revisit if a UI
> rebuild returns or payload pressure materializes.

Ground-up redesign of the operator-facing HTTP surface on the v2 Master, served
under `/api/v2/`. The firmware machine underneath is unchanged: this is a
surface redesign, not a rewrite of the display, cluster, or unit-bus stacks.

The web UI is specified separately. This document settles the endpoint shape
first, because the endpoint shape is what dictates the UI's information
architecture.

## 1. Scope

**In scope** — the operator-facing surface: display control, unit health and
diagnostics, unit maintenance, system vitals, cluster configuration and status,
logs and crash forensics, firmware upload, settings, WiFi.

**Out of scope, frozen** — the leader↔follower machine wire
(`/cluster/{join,render,ping,leave}`, HMAC wire-auth, source-IP binding,
epoch/seq armor, commitAt flip-sync, rollout streaming). It works, it is
security-critical, and it spans two firmware trees plus a commissioned wall.
Nothing in this design touches it.

**Also out of scope** — `/wifi-setup`. It is a captive-portal page served
before the device has joined a network, not part of the operator API.

## 2. Locked decisions

| Decision | Choice |
|---|---|
| Surface in scope | Operator only; machine wire frozen |
| API shape | Screen-shaped; the browser UI is the primary client |
| Primary screen | Compose text + wall mirror + one health indicator that expands on fault |
| Whole-wall state | Aggregated server-side by the leader; the browser never contacts a member |
| Remote-row maintenance | The leader proxies ops to members, server-to-server |
| Rollout | Build in parallel under `/api/v2/`, delete the old surface in a final commit |
| Authentication | None. LAN trust; CORS/CSRF origin enforcement retained |
| Feature scope | Full parity — calibration wizard, cluster member editor + mDNS scan, in-browser logs + crash forensics, onboarding wizard |
| Prefix | `/api/v2/…`, permanent. Old `GET /api` left intact until deletion day |

Consequence of screen-shaping: the UI's information architecture is an *input*
to this design, not an output of it. "API first" holds for delivery order, not
for derivation order.

Consequence of server-side aggregation: the ESP-01's operator surface can
shrink rather than grow, since no browser will contact it directly.

## 3. Route surface

Four families, each with one rule. 16 routes replace 52.

```
INDEX
  GET  /api/v2                       route + action catalog

VIEWS — screen-shaped JSON, one per screen, composed by the leader
  GET  /api/v2/view/home
  GET  /api/v2/view/units
  GET  /api/v2/view/system
  GET  /api/v2/view/cluster
  GET  /api/v2/view/setup

STREAM
  GET  /api/v2/stream                SSE, named events

ACTIONS — every mutation through one dispatcher
  POST /api/v2/action/<name>         → {seq} + 202, or an inline result
  GET  /api/v2/action/<seq>          → 200 terminal | 202 pending | 404 unknown

DOCUMENTS — bulk or non-JSON reads
  GET  /api/v2/logs/ram
  GET  /api/v2/logs/flash?prev=0|1
  GET  /api/v2/logs/coredump
  GET  /api/v2/tz.json

UPLOADS — multipart, streamed to flash
  POST /api/v2/firmware/master?md5=
  POST /api/v2/firmware/follower?md5=
  POST /api/v2/firmware/rescue
```

Documents and uploads are separate families because they genuinely cannot be
views or actions: logs are text and can reach 1 MB, and uploads stream to flash
in `onUpload` chunks before the body is complete.

The three upload paths are one conceptual family with **three different safety
models** — master OTA writes inside the async upload callback, the follower
image accumulates and is committed by netTask, and the relay streams from
clusterTask. URL cleanup is in scope; normalising their implementations is not.

### Actions

28 names replace 30-odd mutating routes:

`set-text`, `set-mode`, `stop`, `reboot`, `settings-set`, `wifi-scan`,
`wifi-config`, `wifi-reset`, `unit-home`, `unit-jog`, `unit-offset-set`,
`unit-identify`, `unit-self-test`, `unit-reset-odometer`, `unit-gates`,
`unit-reboot`, `unit-set-address`, `unit-clear-address`, `units-home-all`,
`units-reflash`, `units-refresh-health`, `cluster-config`, `cluster-discover`,
`cluster-promote`, `mqtt-discover`, `log-clear`, `rescue-install`,
`rescue-boot`.

The dispatcher is a **registry, not a kitchen sink**. Each action declares:
name, argument schema, sync or async, target scope (local / row / any),
conflict class, and deadline. Collapsing routes must not collapse the safety
gates that differ between a settings write, a display command, a proxied
remote job, and an OTA stream.

### Versioning

No version beyond the `/api/v2` prefix. The fleet converges on the baked git
REV; version strings have no operational role. Every payload carries `rev`.

## 4. View contract

### Envelope

```json
{ "rev": "eaba380", "at": 1753641600, "view": "units", "data": { } }
```

`at` is device epoch seconds; `0` means NTP has never synced, which the UI must
distinguish from a clock that is merely late.

No separate per-view schema version. The UI ships inside the same binary as the
API — `index.html` and `script.js` are PROGMEM-baked from `data/` — so they
cannot drift from the endpoints serving them. The only real drift is a stale
browser cache after an OTA, and `rev` already detects it: the UI compares the
rev it was served with against the rev in each payload and reloads on mismatch.

### Readable field names, columnar rows

Terse keys and the ~65-entry legend are both removed. `odo` becomes
`revolutions`, `sx` becomes `stepExcessWorst`, `pmm` becomes
`protocolMismatch`. The field name is the documentation.

Measured on the live leader, `/units/health` is 5,247 B for 16 units across 36
fields, and **key bytes are 60.2% of the payload** because every field name
repeats once per unit. Naive readable keys are therefore not affordable:

| encoding | 21 units | vs the 8,192 B cap |
|---|---:|---|
| terse keys, objects (shipping today) | 6,886 | 84% of cap |
| readable keys, objects | 14,604 | ~2× over |
| **readable keys, columnar** | **3,279** | **52% smaller than today** |

Columnar encoding emits field names once and rows as value arrays:

```json
{ "rev": "eaba380", "at": 1753641600, "view": "units",
  "data": {
    "fields": ["address","alive","state","firmwareRev","uptime","revolutions",
               "stepExcessWorst","driftEvents","vccMin","protocolMismatch"],
    "rows": [
      { "row": 1, "host": "", "plat": "esp32", "self": true, "contactAge": 0,
        "units": [[1,true,"sketch","d6e8a8a",86400,1257,17,0,4720,false]] },
      { "row": 0, "host": "192.168.15.121", "plat": "esp01", "self": false,
        "contactAge": 1840,
        "units": [[1,true,"sketch","d6e8a8a",3600,88,21,0,4810,null]] }
    ] } }
```

`fields` is emitted once per document, from the same table that serializes
every row, so positional drift is structurally impossible. A host test asserts
every row's width equals `len(fields)`.

This also makes a currently-invisible gap visible: the ESP-01 row omits `pv`
and `pmm` from its health payload today, so those units silently differ.
Columnar forces an explicit `null` — an absent capability becomes legible
instead of missing.

**Rule:** columnar when a repeated array is homogeneous *and* its length scales
with the wall. Objects everywhere else: `home`, `setup`, and `cluster` members
(few, and genuinely heterogeneous between an S3 and an ESP-01).

This generalises a choice the project has already made. `/system/stats` serves
its history ring as five named series of 120 samples each — 2,326 B for 600
data points — rather than 120 objects repeating five field names. The same
reasoning applies with more force to `units`, where the repetition factor is 21
rather than 1. The history ring keeps its existing series-per-key shape
unchanged.

### Freshness is mandatory

Leader-side aggregation means remote-row data is only as fresh as the last
successful ping. Every row object carries `contactAge` in milliseconds, and the
UI must visibly age or grey a row past a threshold. **A stale row cannot report
`ok` in the `home` rollup.** Without this, a dead row renders identically to a
live one, which is precisely the failure the existing `degraded` / `suspect` /
`renderStuck` tiers exist to prevent.

### Views

| View | Carries | Rough size |
|---|---|---|
| `home` | display text/mode/width/busy, wall rows with `contactAge`, health rollup, cluster leading/degraded, device name/uptime/ntp | 600–800 B |
| `units` | per-row unit diagnostics, columnar, `contactAge` + `plat` per row | ~3.3 KB |
| `system` | vitals now, history ring (existing series-per-key shape), partitions, OTA state, I2C counters | ~3 KB |
| `cluster` | member table, rollout state, discovery results, stored follower image | ~2 KB |
| `setup` | device, wifi, mqtt, tz, unit-count override, role | ~1 KB |

### Overflow is loud

Each view has an explicit cap; the serializer returns the would-be length like
`snprintf`. Over cap is HTTP 500 with `payload-overflow`. Never a silently
truncated body, never 200 with partial data.

`GET /api/v2` carries strictly more than the current index, which already
measures 7,639 B against an 8,192 B cap. It uses a chunked response from the
start rather than a bumped constant.

## 5. The operation ledger

The only genuinely new machinery in this design, and the highest-risk part.
The browser is promised that the leader is the only board it talks to; that
promise fails hardest when a remote maintenance op is accepted and its result
is lost, overwritten, or confused with another op.

### Why the existing contracts are insufficient

The leader holds one monotonic maintenance seq and one last-result slot
(`DisplayIpc.h`, explicitly not a log). The follower mirrors it with **its own
independent counter**, one staged op slot, and one result slot
(`FollowerWeb.cpp`, `FollowerOps.h`). Two consequences:

1. Leader seq `7` and follower seq `7` are unrelated. A remote seq can never be
   the correlation key exposed to a browser.
2. `maintSeqCounter` is a static initialised to `0` and pre-incremented, so
   **seq values repeat after a member reboot.** A leader holding remote seq 7
   across a follower reboot can be handed a completed result belonging to a
   different operation. That is not a timeout — it is a confidently wrong
   answer reported to the operator, which is worse than an error.

### Structure

A leader-owned table, keyed by leader seq:

| Field | Purpose |
|---|---|
| `seq` | leader-owned, monotonic; the only handle the browser receives |
| `action`, `args` | what was requested |
| `target` | `local`, or `{row, address}` |
| `member`, `bootId` | who executes it, and **which boot of them** |
| `remoteSeq` | filled when the member accepts; never exposed |
| `state` | `staged → sent → running → done \| failed \| timeout \| member-rebooted \| rejected` |
| `deadline` | per action class |
| `result` | terminal, **retained** |

Retention is the point. Both existing slots hold only the most recent result
and overwrite it; a ledger entry survives the remote slot being clobbered by
the next op.

### Rules

1. The async handler **only stages** — allocate seq, write the entry, return
   `{seq}` with 202. No blocking and no HTTP from handler context; `clusterTask`
   remains the sole outbound `esp_http_client` caller.
2. One op in flight per member. The follower accepts a single staged op and
   503s while busy, so the leader must not enqueue unbounded proxy work.
3. **Boot identity comes free from the wire.** Every follower join/ping reply
   already carries `up` (uptime seconds) as part of the additive `plat`/`heap`/
   `rssi` block. The leader captures it at issue time as `bootId`; when a
   member's `up` goes backwards, every non-terminal entry for that member
   becomes `member-rebooted`. This is the rule that prevents a wrong answer.
4. `GET /api/v2/action/<seq>` returns 200 terminal, 202 pending, or 404
   unknown. A leader reboot clears the ledger, so 404 must be distinguishable
   from pending — otherwise the UI waits forever.
5. Optional client-supplied `opId` gives idempotency: a retry with the same
   `opId` returns the existing seq rather than minting a second physical
   address burn.
6. Deadlines are per action class — jog ~5 s, home ~30 s, self-test ~60 s,
   reflash minutes.

### Conflict classes

Rejected at stage time with 409 and a named reason, derived from gates that
already exist: `reflash-in-progress` (the producer gate), `ota-in-flight`,
`rollout-active` (relay and rollout share a buffer and are mutually exclusive),
`member-busy`, `queue-full` (the display command queue is depth 16).

### The ESP-01 reflash asymmetry

The master's `/reflash-units` honours `?address=N`; the follower's ignores it
and reflashes the whole row, two at a time. A per-unit reflash aimed at an
ESP-01 row is therefore **400 `row-wide-only`, never a silent escalation**.
Quietly reflashing five units when one was requested is the class of surprise
that wrecks a commissioned wall. `commission-units.sh` already refuses this;
the API matches it.

## 6. Stream

`GET /api/v2/stream`, one SSE source, named events: `hello`, `display`,
`health`, `cluster`, `op`. Named events give per-concern filtering natively in
the browser with no server-side subscription state, which matters because
`AsyncEventSource` holds every client on one handler.

**The stream is a change notification, not a state transfer.** Events carry the
minimum needed to decide whether to refetch; full state always comes from a
view. `health` carries the rollup only — counts and worst-row `contactAge`,
never per-unit data. Verbose field names stay out of the hot path.

The `op` event pushes `{seq, state}`, so the calibration wizard does not poll.
`GET /api/v2/action/<seq>` remains the authority for reconnects and late
joiners.

Mechanics follow the existing implementation: 100 ms check cadence,
push-on-change, early-out when no clients are connected. On connect, `hello`
carries `rev` so a browser holding a pre-OTA `script.js` reloads immediately.
Stream down means the UI falls back to polling `view/home` at 5 s.

## 7. Errors

Every error is an HTTP status plus `{"error":{"code","message"}}`, with `code`
drawn from a closed set:

| Status | Codes |
|---|---|
| 400 | `bad-args`, `unknown-action`, `row-wide-only`, `address-out-of-range` |
| 403 | `origin-rejected` |
| 404 | `unknown-seq`, `unknown-unit` |
| 409 | `reflash-in-progress`, `ota-in-flight`, `rollout-active`, `member-busy`, `queue-full` |
| 413 | `body-too-large` |
| 500 | `payload-overflow` |
| 503 | `not-ready` |

Accepted async ops return 202, never 200.

Action bodies live under the existing 2,048 B non-upload ceiling. The largest
realistic body, an 8-member cluster config table, measures ~400 B, so there is
headroom — but it is a stated constraint, not an assumption.

Routing every mutation through one dispatcher moves CORS/CSRF origin
enforcement from ~30 handlers to one. Uploads remain the documented exception:
they gate inline at `index == 0` because they stream to flash before the body
is complete.

## 8. Code layout

```
ApiV2.cpp            registration, shared state, dispatcher core
ApiV2Views.cpp       the five view serializers
ApiV2Actions.cpp     action registry, validation, staging
ApiV2Ledger.h/.cpp   pure state machine + glue
ApiV2Stream.cpp      SSE
ApiV2Internal.h      family-only include
```

Master-only. The follower serves no views, so nothing new lands in `shared/`.
`ApiV2Internal.h` follows the existing family-include discipline: included by
`ApiV2*.cpp` and nothing else.

Every view serializer is a pure function over one mutex-copied `DisplaySnapshot`
and returns a would-be length. The API layer holds no logic — it is a
translation layer, which is the only shape the async/task split permits.

## 9. Testing

Native suites, matching the existing pure-logic culture:

- `test_api_v2_views` — cap overflow returns would-be length; `null` for an
  absent capability; envelope correctness
- `test_api_v2_columnar` — `fields` width equals every row width; round-trip
- `test_api_v2_ledger` — boot-id invalidation, deadline expiry, idempotency,
  404-vs-202, retention past a remote-slot overwrite
- `test_api_v2_actions` — catalog completeness, conflict-class rejection, body
  cap enforcement

Integration, pytest: extend `fake_follower.py` to serve the operator op
endpoints **and simulate a reboot by winding `up` backwards mid-op**. That is
the exact failure the ledger exists to prevent, and it must be proven host-side
before it is trusted on the wall.

Anti-drift gates: the action catalog covers every registered action (replacing
the terse-key legend gate), and `fields` width matches row width. The existing
`test_api` legend gate stays alive until the old surface is deleted.

## 10. Sequencing

1. Ledger + native tests — pure, no routes
2. Views + columnar serializers + tests
3. Action registry, dispatcher, catalog — local targets only
4. Proxy path via clusterTask + the `fake_follower` reboot test
5. Stream
6. New web UI — separate spec
7. Delete the old surface, `script.js`, and the legend gate

Stages 1–3 and 5 bench on the spare S3 (no row attached). Stage 4 needs the
real follower. The old surface stays working throughout, so the wall keeps
running and there is a fallback at every point.

Both surfaces are resident in flash for the duration. The app slot is 4 MB and
the current image is 1.57 MB, so the overlap is affordable.

## 11. Open questions

1. Ledger depth — how many entries retained, and eviction policy. A ring of 8
   or 16 is the likely answer; it needs a number before implementation.
2. Whether `settings-set` stays one coarse action or splits per settings group.
   Coarse is simpler and matches the current form POST; split gives better
   conflict granularity.
3. **The web server stack is inherited, not chosen.** This design assumes
   `esp32async/ESPAsyncWebServer`, and several of its rules follow from that —
   "handlers stage, tasks mutate", the chunked-response requirement, and
   `AsyncEventSource` having nowhere to keep per-client subscription state.
   Switching to IDF `esp_http_server` would relax some of those constraints and
   change Section 8's rationale. The findings that survive either choice are the
   operation ledger and the columnar encoding; the handler-context rules do not.
   Decide the stack before implementing Section 8.
