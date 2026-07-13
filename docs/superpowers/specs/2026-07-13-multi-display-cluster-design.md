# Multi-display cluster — N-row wall of v2 masters (design)

Approved 2026-07-13. Supersedes the #235 sketch; jumps the #226 post-parity gate by
user decision (bench vehicle exists: spare S3 `c8a746` as a dummy 16-unit display,
leader candidate `a47dee`).

## Vision

Scale the display by composing it: each row stays the proven cheap hardware (Nano
units on I2C behind one master), and you add an ESP32-S3 per row for power. Two or
more v2 masters cooperate as ONE logical display — a 2-, 4-, or 6-row split-flap
wall driven from a single web UI / HA integration, updating its own firmware as a
fleet.

## Decisions (brainstorm 2026-07-13)

| Question | Decision |
|---|---|
| Semantics | Flexible layout: leader maps a logical text grid onto members via per-member `{row, col, width}` — rows, wide lines, and mirror are member-table shapes, not modes |
| Topology | Fixed leader (config, not election). Leader's UI/MQTT/clock drive the cluster; followers render segments |
| Transport | HTTP/JSON on the LAN over the existing async web servers + mDNS + NTP. BLE rejected (radio coexistence cost, ~100 KB stack, no benefit while WiFi is mandatory anyway); ESP-NOW rejected (router-down win is hollow — NTP/UI die with the router too); MQTT rejected as bus (broker = new single point of failure, MQTT is optional today) |
| Follower loses leader | Hold last segment ~2 min grace, then LOCAL CLOCK fallback; leader reclaims seamlessly |
| Follower control surfaces while clustered | Producer gate (409) on text/mode/clock + "Clustered — row N of <leader>" banner; maintenance (calibration, unit ops) stays local |
| Launch content modes | Grid text + cluster clock. Per-row independent content and explicit mirror mode deferred (engine supports them) |
| Firmware | Leader's running build is the cluster version — mismatched followers are converged automatically (both directions), sequentially, health-gated |
| Scale | `CLUSTER_MAX_MEMBERS = 8`; design target 4–6 rows, not 2 |

## Architecture

Roles are NVS settings (any v2 master can be standalone / leader / follower).
Nothing new touches display state directly (Hard rule preserved): the cluster layer
is a producer like web/MQTT — on the follower `POST /cluster/render` enqueues a
`DisplayCommand`; on the leader, text/clock producers hand LOGICAL grid text to the
cluster layer, which slices segments, enqueues the leader's own segment locally,
and ships the rest over HTTP. Cluster disabled → byte-identical passthrough to
today's behavior.

```
HA / browser / clock ──▶ leader (a47dee)
                          ├─ ClusterLayout: grid text → N segments
                          ├─ own segment → displayEnqueue → I2C units
                          └─ others → clusterTask → POST /cluster/render on members
                                        └─ follower enqueues DisplayCommand
```

New pieces (v2 Master tree, pure-header + glue pattern):

- **`ClusterLayout.h`** — pure, natively tested. Member table → derived grid
  (rows = max(row)+1, row width = Σ member widths on that row; side-by-side members
  compose wide rows). `layoutGridText()` = word-wrap (break on spaces, hard-split
  over-long words) + per-row alignment + slice into per-member segments.
  `clusterClockSegments()` = row 0 time, row 1 date (v1 formats), rows 2+ blank at
  launch. `validateMemberTable()` rejects overlaps/gaps/missing rows at config time
  and is re-checked before every fan-out (MaintenancePolicy.h philosophy).
- **`ClusterFollowerPolicy.h`** — pure, natively tested. State machine
  `Standalone → Clustered → Grace → LocalFallback (→ reclaimed)`, driven by
  leader-contact timestamps; epoch/seq acceptance rules.
- **`ClusterLeader.h` + glue** — member table (NVS), per-member health/degraded
  state, render fan-out bookkeeping, rollout sequencing policy (pure where possible).
- **clusterTask** — new FreeRTOS task, core 0 (network domain). Owns ALL outbound
  HTTP (`esp_http_client`, short LAN timeouts) so a dead follower can never stall
  netTask's SSE/WiFi/LED ticks. Producers hand it work via queue; state reads back
  as a mutex-copied snapshot (same contract style as Tasks.h).

Member width is a join-handshake fact (16 today). When #234 lands probe-derived
widths, the handshake simply reports real numbers — no protocol change.

## Wire protocol

JSON on the existing async servers. Follower endpoints:

- `POST /cluster/join` (leader→follower): `{leaderName, leaderHost, row, epoch}` —
  persists `clusteredBy` in NVS (banner + gate survive reboot); reply carries
  identity, firmware rev, width.
- `POST /cluster/render`: `{epoch, seq, text, speed, commitAtMs}` — the member's
  pre-baked segment ONLY (follower never re-wraps). `commitAtMs` = UTC epoch-ms ≈
  leader-now + 400 ms; both ends are NTP-synced so all rows start flipping within
  tens of ms (synchronized ripple). Follower clock unsynced → render immediately.
- `POST /cluster/ping` (~10 s when idle): feeds the grace timer; any leader contact
  counts.
- `GET /cluster/health`: follower state, current segment, unit-health rollup, rev.
- `POST /cluster/leave`: revert to standalone.

**Staleness armor:** `epoch` = random ID minted at leader boot; `seq` monotonic
within epoch. Follower rejects `seq <= lastSeq` in the same epoch (delayed retries
can't regress the wall), accepts any NEW epoch (leader rebooted). Renders are
idempotent; every join handshake ends with a re-send of the member's current
segment.

**Auth:** LAN-trust, same as the whole v2 API today (`/firmware/master` is open —
a cluster secret would be theater). Revisit if the API grows auth.

**Versioning:** protocol version in the join handshake; `render` is the frozen,
boring part — it keeps working across fleet rollouts.

## Failure handling

- **Follower loses leader:** grace ~2 min (any contact feeds it) → LocalFallback
  local clock; on renewed contact, reclaimed + segment re-sent. Rebooting follower
  comes up gated (NVS `clusteredBy`) in Grace — never flashes stale standalone
  content.
- **Leader loses follower:** 3 failed POSTs with backoff → member `degraded`
  (cluster card + HA). No re-layout: remaining rows keep rendering; the orphaned
  board shows its own clock rather than a hole. Recovery idempotent via re-join.
- **Leader reboot/OTA:** new epoch; re-join + re-render fan-out on boot.

## Fleet firmware convergence

Leader's running build = the cluster version, enforced in BOTH directions (a
deliberately newer follower is converged back — uncluster first to bench-test a
follower build; documented loudly).

- Follower rev arrives in join + health.
- On mismatch, clusterTask streams the leader's OWN running slot
  (`esp_ota_get_running_partition` + esp_image metadata for exact image length,
  MD5 computed while streaming) to the follower's EXISTING
  `POST /firmware/master?md5=` endpoint — zero new follower code, same A/B
  rollback net underneath.
- Strictly sequential: next member starts only after the previous reports the new
  rev on `/cluster/health`. A bad image strands at most one board, and native
  rollback un-strands it.
- Renders keep flowing during rollouts. Upload firmware to the leader; the wall
  updates itself.

## UI + HA

- **Leader "Cluster" card:** masters advertise `_splitflap._tcp` (TXT: identity,
  width, rev); browse → discovered boards → assign row/col → join. Live member
  list (ok / degraded / updating / rev), remove/leave controls. The SSE wall
  mirror extends to render ALL grid rows (leader knows every segment).
- **Follower UI:** banner "Clustered — row N of <leader> → open", text/mode cards
  gated 409, maintenance live.
- **HA/MQTT (leader):** text capacity = grid size, per-member availability
  attributes, cluster-degraded binary sensor. Followers publish availability only
  while clustered. Wire contract otherwise unchanged from #224.

## Testing

- Native: `ClusterLayout` (wrap/align/slice/validate, N-row), `ClusterFollowerPolicy`
  (state machine, epoch/seq), rollout sequencing policy.
- pytest: endpoint JSON contracts; **fake-follower harness** — a tiny Python HTTP
  server impersonating N followers so 4–6-row layouts, degraded members, and
  rollout sequencing run without hardware.
- Bench: a47dee (leader, 16 physical units) + c8a746 (dummy follower, no units —
  probe 0/16 assumes width 16, SSE mirror shows its row). Drills: leader kill,
  follower kill, leader OTA (fleet rollout), WiFi blip, reboot-while-clustered.

## Out of scope (decided)

- BLE / ESP-NOW / MQTT transports (rejected above; Improv-BLE provisioning #232 is
  unrelated and unaffected).
- Leader election / any-node entry (fixed leader only; revisit if ever needed).
- Per-row independent content + explicit mirror mode (engine-ready, deferred).
- Cluster auth (tracks whole-API auth, not cluster-specific).
- v1 ESP8266 participation (v2-only feature).

## Relationship to existing issues

- Supersedes **#235** (close as superseded; its design questions are answered here).
- Independent of **#234** (probe-derived widths later refine the handshake numbers).
- Jumps the **#226** gate deliberately (user decision 2026-07-13).
