# Headless-monitor role (#332) — design

Slice 3b of #329, the last headless slice. Gives `headless-monitor` its distinct
behavior: an always-on cluster dashboard/API node. Today the three headless
roles are behaviorally identical — only `isHeadlessRole()` is consumed (#331
width-0 forcing); the role vocabulary exists in `HeadlessPolicy.h` but nothing
branches on the individual values.

## Decisions (user-approved 2026-07-20)

1. **Succession: monitor is last resort** (not excluded, not equal). A monitor
   runs full S3 firmware, so a wall whose only surviving S3 is a monitor is not
   stranded — but every other candidate outranks it.
2. **No server-side aggregation.** A follower never becomes an outbound HTTP
   poller; per-member detail stays browser fan-out (#294 architecture). The
   monitor's added value is UI-first + a staleness signal.
3. **#338 (WebEndpoints split) lands first, as its own PR**, then #332 branches
   on the new structure — same-file conflict cascade avoided.

## 1. Role on the cluster wire (additive, the #297 `plat` pattern)

- Follower join/ping **replies** gain `role=<deviceRole>` (wire values from
  `HeadlessPolicy.h`). Absent ⇒ `display` (pre-#332 peer or ESP-01; the ESP-01
  is already excluded from succession by `plat`).
- Leader parses it into `ClusterMemberRuntime`/member status; surfaces `role`
  in `/cluster/status` members[] (like `plat`).

## 2. Role-aware succession — `clusterSuccessorList()` in `ClusterDigest.h`

Current: two passes, `width == 0` members first (proxy for "backup"), then
rendering S3 rows. That makes a monitor OUTRANK real display rows — wrong under
the approved semantics.

New: four tiers, each in table order, S3-only as before:

1. `headless-backup` (width-0 warm standby — unchanged intent)
2. rendering display rows (`display`, width > 0)
3. `headless-spare` (uncommitted shelf unit)
4. `headless-monitor` (promoting it kills the dashboard — true last resort)

Defensive corner: a width-0 member whose role reads `display` (pre-#332 build)
ranks in tier 1's position by the old `width==0` rule — i.e. tier 0 falls back
to "width-0 AND (role backup OR role unknown)" so a mixed-rev cluster keeps
today's behavior until the fleet converges. Pure function, natively tested.

## 3. Digest staleness

- Follower tracks `digestReceivedMs` (millis of last accepted leader ping that
  carried a digest).
- Additive `digestAgeMs` in the **follower's `/settings` JSON** (key absent
  when no digest has ever been received). `GET /cluster/digest` stays byte-raw — its consumers
  (follower page, #294) are wire-pinned.

## 4. Monitor UI (`data/script.js` + `data/style.css` only)

When `/settings.deviceRole == "headless-monitor"`:

- The page boots straight into the existing #294 read-only wall view (wall
  mirror + health strips + browser fan-out member vitals) as the primary card;
  display-only cards stay collapsed as #330 already does for headless roles.
- A "leader silent for Xs" line appears when `digestAgeMs` exceeds ~2× the ping
  cadence; text nodes only (the #294 XSS rule).

No new endpoints. No MQTT changes (a monitor keeps the existing clustered-
follower stand-down behavior).

## Testing

- Native: successor-tier table (all role mixes, mixed-rev fallback), role
  parse/serialize round-trip, `/settings` `digestAgeMs` emission.
- Pytest: `fake_follower` gains a `role` knob; assert the leader's digest
  `succ` order for a 3-role synthetic cluster (pins the wire key both ways).
- Remote bench (no hands needed): set .20's role through the UI POST per role,
  curl the leader digest, assert `succ` reorder live; monitor-page smoke via
  browser fan-out against the real fleet.

## Out of scope

- ESP-01 follower: no role (never a candidate, no UI).
- Standalone (unclustered) monitor: allowed but does nothing special (YAGNI).
- Aggregate `/cluster/monitor` endpoint: rejected (decision 2).
