# v2 ESP-01 cluster follower (dumb row)

Issues: #298 (follower firmware) + #297 (leader-side platform guard, ships first or together). Part of epic #270.

Reuse v1 master hardware (ESP-01 + Nano row, I2C on GPIO0/GPIO2) as cheap extra rows in the #270 cluster wall. New minimal firmware — NOT a trim of v1 ESPMaster — that speaks the existing follower wire contract, so one S3 leader can drive 3–5 ESP-01 rows with (almost) no leader-side changes. Typical wall: 1× S3 master (leader) + N× ESP-01 rows.

Decisions locked with the user: existing `/cluster/*` wire contract (no second protocol); no HTML UI beyond the WiFi setup portal; calibration driven from the S3's member panel via op endpoints; blank the row after prolonged leader silence; never a takeover candidate; **no new security mechanism** — same trusted-LAN posture as the rest of the fleet (#294 CORS rules copied, `?md5=` OTA integrity).

## Project shape

- New self-contained PlatformIO project `firmware/v2/FollowerEsp01/`, espressif8266 platform, v1's proven stack: `dvarrel` ESPAsync forks, `eagle.flash.1m.ld` (OTA-capable on 1 MB, proven on this exact board by v1).
- Plain `.cpp` files (v2 style), NOT the v1 `.ino`-concat model.
- Pure-logic headers are trimmed **copies** from v2 Master / v1 (per v2 convention — fix bugs in both trees): follower phase machine, faultMask build, op validation, reflash plan, twiboot + unit protocol helpers.
- Single-core superloop, no RTOS. v1's context rule verbatim: **async handlers stage, `loop()` mutates** — `loop()` is the only I2C toucher (the superloop plays both v2 roles: it drains staged work like netTask and owns the bus like displayTask).
- Debug build flag trades I2C for serial (shared pins, v1 `SERIAL_ENABLE` semantics).
- Honors the twiboot probe quirks: 1500 ms pre-probe delay, probe-inhibit deadline after `/unit/reboot` and address burns.

## Endpoint surface (complete — nothing else exists)

| Endpoint | Behavior |
|---|---|
| `POST /cluster/join` | existing contract; reply gains additive `plat":"esp01"` + own vitals |
| `POST /cluster/render` | segments rendered verbatim to the Nanos; commitAt honored when SNTP-synced, immediate otherwise (existing `clusterRenderDelayMs` rule) |
| `POST /cluster/ping` | health reply (below); `digest=`/`you=` body params ignored |
| `POST /cluster/leave` | existing contract |
| `GET /cluster/health` | existing JSON (leader + member panel) |
| `POST /firmware/master?md5=` | OTA of the ESP-01 image — same contract as S3, so `ota-flash.sh` works unchanged |
| `POST /reflash-units` + progress | twiboot reflash of the row from the bundled hex (v1 machinery) |
| `GET /settings` | tiny JSON: name, rev, plat, width, phase, vitals — enough for the member ⚙ panel |
| `GET /units/health`, `POST /units/health/refresh` | existing shape |
| `GET/POST /unit/offset`, `POST /unit/jog`, `/unit/home`, `/unit/identify`, `/unit/reset-odometer`, `/unit/self-test` (+result), `/unit/reboot`, `GET /unit/op-result` | `{"seq":N}` maintenance-op contract subset — the S3 member panel calibrates this row from the wall UI |

Not served, by design: `/cluster/promote` (never a takeover candidate), `/cluster/digest`, `/cluster/config`, `/cluster/discover`, text/mode/MQTT/clock endpoints, any HTML.

## Behavior

- Follower phase machine (trimmed `ClusterFollowerPolicy` copy): Standalone → Clustered → Grace → **Blank**. Grace (existing 120 s constant) holds the last rendered text; prolonged silence blanks the row — a stale frozen row looks broken, and the leader's health strip already shows it as lost. Rejoin re-renders. Epoch/seq armor kept.
- Standalone (never joined / after leave): blank display. The row is meaningless without its leader.
- Plain SNTP (8266 core built-in, epoch only — no timezone machinery) so commitAt flips stay in unison with the wall; unsynced → render immediately (existing rule).
- Membership is not persisted beyond what the contract requires (`clusteredBy` marker for the boot-into-Grace rule, as v2 followers do); cluster config lives on the leader.
- Producer gating is trivial: there are no local producers. Ops (calibration, reflash) refuse while a render is being applied and vice versa, same 409 discipline as v2.

## Health up (rides #294 rung 1)

Join/ping replies carry the existing keys — `state/epoch/seq/width/detected/faulty/faultMask/wear/rev` — so wall-mirror health strips, `/cluster/status` fold and HA attributes work for ESP-01 rows with zero leader changes. Additive ESP-01 vitals on the same replies: `heap`, `rssi`, `up` (uptime s).

## Leader-side changes (small; ship first or together)

1. **#276 rollout platform guard (the load-bearing one):** join reply's `plat` is parsed into `ClusterMemberRuntime`; any member whose plat differs from the leader's own is **excluded from firmware convergence** (today's leader would stream the S3 image into the ESP-01's `/firmware/master`). Absent `plat` = same-platform (S3 fleet unchanged). Surfaced as `plat` in `/cluster/status` members[].
2. Member pill/panel: show vitals (`heap`/`rssi`/`up`) when present; hide panels for endpoints the plat doesn't serve (no digest link).
3. Discovery: ESP-01 advertises `_splitflap._tcp` with `plat=esp01` in TXT (plus existing name/rev/width) so the Cluster-card scan finds it; scan list shows the plat tag.

## WiFi + identity

v1-style captive setup portal (`split-flap-<hex chipid>-setup`) on first boot or lost credentials; join timeout re-opens the portal. DHCP only. `WiFi.persistent(false)` ordering foot-gun handled as in v1. WiFi credentials are the only on-ESP persisted settings (plus the `clusteredBy` marker); unit offsets/addresses stay in Nano EEPROM as today.

## Unit firmware bundle

`data/unit-firmware.hex` + `.rev` staged by `make_manifest.py stage` (same flow as v2 Master — stage MUST run between the Unit and FollowerEsp01 builds; CI drift `gate` extended to this project). 27 KB hex baked into PROGMEM. Updating the row's Nanos = OTA a new-bundle ESP-01 build, then `POST /reflash-units`. Reply/progress shape reuses v1's; the S3 UI does not orchestrate member reflashes (driven per-row via `ota-flash.sh`/curl or the member panel).

## Size budget

v1's full firmware (MQTT + PROGMEM web app + NTP/tz + mDNS + OTA verdicts) fits the 1 MB OTA layout; this image drops all of that and adds nothing comparably large — expect ~400 KB, comfortable for OTA headroom. ~40 KB free RAM at rest target (v1 ran at ~37 KB with far more services).

## Testing

- **Native** (`pio test -e native`, ArduinoFake — third native project alongside Unit and v2 Master): copied policy headers — phase machine incl. blank-after-grace, faultMask build, op validation, reflash plan slicing, settings-JSON shape.
- **pytest** `tests/fake_leader.py` (counterpart of `fake_follower.py`): pins the ESP-01's wire behavior from the leader's side — join (plat key, health keys), render verbatim + commitAt, ping reply, leave, epoch/seq rejection, op-contract happy path, OTA md5 reject. Runs against the native-hosted logic where possible; full-stack paths are bench.
- **Leader-side native/pytest:** plat parse + rollout exclusion in `ClusterRolloutPolicy`/`ClusterLeaderPolicy` tests; `fake_follower.py` grows a `plat=esp01` variant asserting the leader never streams firmware at it.
- **Bench:** real ESP-01 row joined to the S3 wall — portal join, render + flip sync, health strip, member-panel calibration ops, `ota-flash.sh` OTA, `/reflash-units`, leader-silence blank, rejoin.

## Non-goals

MQTT/HA, clock/timezone/local modes, leader capability, promote/takeover/digest, SSE, transient overlays, wall mirror, TLS/auth (revisit fleet-wide if ever — explicitly dropped for now), settings sync, any custom PCB change.
