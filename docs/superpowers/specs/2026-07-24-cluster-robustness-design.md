# Cluster robustness — contact-age degrade + suspect tier (#385)

**Status:** approved design, not yet implemented
**Scope:** Master (leader side) only. No follower changes, no wire-format changes.
**Supersedes:** the #326 render-busy grace special-case (`CLUSTER_RENDER_GRACE_MS`), which this design retires.

## Problem

The leader marks a member `degraded` after 3 consecutive contact failures
(1.5 s HTTP timeout each). A degraded member is force-rejoined, its pill goes
red, and the HA `cluster_degraded` problem sensor fires. Three failure classes
produce *false* degrades of a healthy board:

1. **Follower I2C stall, render window.** The single-core ESP-01 blocks
   `loop()` ~1.1 s on any I2C transaction to a mid-flap Nano (bench-measured,
   #326). The 6 s post-ACK grace covers only renders that ACKed — a render
   request that itself times out never arms it.
2. **Follower I2C stall, outside any render window.** A heartbeat/health read
   colliding with a unit's autonomous drift re-home (units self-re-home up to
   1/min) stalls identically. No grace applies.
3. **Leader's own netif down.** A leader STA drop (observed 2026-07-24 01:51)
   burns failures against members that are fine.

The #326 lesson: follower-side fixes are falsified (any loop()-context I2C to
a busy slave stalls — physics), and cause-by-cause forgiveness is whack-a-mole.
Overnight evidence on v2026.07.24 (7 h clock mode, zero stall degrades) shows
this is worst-case hardening, not a constant fire — but each false degrade is
disruptive and makes the HA alarm meaningless.

## Design

### 1. Detection: contact-age degrade

Degrade stops being a failure-*count* decision and becomes a wall-clock one:

- `CLUSTER_DEGRADED_SILENCE_MS = 30000` — a member is degraded only when a
  counted failure lands while `nowMs - lastContactMs >= 30 s` (any successful
  round-trip — join, render, ping — refreshes `lastContactMs`, unchanged).
- 30 s aligns with the follower's own 25 s contact-fresh window: by the time
  the leader flags a board, that board has already fallen back to Grace on its
  own. Nobody reacts faster than the wall itself.
- Failures keep incrementing and keep driving the existing 1-2-4-8 s backoff;
  retries land inside any silence window, so a transient ~1.1 s stall of *any*
  class — render, heartbeat collision, re-home, future unknowns — is forgiven
  by construction. `CLUSTER_DEGRADED_AFTER_FAILURES` and
  `CLUSTER_RENDER_GRACE_MS` (+ its `lastRenderMs` grace branch) are removed.
- **Never-contacted members:** `lastContactMs` starts at the runtime epoch —
  every member-table apply (config swap, promote) stamps
  `lastContactMs = nowMs` on all members, so a dead host degrades ~30 s after
  the table goes live instead of never or instantly. A table restored from
  NVS at boot is stamped twice (belt and braces, Codex + cpp-review
  findings): once in `clusterLeaderInit` and again at the first
  netif-connected tick (the edge detector starts false), so the window
  anchors at WiFi-up — the earliest a contact could even be attempted —
  regardless of task/WiFi startup ordering.
- **Deliberate no-contact windows** (Codex finding): while a #276 rollout or
  #304 follower-push upload streams to a member, the fan-out skips it by
  design — the skip branch keeps re-stamping its contact epoch, so however
  the upload window ends (converged, stream abort, 409 holdoff), supervision
  resumes with the full 30 s window instead of a pre-upload contact age.
- Wraparound-safe via the existing `uint32_t` subtraction idiom.

### 2. Suspect tier (quiet intermediate state)

- `suspect` = `failures > 0 && !degraded`. Derived, not stored.
- Suspect consequences: **none that disturb the cluster.** Membership and the
  HMAC key are kept, backoff retries continue, `renderDirty` stays pending.
  Surfacing: additive `"suspect":true` in `/cluster/status` members[], amber
  member pill, one flash-log line per episode (`suspect` onset / cleared —
  transition-only, #322 philosophy). No HA alarm, no forced re-join.
- Degraded keeps today's consequences (`joined = false` → fresh-join recovery,
  red pill, HA `cluster_degraded`) — and now genuinely means "that board is
  running on its own fallback."
- `clusterSuccessorTier`/promote logic, the #276 rollout gates, and the #343
  rescue machinery all key off `degraded`/`joined` exactly as today; suspect
  is invisible to them by design.

### 3. Leader-offline gate

- Glue passes `leaderOnline` (netif up, from the existing WiFi service state)
  into the failure handler. While offline: backoff only — no failure count, no
  degrade evaluation.
- On netif recovery the glue re-stamps every member's `lastContactMs = nowMs`
  (fresh benefit-of-the-doubt epoch). Without this, a >30 s leader outage
  would degrade every member on its first post-reconnect timeout.

### 4. Render-stuck flag

A member whose pings succeed but whose renders keep failing must never
degrade — it is alive; its row just shows stale content. Two pieces make
that guarantee real:

- **Stale-contact ping priority** (review finding, cpp-reviewer HIGH):
  `renderDirty` outranks the keep-alive ping only while contact is fresh.
  Once `now - lastContactMs >= CLUSTER_LEADER_PING_MS`, a liveness ping
  outranks the render — otherwise a member with a broken render path is
  never pinged at all, `lastContactMs` ages to the degrade bar, and an
  alive member degrades. With the inversion, successful pings keep contact
  age ≤ ~13 s forever; a genuinely dead member fails the pings too and
  degrades honestly.
- **The renderStuck flag** surfaces the stale row:

- Stamp `renderDirtySinceMs` when `renderDirty` first sets (0 when clear).
- `renderStuck` = `renderDirty && nowMs - renderDirtySinceMs >= 30 s`
  (reuses `CLUSTER_DEGRADED_SILENCE_MS`). Additive `/cluster/status` key +
  transition-only log line. Cleared by the next ACKed render.

## Touched components

| File | Change |
|---|---|
| `ClusterLeaderPolicy.h` | New constant; age-based degrade in `clusterMemberOnFailure` (gains `leaderOnline` param); remove grace branch + `CLUSTER_DEGRADED_AFTER_FAILURES`; suspect/renderStuck helpers; epoch re-stamp helper. Pure — stays natively tested. |
| `ClusterLeaderFanout.cpp` / `ClusterLeader.cpp` | Pass `leaderOnline`; re-stamp epoch on member-table apply and netif recovery; stamp `renderDirtySinceMs`. |
| `ClusterLeaderStatus.cpp` / `WebCluster.cpp` | Additive `suspect` + `renderStuck` member keys. |
| `ClusterMqtt.h` | `cluster_degraded` unchanged; suspect appears in the attributes payload only. |
| `data/script.js` / `style.css` | Amber pill state (nbsp caveat: edit script.js via python). |
| `test/test_cluster_leader_policy` | Rework degrade cases; new: stall-pattern stays suspect, 30 s silence degrades, offline gate, reconnect re-stamp, never-contacted epoch, renderStuck set/clear. |

`ClusterLeaderPolicy.h` is Master-only (not a copied header) — no drift-gate
impact. Follower trees untouched.

## Error handling / edge cases

- A member answering 409 `not-clustered` keeps today's path (re-join, no
  backoff) — unrelated to failure counting.
- Rescue-beacon members (#343) ride the same detection; their re-push logic
  keys off join replies, not suspect.
- HMAC monotonic-ts marks are untouched (suspect never wipes the key; degrade
  → re-join re-mints exactly as today).
- Log chattiness: suspect episodes are rare in practice (overnight rate 0);
  transition-only logging bounds the worst case at one onset + one clear line
  per episode.

## Testing

- **Native (the correctness tier):** `pio test -e native` in Master —
  `test_cluster_leader_policy` reworked as listed above.
- **Bench (the E2E tier):**
  - False-degrade immunity: #326 repro — leader in clock mode, forced
    full-revolution text renders, concurrent follower `/settings` latency
    probes; assert no suspect→degraded escalation and zero HA alarms.
  - Genuine death: pull follower power → suspect within seconds, degraded at
    ~30 s, HA alarm fires, board's own row falls back; power restore →
    fresh-join recovery.
  - Leader blip: toggle leader WiFi (or AP-side) → no member degrade on
    reconnect.

## Rejected alternatives

- **Cause-aware grace extension** — every stall class must be recognized to
  be forgiven; the arc already bench-falsified one "we understand the stall"
  hypothesis. Structural forgiveness beats enumeration.
- **Retry-burst probing (longer timeouts, immediate re-probes)** — fights the
  sequential fan-out + 5 s core-0 watchdog; buys genuine-failure latency that
  the 30 s decision made unnecessary.
- **Follower-side `Wire.setClockStretchLimit` cap** — still deferred (risks
  dropping legitimate slow reads); unchanged from #326.
