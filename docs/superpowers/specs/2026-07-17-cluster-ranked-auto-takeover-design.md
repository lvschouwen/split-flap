# Cluster ranked auto-takeover + takeover-crash fix

**Date:** 2026-07-17
**Epic:** #270 (multi-display cluster), follows #295 (leader failover 4a shipped, 4b deferred)
**Status:** design — awaiting review

## Problem

The bench cluster lost its leader (`split-flap-c8a746` @ `.91`) to a hardware/power failure. The wall went dark: the ESP-01 follower (`.121`) blanked after 2 min of silence, and the surviving S3 (`split-flap-a47dee` @ `.20`) did **not** take over, because automatic takeover was deliberately deferred (#295 4b) and the ESP-01 can never promote.

When `.20` was promoted **manually** (`POST /cluster/promote`), it **crashed and rebooted** — confirmed as a **Task Watchdog reset** (`esp_reset_reason()` → `ESP_RST_TASK_WDT`, surfaced live as `"lastResetReason":"Task watchdog"`). The flash log shows `cluster: leading 3 member(s)` immediately followed by a boot banner in the same second. So the manual promote primitive (4a) is not actually reliable — which is exactly the gate the spec required before auto-takeover was allowed.

This design ships three pieces in one spec + PR:

- **Piece 0 — `/coredump` endpoint** — remote crash diagnostics (also used to confirm Piece 1).
- **Piece 1 — watchdog-safe takeover** — fix the TWDT crash so manual promote is reliable.
- **Piece 2 — ranked auto-takeover** — leader-designated successor + 30 s rank-staggered self-promotion, with a graceful-reboot "hold" so only a genuinely-gone board triggers a takeover.

The ESP-01 follower is behaviorally **untouched** (it never promotes, never probes, never carries a takeover timer).

---

## Piece 0 — `/coredump` HTTP endpoint

**Purpose.** No USB is available over the VPN, and there is no remote way to read the coredump today (`esp_reset_reason()` gives only the category). Add a small read endpoint that exposes the ESP-IDF coredump for the last panic.

**Surface.**
- `GET /coredump/summary` → JSON from `esp_core_dump_get_summary()`: crashed task name, PC, and the backtrace address list (+ `GIT_REV` so the addresses can be resolved against the right `firmware.elf`).
- `GET /coredump/raw` → the raw coredump image (`application/octet-stream`) for `espcoredump.py` offline analysis. Optional; summary is the primary path.
- Returns `404` when no coredump is present (`esp_core_dump_image_check()` fails).

**Security (important).** A coredump contains live RAM — **HMAC per-member keys and WiFi credentials**. This endpoint is therefore placed on the **closed / same-origin surface**, exactly like `/firmware/*`:
- NOT added to `clusterCorsPathAllowed` / `followerCorsPathAllowed` (stays off the cluster-open CORS rung).
- Subject to the same CSRF/Origin rejection as other sensitive routes.
- S3 (Master) only for now; not ported to the ESP-01 (the ESP-01 has no coredump partition and no serial — out of scope, tracked separately as #316).

**Config check.** Confirm the Master `custom_sdkconfig` has `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` + a summary-capable format (ELF). The partition table already reserves `coredump data @ 0x820000 (64 KB)`. If coredump-to-flash is not actually enabled, enable it (guarded by `tests/test_partition_table.py` / a new sdkconfig assertion) — this is a prerequisite for the endpoint to ever return data.

---

## Piece 1 — Watchdog-safe takeover (crash fix)

**Root shape (to be confirmed against the `/coredump/summary` backtrace).** On promote, `clusterPromoteTransform` refills the **dead** leader (`.91`) back into the member table as a plain member, and `applyStagedConfig` resets every per-member runtime to un-joined. The first thing `clusterTask` then does as leader is a **blocking, sequential join fan-out over all members including the dead one** — a 1.5 s `esp_http_client` call to an unreachable host — and the post-promote burst does the whole fan-out in one tick, starving core 0's idle task past the Task Watchdog window.

**Fix.** Make the follower→leader transition watchdog-safe:
- The post-promote fan-out must obey the same **one-member-per-tick** cadence the steady-state supervisor already uses — the takeover must not synchronously drive the entire join round in a single `clusterTask` iteration.
- Between per-member network ops in any burst, yield (`vTaskDelay` / feed the TWDT) so idle runs.
- A dead member during the transition must not block the whole round; it degrades on its own timeout like any other supervised member (existing behavior once we stop bursting).

**Pure-logic seam.** Express "which single member to act on this tick, and when the round is complete" in `ClusterLeaderPolicy.h` (already home to `clusterMemberNextAction`) so it is natively testable — the transition should be a state progression, not a synchronous loop.

**Definition of done (bench drill — the #295 4a gate):** kill the leader, `POST /cluster/promote` on the surviving S3 → it takes over **without resetting** (verify `lastResetReason` stays unchanged across the promote) and re-attaches `.121`. Confirm via `/coredump/summary` that the pre-fix crash was in the takeover path.

---

## Piece 2 — Ranked auto-takeover

Built on the now-reliable promote primitive. Two roles: the leader designates, the S3 followers act.

### Leader side — designate the successor ordering

While leading, the leader computes an ordered list of **eligible successors** and ships it in the ping digest it already sends every tick (#294):

- **Eligible = S3 followers only.** ESP-01s excluded (no promote support). Foreign-platform members excluded (per #297, `clusterMemberPlatForeign`). The leader knows every member's platform from `ClusterMemberRuntime.plat`, so **no `plat` field is added to the digest** — the leader ships the already-filtered result.
- **Ordering = member-table order** (row/col). Table order *is* the rank. Deterministic, no votes.
- **Wire:** one additive digest field, an ordered list of member indices, e.g. `succ=2,3`. Followers already store the promote-critical digest table + `you=` self-index in NVS/EEPROM; they now also store `succ`. Absent `succ` (pre-feature leader) ⇒ auto-takeover disabled, manual promote still works (backward compatible).

A follower's **rank** = the position of its own `you=` index within `succ`. Not in `succ` (e.g. an ESP-01) ⇒ rank = none ⇒ never auto-promotes.

### Follower side (S3 only) — rank-staggered self-promotion

A new, independent auto-takeover timer, separate from the display grace/blank timers:

- Constant `CLUSTER_AUTO_TAKEOVER_MS = 30000` (30 s) — the base delay `T`.
- Constant `CLUSTER_AUTO_TAKEOVER_STAGGER_MS` (e.g. 5 s) — per-rank offset.
- A board with rank `r` that has been in **LocalFallback** (leader silent, past the standalone/fallback threshold) for `T + r * STAGGER` **and still holds a valid `succ` naming it** self-promotes via the Piece-1 promote path.
- **Primary (r=0) fires first at 30 s.** If it is alive, it takes over, re-joins every member (fresh HMAC key per member — followers rekey and reset their replay mark on the new-leader join, per #313), their fallback timers clear on the new leader's contact, and **lower-ranked boards never reach their fire time.** Purely time-ordered — no S3→S3 probing, no votes.
- The existing **sticky-leadership 409** remains the backstop for the rare case where two boards fire near-simultaneously (partition): the loser demotes on contact.

Decision logic is pure in `ClusterFollowerPolicy.h` (`clusterFollowerAutoPromoteDue(state, succList, selfIndex, nowMs)` → bool) with native tests; `ClusterFollower.cpp` calls it from the supervision tick and invokes the existing `clusterFollowerPromote()` when due.

### Graceful-reboot "hold" (load-bearing for 30 s)

At 30 s, a *normal* leader reboot — **including every leader OTA (#305/#276)** and `POST /reboot` — would otherwise look identical to death and trigger a takeover, handing the wall to a unit-less backup while the real leader (with the units) demotes on its sticky-409 return. To prevent this, an **intentional** leader restart announces itself:

- Before an intentional restart (OTA finalize, `/reboot`, reboot-causing config apply), the leader fans out a one-shot **hold hint** to followers — carried on the existing render/ping wire as a `hold=<ms>` field (a bounded value, e.g. ≤ 60 s), HMAC-signed like any leader-wire request.
- On receipt, a follower extends its auto-takeover deadline by `hold` ms (clamped). This pushes the takeover decision past the reboot window, so **the leader comes back and resumes with no promote/demote churn.**
- A follower whose leader **vanishes without a hold** hits the plain 30 s path → takeover. This is the "board is just gone" case.
- The hold **only** suppresses auto-takeover; it does not affect the display grace/blank timers (the wall still holds/blanks on its own schedule).

Pure: `clusterFollowerHoldExtend(state, holdMs, nowMs)` in `ClusterFollowerPolicy.h`, natively tested (including the clamp and that an expired hold reverts to the 30 s rule).

### ESP-01 impact

None behavioral. It is never in `succ`, so it never self-promotes and carries no takeover timer. It already stores the digest; it now stores `succ` and honors `hold` for its **display** grace only if we choose to (optional — the ESP-01 already blanks at 120 s regardless). The pure headers that are copied into the ESP-01 tree (`FollowerPolicy.h`) gain the `succ`/`hold` parsing to keep the twins byte-identical, but the ESP-01's auto-promote path is compiled to a no-op (it has no leader module). Bug-fix-in-both-trees rule applies to the shared pure logic.

---

## Wire changes (summary)

| Field | Direction | Carrier | Meaning | Back-compat |
|-------|-----------|---------|---------|-------------|
| `succ=i,j,…` | leader → follower | ping digest | ordered eligible-successor member indices | absent ⇒ auto-takeover off, manual promote unaffected |
| `hold=<ms>` | leader → follower | render/ping (HMAC-signed) | extend auto-takeover deadline (planned reboot) | absent ⇒ no extension (plain 30 s) |

Both are additive; a pre-feature peer on either end degrades to "manual promote only," never worse than today. No changes to join/leave/render semantics, the HMAC contract, or the IP binding.

---

## Security considerations

- **`/coredump`** is closed-surface / same-origin only (RAM contains keys + creds) — see Piece 0.
- **`hold` must be HMAC-signed** (it rides the existing signed render/ping), so a spoofed hold can't be injected to *suppress* a legitimate takeover. Bounded/clamped so a malicious large value can't wedge takeover indefinitely.
- **`succ`** rides the ping digest, which is already signed under #313 (`ping` signs `ts + sha256(digest) + you`); the successor list is inside `digest`, so it is covered — a swapped `succ` breaks the mac.
- The promoted leader **rekeys every member on takeover** (existing #313 behavior), so a takeover does not weaken the wire-auth posture.

---

## Testing

- **Native (`pio test -e native`)**, both trees where shared: `clusterFollowerAutoPromoteDue` (rank math, staggered fire, `succ` membership), `clusterFollowerHoldExtend` (extend, clamp, expiry), the Piece-1 leader transition state progression.
- **Wire contract:** `tests/fake_follower.py` / `tests/fake_leader.py` pinned for `succ` + `hold` parse/emit; the ESP-01 fake stays in lockstep with the Master twin.
- **Bench drills:**
  1. **Piece 1:** manual promote → no reset (`lastResetReason` unchanged), `.121` re-attaches; `/coredump/summary` confirms the old crash site.
  2. **Auto-takeover:** pull the leader's power → the designated backup S3 takes the wall within ~30 s, ESP-01 never blanks.
  3. **Planned reboot:** leader OTA / `/reboot` → **no** takeover, leader resumes, zero promote/demote churn.
  4. **Two-backup partition:** verify staggered timing means only the primary promotes; sticky-409 resolves the simultaneous edge.

---

## Sequencing / rollout

One branch, one PR, but implemented in dependency order:

1. **Piece 0** `/coredump` → OTA to `.20` → capture the real backtrace.
2. **Piece 1** watchdog-safe takeover → bench drill 1 (the #295 4a gate) must pass **before** Piece 2 is trusted.
3. **Piece 2** leader designate + follower staggered promote + graceful hold → bench drills 2–4.

Firmware convergence note: leader and follower must both carry the feature for `succ`/`hold` to matter, but the additive back-compat means a mid-rollout mixed fleet is safe (degrades to manual promote). The #276 rev-mismatch convergence handles bringing the fleet to one build.

## Open items for review

- **Stagger value** (`CLUSTER_AUTO_TAKEOVER_STAGGER_MS`): 5 s proposed — enough that the primary's re-join reaches peers before the secondary fires, small enough that a dead-primary handoff is still quick. Confirm.
- **Hold trigger points:** OTA finalize + `/reboot` + reboot-causing config apply. Any other intentional restart path to cover?
- **`/coredump/raw`**: include it, or summary-only for now?
