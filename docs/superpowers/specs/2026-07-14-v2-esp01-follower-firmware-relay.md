# v2 — ESP-01 follower firmware, S3-relayed (#304 Part B, epic #270)

## Problem

An ESP-01 dumb-row follower (#298) can't be firmware-updated from the S3 wall
UI. #276 fleet convergence streams the leader's OWN running app slot — correct
for S3→S3, but #297 deliberately EXCLUDES the ESP-01 (an S3 image would brick
it). So the S3 has no path to push the *right* (follower) image, and the only
way to update a wall row today is `ota-flash.sh` / curl straight to the ESP-01.

## Decision (bench, 2026-07-14): 2a — S3 stores + relays, on-demand

The issue's original Part B was a browser-direct upload with a scoped CORS
exception on the follower's `/firmware/master`. Dropped in favour of the
"Alternative considered" the issue had parked:

- The operator uploads a `follower-<rev>.bin` to the S3 **once** (same-origin,
  no CORS); the S3 stores it on the shared `storage` LittleFS.
- On demand, the S3 streams that stored image to a chosen ESP-01 row,
  server-to-server via clusterTask, reusing #276's chunked multipart machinery.

**Why the flip:** capacity overturns the parking objection ("380 KB relay
burden"). `storage` is 5.8 MB with ~3.8 MB free after FlashLog's 2 MB budget,
and the partition-table comment already names "staged unit firmware" as a
tenant. The follower bin is 384 KB (~10× headroom). Benefits:

- **#294's "/firmware/* stays closed" boundary stays fully intact** — the
  browser never touches the follower's `/firmware/master`; the S3 relay is not
  a browser request, so no CORS exception anywhere.
- Reuses #276's proven streaming rather than a parallel browser mechanism.
- "The follower image lives on the S3" = the real "manage everything from the
  S3" outcome. Upload once → push to any/all ESP-01 rows.

**Trigger = on-demand**, not auto-converge: a bad stored image must not
silently re-brick every row on rejoin. The operator pulls the trigger.

## Invariant compliance (all "always-review" tiers)

- **netTask is the SOLE flash writer to `storage`.** The upload's async
  handler accumulates the image into a **PSRAM buffer** (`largeAlloc`) and
  hands it off under a mutex; **netTask** performs the single
  `open→write→close` of `/follower-fw.bin` + `/follower-fw.rev`. The async
  handler never touches LittleFS.
- **clusterTask is the SOLE outbound HTTP caller.** The relay runs there; the
  web endpoint only *stages* the target host (like `clusterLeaderStageConfig`).
- **#297 preserved.** The relay streams ONLY the stored follower image at an
  `esp01` member — never the S3 slot, never at an S3. It is the inverse guard,
  not a hole in #297.
- **Mutually exclusive with the #276 auto-rollout** (`rollout.phase == Idle`
  to start; auto-rollout skips a member while a follower-push is in flight).

## Components

### Storage
- `POST /cluster/follower-firmware?md5=&v=` (same-origin). Async multipart
  upload; cursor-checked chunk accumulation into a 384 KB PSRAM buffer; MD5
  verified on the final chunk (mandatory, mirrors `/firmware/master`).
- Handoff: `{buf,len,rev,md5}` staged under a mutex + pending flag; netTask
  drain (in `webEndpointsLoop`) writes `/follower-fw.bin` + `/follower-fw.rev`,
  frees the buffer. Boot reads `/follower-fw.rev` → stored-rev state.
- Prefix guard: accept only `follower-*.bin` (mirrors ota-flash.sh #299).

### Relay
- `POST /cluster/member/update?host=…` stages the target host for clusterTask.
- A self-contained follower-push session on clusterTask mirrors
  `rolloutOpenUpload`/`rolloutPumpUpload` but sources chunks from the LittleFS
  `File`; MD5 via `MD5Builder` over the file. Reuses the image-agnostic
  multipart helpers (`clusterRolloutBoundary/ContentType/Preamble/Trailer/Url`
  in `ClusterRolloutPolicy.h`). The `/firmware/master` S3→S3 path is untouched.
- Guards: target foreign-plat `esp01`, stored image present, auto-rollout idle.
- Progress/verdict surfaced in `GET /cluster/status`.

### Pure logic — `FollowerImagePolicy.h` (natively tested)
- `followerImageUploadAccepts(filename)` — `follower-*.bin` only; extract rev.
- `followerPushEligibility(memberPlat, leaderPlat, storedPresent)` →
  `{Eligible, NotEsp01, NoStoredImage}`. Rev-equality is a UI concern (hide the
  button) — the endpoint still allows a deliberate re-flash.
- `followerImageChunkOk(index, accumulated, len, cap)` — cursor-match +
  capacity bound for the PSRAM accumulator.

### UI (`data/script.js`)
- S3 Cluster card (Settings tab): "ESP-01 firmware" — same-origin upload to
  `/cluster/follower-firmware` (SparkMD5), shows the stored rev.
- ESP-01 member ⚙ panel: "Update this row" → `POST /cluster/member/update`;
  progress polled from `/cluster/status`. Shown only when a stored image exists
  and its rev ≠ the member's rev.

## Tests
- Native: `FollowerImagePolicy` (upload accept, eligibility matrix, chunk
  cursor/bounds).
- `tests/fake_follower.py` (esp01 variant) already speaks `/firmware/master` —
  extend to assert the S3→follower streamed image round-trips.

## Bench (user)
- Upload a `follower-<rev>.bin` to the S3; confirm stored-rev shows in the
  Cluster card. Trigger "Update this row" on an ESP-01; confirm it reboots onto
  the new rev and rejoins. Confirm the control is absent for an S3 member and
  refused server-side, and that an S3 image is refused at upload (prefix guard).
