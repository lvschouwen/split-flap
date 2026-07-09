# v2 WiFi join + captive portal + mDNS — design (#188, #58 slice 4)

Decisions locked with user 2026-07-09 (design brief on #58): hand-rolled
portal on the existing async server (Option A), credentials in our own
Settings NVS with esp_wifi storage in RAM, v1 timings verbatim, scan-and-pick
portal page, mDNS included.

## What ships

Stored-credential join + captive-portal fallback + `webEndpointsStart()`
(first live UI) + `/reset-wifi` + mDNS, all inside the #187 task
architecture: `netTask` owns WiFi supervision, web handlers stage only.

## Credential store

New NVS keys via the existing `SettingsStore` seam:

| key        | field                    | rule |
|------------|--------------------------|------|
| `wifiSsid` | `MasterSettings.wifiSsid`| 1..32 printable ASCII bytes; empty = unprovisioned |
| `wifiPass` | `MasterSettings.wifiPass`| empty (open AP) or 8..63 printable ASCII; write-only in `/settings` (never serialized) |

- `SettingsLimits.h`: `LEN_WIFI_SSID 33`, `LEN_WIFI_PASSWORD 64` (validators
  accept strictly shorter, same slot-heritage rule as the rest).
- `SettingsValidation.h`: `isValidWifiSsidValue()`, `isValidWifiPasswordValue()`.
- `SettingsStore` grows `virtual void remove(const char* key)` —
  `/reset-wifi` is two key deletes, not two empty writes. `NvsSettingsStore`
  forwards to `Preferences::remove`; the in-memory test fake erases.
- Load sanitation: invalid ssid → both fields cleared (a stored password
  without a usable ssid is dead weight); invalid pass → pass cleared.
- `WiFi.persistent(false)` before any radio call — esp_wifi keeps its own
  copy in RAM only; our NVS namespace is the single credential store. The
  v1 `persistent()`-before-`disconnect()` foot-gun class dies here.
- `/settings` JSON: `wifiSettingsResettable` flips to true when a ssid is
  stored. SSID itself is not exposed (parity: v1 exposes nothing).

## Join/portal policy — pure logic, natively tested

`WifiPolicy.h` — a step function over a POD state, no Arduino radio types:

```
enum class WifiPhase { Boot, Joining, Connected, Portal };
enum class WifiAction { None, StartJoin, StartPortal, StartOnline, SaveAndReboot, Reboot };

WifiAction wifiPolicyStep(WifiPolicyState& st, uint32_t nowMs,
                          bool linkUp, bool credsStored, bool portalSubmitted);
```

Transition table (v1 `initWiFi()` parity):

- `Boot`: creds stored → `Joining` (emit `StartJoin`, deadline now+30 000);
  none stored → `Portal` (emit `StartPortal`, deadline now+300 000).
- `Joining`: `linkUp` → `Connected`, emit `StartOnline` (mDNS +
  `webEndpointsStart()`); deadline passed → `Portal` (emit `StartPortal`).
- `Portal`: `portalSubmitted` → emit `SaveAndReboot`; deadline passed →
  emit `Reboot` (v1's parked reboot-retry cycle — router may have been down).
- `Connected`: terminal. Link drops are the SDK's problem
  (`WiFi.setAutoReconnect(true)`), never a portal re-entry — v1 parity.

Timeouts are `constexpr` parameters of the header so tests pin them:
`WIFI_JOIN_TIMEOUT_MS = 30000`, `WIFI_PORTAL_TIMEOUT_MS = 300000`.

Rollover-safe deadline math (`(int32_t)(nowMs - deadline) >= 0`).

## Scan JSON — pure logic, natively tested

`WifiScanJson.h`: `buildWifiScanJson(entries, n)` →
`{"networks":[{"ssid":"…","rssi":-42,"secure":true},…]}`.
Dedup by ssid keeping strongest RSSI, sort descending RSSI, drop empty
ssids (hidden APs), escape via SettingsJson.h's `jsonEscape`, cap at 20.

## Hardware-facing service (thin, bench-verified)

`WifiService.h/.cpp` — owns `WiFi`, `DNSServer`, `MDNS`. Runs entirely in
`netTask` (`wifiServiceTick()` added to `netTaskMain` beside
`webEndpointsLoop()`); handlers reach it only through mutex-guarded staging:

- `wifiStagePortalConfig(ssid, pass)` — from `POST /wifi/config`.
- `wifiStageReset()` — from `POST /reset-wifi`.
- `wifiStageScan()` — from `POST /wifi/scan`; tick starts
  `WiFi.scanNetworks(/*async=*/true)`, completion is polled in tick and the
  JSON cached for `GET /wifi/scan` (202 while pending).

Action execution in tick:

- `StartJoin`: `WiFi.persistent(false)`, `WiFi.mode(WIFI_STA)`,
  `WiFi.setHostname(deviceName)`, `WiFi.setAutoReconnect(true)`,
  `WiFi.begin(ssid, pass)`.
- `StartPortal`: `WiFi.mode(WIFI_AP)`, `WiFi.softAP(deviceName + "-setup")`
  (`AP_SUFFIX_SETUP`, open — v1 parity), DNSServer catch-all `*` →
  `softAPIP()`, then `webEndpointsStart()` (LWIP is up on the AP netif).
  `dnsServer.processNextRequest()` every tick while in portal.
- `StartOnline`: log IP, `webEndpointsStart()`, `MDNS.begin(deviceName)` +
  `addService("http","tcp",80)`.
- `SaveAndReboot`: `saveWifiCredentials()`, log, 750 ms grace,
  `ESP.restart()`.  `Reboot`: grace + restart.
- Staged reset (any phase): `clearWifiCredentials()` (two `remove()`s),
  grace, restart — next boot lands in the portal.

`webEndpointsStart()` is idempotent-guarded (a bool) — exactly one
`server.begin()` regardless of which path brings the netif up.

Recovery/OTA portal suppression: v2 has no recovery/OTA boot modes yet;
the policy's `Boot` entry is where that slice will branch before any
portal can open (documented at the call site).

## Portal page

`data/portal.html` — self-contained (inline CSS/JS, no dependency on
script.js/style.css so it renders instantly on a captive check webview):
scan list (auto-triggers `POST /wifi/scan` on load, polls `GET /wifi/scan`,
tap to fill) + manual SSID field + password field + save via
`POST /wifi/config` (fetch, shows "rebooting…" on ok).
Added to `build_assets.py` `ASSETS` → `PORTAL_HTML_GZ`.

Routing: `GET /wifi-setup` serves it always (from the LAN it doubles as a
"change WiFi" page). In portal phase, `server.onNotFound` redirects to
`http://192.168.4.1/wifi-setup` — the DNS catch-all plus this redirect is
what pops the OS captive-portal sheet (`/generate_204` etc. all land in
`onNotFound`). Outside portal phase, `onNotFound` is a plain 404 (v1 has no
catch-all either).

## Endpoint changes (WebEndpoints.cpp)

| route | method | behavior |
|---|---|---|
| `/wifi-setup` | GET | portal page from PROGMEM |
| `/wifi/scan` | POST | arm scan (stage), 200 |
| `/wifi/scan` | GET | 202 `pending` / 200 scan JSON |
| `/wifi/config` | POST | validate ssid+pass (400 on fail), stage, 200 `ok-reboot` |
| `/reset-wifi` | POST | stub retired; stage reset, 200 + v1 message text |
| `/settings` | GET | `wifiSettingsResettable` now live |

## main.cpp

`wifiServiceInit(settings, deviceName)` after `tasksInit()` (it only stores
pointers + builds phase state; the radio comes up on netTask's first tick,
keeping setup() fast and all WiFi work on core 0).

## Tests

Native (new suites): `test_wifi_policy` (full transition table incl.
timeout edges + rollover), `test_wifi_scan_json` (escape/dedup/sort/cap/
hidden), extensions to `test_settings_validation` (ssid/pass rules) and
`test_settings_store` (wifi keys round-trip, sanitize-on-load pairs,
remove()). pytest: `ASSETS` list gains portal.html (existing build_assets
tests updated if they enumerate).

## Out of scope

Recovery/OTA boot modes (later slice), NTP (clock slice), any I2C.
