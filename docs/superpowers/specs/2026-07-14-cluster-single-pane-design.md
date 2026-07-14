# Cluster single pane of glass — full-wall health, digest mirror, per-member management (#294) + leader failover (#295)

**Date:** 2026-07-14 · **Epic:** #270 · **Depends on:** PR #279 merged (e146b3a), #276/#292 fleet convergence working

## Goal

A clustered wall should be manageable and observable from ONE place — and, cooler, from *any* of its own panes. Today (#274/#277) the leader shows member pills, rollout progress, and the wall mirror, but unit-level health renders only for the leader's own row, follower UIs show nothing of the wall, and per-member maintenance requires opening each follower's page by hand.

## Topology decision (research-backed)

Web sweep of comparable systems (Sonos group coordinator, WLED UDP sync, AWTRIX/HA orchestration, painlessMesh, PicoMQTT on-device broker, SWIM/Raft literature):

- **Stay single-active-master, statically configured, hub-and-spoke.** At n≤6 on home WiFi this is what every comparable system converges on; gossip/consensus solve problems that start at hundreds of nodes, and a 2-board cluster can't form a Raft quorum.
- **No message bus.** An on-device MQTT broker is QoS-0 (we'd re-add epoch/seq armor on top) and makes the broker board a fatter SPOF than today's leader. UDP multicast is loss-tolerant-only (IGMP snooping black-holes it) — wrong for commitAt flip-sync. External MQTT stays observability fan-in (HA), never the control plane.
- **The insight that replaces the bus:** the leader already contacts every follower every tick and the ping POST body is empty. The existing hub carries the bus's payload — health up on the ping reply, a cluster digest down on the ping body. Coordination stays single-master; *visibility* becomes symmetric.

## The four rungs

| Rung | What | Issue / arc |
|---|---|---|
| 1 | Per-row unit health up on the ping reply → full-wall health strips | #294, arc 1 |
| 2 | Cluster digest down on the ping body → every pane shows the whole wall | #294, arc 1 |
| 3 | Browser fan-out + CORS → manage any member from any pane | #294, arc 2 |
| 4 | Leader failover: (a) promote button, (b) automatic takeover later | #295, arc 3 |

All wire changes are additive JSON/form params: old-rev followers simply lack the new keys (leader hides that row's strip) and ignore the digest param. #276 convergence rolls the fleet to the new schema anyway.

## Rung 1 — health up (ping reply)

Follower's `/cluster/ping` reply gains additive keys (source: its own `DisplaySnapshot`, same distillation as `/cluster/health`):

```json
{"state":"Clustered","epoch":…,"seq":…,
 "width":16,"detected":16,"faulty":1,
 "faultMask":"0004","wear":false,"rev":"334460d8"}
```

- `faultMask`: hex string, fixed width `ceil(width/4)` nibbles; bit *i* set = unit at position *i* (leftmost = 0) unhealthy. Enough for a strip; per-unit *detail* is rung 3's job (browser fetches the member's `/units/health` directly — no new detail wire format).
- `wear`: the #231 `assessWear` any-unit-flagged bool.
- The join reply carries the same health keys too (minus the width/rev it already had) — the leader's strip is live from the handshake, not the first ping.
- Leader: `applyMemberResult` (Ping branch) parses into new `ClusterMemberRuntime` fields (`faulty`, `detected`, `faultMask`, `wear`; `rev` refreshes too — today it's join-only). The leader's own row folds straight from its local snapshot.
- Surfacing:
  - `GET /cluster/status` members[] gains the same keys → the Cluster card's existing 5 s poll drives **health strips under every row of the wall mirror** (and wear badges on pills). SSE `/events` stays text-only — strips update at poll cadence, deliberately.
  - HA: `cluster_degraded` json_attributes gain per-member `faulty`/`detected` (pure `ClusterMqtt.h` extension).
- Missing keys (old follower fw) → `faulty: null` in status, strip hidden for that row.

Cadence: ping is the 10 s keep-alive, and renders/joins also refresh contact — health staleness ≤10 s. Acceptable for a wall; the leader's own row stays live.

## Rung 2 — digest down (ping body)

The leader's ping POST body (today empty) gains one form param, `digest=<urlencoded JSON>`, built fresh per fan-out round:

```json
{"gen":N,
 "leader":{"name":"…","host":"192.168.15.90"},
 "table":"host|row|col|width;…",
 "rows":["ROW 0 TEXT","ROW 1 TEXT"],
 "members":[{"host":"…","row":1,"state":"ok","rev":"…",
             "width":16,"faulty":0,"faultMask":"0000","wear":false}]}
```

- `rows` comes from the existing `clusterMirrorRows` (#277); `table` is the NVS member-table wire string verbatim; `gen` increments when any of it changes (drives UI churn gating and lets rung 4 detect table updates).
- Follower stores the **raw JSON string** mutex-copied with a receive timestamp; cleared on leave. The promote-critical bits — the `table` wire string and this member's index (the ping's `you` param) — additionally persist to NVS *when they change* (config edits, not the 10 s cadence), so a takeover stays possible after a follower reboot.
- `GET /cluster/digest` → `{"ageMs":…,"digest":<raw>}`; 404 when none held.
- Follower UI: when clustered and a fresh digest exists, the page renders the **same stacked wall mirror + health strips + member pills** as the leader (read-only, 5 s poll of `/cluster/digest`, wire strings as text nodes only), with the existing banner's leader link for anything write-shaped. Digest older than ~30 s renders greyed ("last seen …").
- Size: ~1–1.5 KB urlencoded at 6 rows — noise on LAN HTTP; built transiently per round (PSRAM-preferred `String`).

## Rung 3 — per-member management (browser fan-out + CORS)

The operator's browser fans out directly to member hosts; the leader firmware never proxies (keeps "clusterTask = sole outbound caller" intact, zero leader RAM).

- **CORS policy (pure helper, natively tested):** reflect the request `Origin` back only when its host is an RFC1918 private IPv4 or a `.local` name; otherwise no CORS headers. Methods GET/POST, no credentials, `Access-Control-Max-Age` set. Applied ONLY to the per-member surface below. Rationale: reflection beats `*` — a random internet page's origin never validates, so drive-by reads from outside the LAN stay blocked, while any LAN pane works as origin.
- **Per-member surface (CORS-enabled):** `POST /` (the per-card settings save — device-name edit), `GET /settings`, `GET /units/health`, `POST /units/health/refresh`, the `{"seq":N}` maintenance op POSTs + `GET /unit/op-result` (calibrate, offset, unit reboot, wear reset, address burn), `GET /system/stats`, web log read. All form-encoded simple requests — no preflight handler needed beyond the header helper.
- **Leader-owned, never per-member (unchanged producer gates):** text, mode, clock, tz, MQTT state, cluster config. **WiFi is off-limits from the wall UI** — remotely re-pointing a member's WiFi can strand it; that stays on the board's own page.
- **UI:** member pills (on ANY pane — hosts come from `/cluster/status` on the leader, the digest on followers) become buttons; clicking opens an inline panel that fetches the member's `/settings` + `/units/health` and renders the familiar unit grid + maintenance card pointed at that host, plus device name edit and log link. Same DOM-safety rules (text nodes only).
- Unreachable member / no-CORS old firmware → panel shows "unreachable from this browser — open its page" with a plain link.

## Rung 4 — leader failover (#295, separate issue)

4a (promote button): a follower in LocalFallback that still holds a digest shows **"Promote this board to leader"** in the cluster banner. `POST /cluster/promote` runs a pure table transform — its own entry's host blanks (empty host = own row), the dead leader's empty-host entry gets `leader.host` filled in — then applies it as its own cluster config with a fresh epoch; the whole takeover rides the existing join machinery. Join-conflict rule (sticky leadership): a follower clustered to a *live* leader answers a foreign join with 409 + current leader info; a returning old leader collects 409s, clears its member table, and joins the new leader. 4b (automatic): after 4a is bench-drilled — LocalFallback + T minutes → first-alive-in-table self-promotes (deterministic, no votes). Full design lives on #295.

## Testing

- **Native:** faultMask build/parse round-trip; digest build (new pure `ClusterDigest.h`) incl. gen bumping and table passthrough; `/cluster/status` health fold incl. old-follower nulls; CORS origin validator table (private v4 / .local / public / garbage); promote table transform (#295).
- **pytest (`fake_follower.py`):** pins the new ping-reply keys, asserts the leader sends `digest=` with parseable JSON + monotone gen, `/cluster/digest` passthrough.
- **Bench:** two boards + fake followers — strips under every row, follower pane wall mirror, member panel ops against a real follower, old-rev follower degrading gracefully, and (arc 3) the pull-the-plug promote drill.

## Non-goals

Settings *sync* between boards (per-device by design; conflict resolution is where over-design lives), multi-active-master, on-device broker, any change to render/commitAt coordination, WiFi management from the wall UI, SSE for cross-row health.
