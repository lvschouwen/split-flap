# v2 OTA: /firmware/master upload + native rollback verdicts (#190, #58 slice 5)

Date: 2026-07-10 · Status: approved for implementation (overnight run, decisions pre-locked 2026-07-09)

## Problem

The v2 master needs web OTA. The S3's native bootloader A/B rollback is already
enabled (#187: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, 2×4 MB app slots), but
today the Arduino core auto-confirms every freshly-OTA'd image inside
`initArduino()` — before `setup()` runs — so rollback protection is effectively
inert. And `flashing/ota-master.sh` needs its v1 wire contract served.

## Locked decisions (user, 2026-07-09)

- Native `esp_ota_*` state APIs + the core's weak `verifyRollbackLater()` /
  `verifyOta()` hooks. **Do NOT port v1's RTC-cookie / sketchMd5-compare
  machinery** — the S3's true A/B boot has no eboot copy step, so the silent-
  revert failure class it detected does not exist.
- `/firmware/master` keeps the v1 wire contract so `ota-master.sh` works
  unchanged.
- `/settings` verdict fields are synthesized from `esp_ota` state, not
  re-implemented.

## Wire contract (v1 parity)

`POST /firmware/master?md5=<32-hex>[&v=<rev>]`, multipart field `firmware`.

- `md5` mandatory (v1 #144): missing / not 32 chars / not lowercase-hex → 400.
  Normalization + validation is pure (`OtaStatus.h`, natively tested).
- `v` optional: persisted to NVS as `intendedVersion` — pure diagnostics
  ("what was last attempted"), valuable exactly when a revert leaves the old
  image running. Not used for verdict detection (that was #52's eboot problem).
- Stream → `Update` (ESP32 core): `begin(UPDATE_SIZE_UNKNOWN)` targets the
  inactive OTA slot, `setMD5()` makes `end(true)` verify the digest,
  `_verifyEnd()` arms the slot via `esp_ota_set_boot_partition` (image boots
  PENDING_VERIFY). A stale in-flight update is `abort()`ed on a new upload's
  first chunk (v1 #162 re-entry class).
- Responses: 200 `"Master firmware flashed; rebooting…"`, 400 md5 problems,
  500 `Update` errors (`ota-master.sh` treats any non-200 as upload-failed).
- Success stages the existing `pendingReboot` (750 ms grace, drained by
  netTask's `webEndpointsLoop`) — same reboot path as `/reboot`. The staged
  `intendedVersion` NVS write happens in the drain too (async-context rule:
  no NVS writes from handlers).

**Async-context exception (deliberate, v1 precedent):** `Update.write()` runs
in the upload callback (async_tcp task) — a firmware stream cannot be staged
through a queue. This is the one sanctioned exception to the handlers-stage/
tick-mutates rule; the handler still never touches settings, radio, or NVS.

## Rollback gating

- Strong `verifyRollbackLater()` → `true` (OtaService.cpp): defers confirmation
  out of `initArduino()`.
- `otaServiceInit()` (setup(), single-threaded, before tasksInit): snapshots
  `esp_ota_get_state_partition(running)` == PENDING_VERIFY and
  `esp_ota_get_last_invalid_partition() != NULL` (a rollback happened).
- `otaHealthConfirm()` — called by WifiService's `startOnline()` **and**
  `startPortal()` (netTask): first call while pending →
  `esp_ota_mark_app_valid_cancel_rollback()`.
  Rationale: PENDING_VERIFY protects against boot-loop/brick images. Portal-up
  proves boot + radio + web stack alive, and must confirm: a legit
  router-down scenario reboots through the portal-timeout path, and an
  unconfirmed image would be silently reverted by that reboot.
- If the image crashes/reboots before either netif comes up, the bootloader
  reverts to the previous slot on the next boot. That is the entire v1
  verdict machinery, for free.

## /settings verdict synthesis (pure, natively tested)

`synthesizeOtaVerdict(rolledBack, pendingVerify, confirmedThisBoot)` →
`{lastFlashResult, otaReverted}`:

| state | lastFlashResult | otaReverted |
|---|---|---|
| last_invalid partition exists | `"reverted"` | true |
| confirmed this boot (was pending) | `"ok"` | false |
| still pending (net not up yet) | `"pending"` | false |
| plain boot | `""` | false |

`ota-master.sh` verdicts stay correct: SUCCESS is primarily `sketchMd5 ==
uploaded md5` (holds — ESP32 `getSketchMD5()` hashes the running app image,
which is byte-identical to the uploaded .bin); REVERTED trips on
`lastFlashResult == "reverted"`. `"pending"` is new but the script only
string-matches `"reverted"`/`"ok"`. Cross-task reads (async /settings handler
vs netTask confirm) go through a mutex-guarded snapshot getter in OtaService.

Other v1 fields: `flashConfigMismatch` stays `false` (ESP8266 flash-header
concept, N/A), `bootCounter`/`recoveryMode`/`isInOtaMode` stay 0/false —
recovery + quiet-OTA modes are a different problem class (bad-but-confirmed
firmware) and a later slice; `/firmware/recover-mark` + `/firmware/ota-mode`
remain 501.

`GET /debug/ota` (retires its stub): small JSON — running/next partition
labels, ota states, last-invalid label, verdict fields. Bench candy for the
A/B flow.

## Not ported / deferred (explicit)

- v1 display-freeze (`masterOtaUploadActive`) + `mqttStopForOta()` — no I2C or
  MQTT on v2 yet; the flag returns with the I2C slice.
- v1 OTA TX-power reduction (#60) — ESP-01 power-sag workaround; the S3 devkit
  has a real supply.
- Recovery mode / boot counter (see above).

## Files

- `OtaStatus.h` (new, pure): `normalizeOtaMd5()` + `synthesizeOtaVerdict()` —
  `test/test_ota_status/`.
- `OtaService.h/.cpp` (new): esp_ota integration, `verifyRollbackLater()`
  override, `otaHealthConfirm()`, mutex-guarded status snapshot.
- `WebEndpoints.cpp`: `/firmware/master` two-lambda route, `/debug/ota`,
  `/settings` field wiring, `intendedVersion` staging in the drain.
- `Settings.h`/`SettingsLimits.h`: `intendedVersion` NVS slot (load + truncate
  to `LEN_INTENDED_VERSION`; not a user-facing POST field).
- `WifiService.cpp`: `otaHealthConfirm()` from `startOnline()`/`startPortal()`.
- `main.cpp`: `otaServiceInit()` after settings load, before `tasksInit()`.
