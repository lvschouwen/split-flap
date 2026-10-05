# Wall link and console — ground-up rebuild (#559)

Status: **draft, awaiting owner confirmation.** Nothing here is built. Section 9 lists what must be measured before the design is fixed.

Rebuilds three things together: how the boards of one Split-Flap talk to each other, the operator API, and the web UI. Existing mechanisms are kept only where their history says they work (section 8).

## 1. Decisions taken (owner, 2026-10-05)

- One ESP32-S3 (the **master**) per Split-Flap. It may drive a row of units itself. Every other row is an ESP-01 (a **row board**). No S3 members, no backup masters, no promotion.
- Up to 8 boards per Split-Flap.
- The boards do not have to speak HTTP to each other.
- WiFi is required. No direct radio path (ESP-NOW was evaluated and declined: 250-byte frames on the ESP8266, no firmware transfer, and it needs every board on one channel, which a multi-AP mesh does not guarantee).
- The row board keeps every unit job it has today: calibration, unit firmware, bootloader update, addresses, self-test, boot dump. Only the way commands reach it changes.
- UI structure: wall → board → unit, plus Firmware, History and Wall settings. Clean neutral look, light and dark. Mock-up approved.
- Wanted information: verdicts with reasons, a timeline of events, firmware state of the whole wall, power and link quality.
- Order: screens first (done), then link and API on both boards, then the UI on the master only.

## 2. What goes away

| Removed | Why |
|---|---|
| Leader calling members over HTTP, one per tick, 1.5 s timeouts (`ClusterLeader*.cpp`, `esp_http_client`) | Source of the stall, degrade and exemption history (#320, #326, #385, #436, #514). |
| The digest in every ping, its 4 KB special-case ceiling, `GET /cluster/digest` | #386 took the wall down; #387 shows it never fits at 8 members; the ESP-01 has no consumer for it. |
| S3 member side (`ClusterFollower.cpp`), promote, successor ranking, sticky leadership, `other-leader`, device roles | No S3 members and no backups. |
| Browser fan-out to members and both CORS allowlists | The browser talks to the master only. |
| Form-encoded wire bodies and the substring JSON scanner | Replaced by one binary codec in `shared/`. |
| Key sent in clear at every leader boot, epoch, NTP time window, persisted replay mark | Replaced by pairing once and a per-connection handshake. |
| Leader pushing firmware (multipart) | #292, #340, #419; and it was the least protected message. |
| ~30 HTTP routes and JSON building on the ESP-01 | The row board sends unit facts as data; the master words them. |

## 3. The link

**Transport.** One TCP connection per row board, opened by the row board to the master, kept open. The master listens; it never dials a row. A dedicated `linkTask` on the master owns the listening socket and all row sockets (plain lwIP sockets with `select`), replacing `clusterTask`.

**Finding the master.** The row board stores the master's id and last address at pairing. If the address stops answering it looks the id up over mDNS (`_splitflap._tcp`). A DHCP move of the master needs no operator action.

**Frame.** `length u16 | type u8 | counter u32 | payload | tag 8 B`. Payload at most 512 B, fixed buffers on both sides, so there is no size ceiling to tune per message. `tag` is a truncated HMAC-SHA256 over the frame under the session key. The counter must increase by one per direction; anything else closes the connection.

**Pairing (once).** The operator adds a row in Wall settings. The master posts a 256-bit pairing key and its own id to the row board's `POST /pair`. An unpaired row board accepts it and stores it; a paired one refuses. A row is released by a signed `RELEASE` frame from its master, or by the row's WiFi reset (which clears the pairing with the credentials). The key crosses the LAN in clear exactly once per pairing — the same threat model as today's join, minus the repeat at every master boot.

**Handshake (every connection).** `HELLO{row id, protocol, nonce, rev, boot id, rescue flag}` → `WELCOME{nonce, proof}` → `READY{proof}`. Both proofs are HMACs over both nonces and both ids under the pairing key; the session key is derived from the same inputs. Replay protection needs no clock and nothing persisted.

**Messages.** One job each. Codec and constants live once in `shared/WallLink.h`, natively tested, with host twins (`fake_row.py`, `fake_master.py`) pinned by pytest as today.

| Direction | Message | Carries |
|---|---|---|
| master → row | `TIME` | master clock for flip sync and for the row's fallback clock; tz rule |
| | `SHOW` | render id, text for this row, speed, flip instant on the link clock |
| | `QUIET` | on/off |
| | `CONFIG` | what to show when the master is lost (blank, time, date), update-units-at-start |
| | `OP` | op id, opcode, unit address, arguments |
| | `UPDATE` | rev, size, checksum, packed flag |
| | `LOG` on/off, `PING`, `RESTART`, `RELEASE` | |
| row → master | `STATUS` | vitals: memory, signal, TX level, uptime, reset ring, bus state, escalation record, image sizes. Every 10 s and on change |
| | `UNITS` | unit facts as the binary struct, a few units per frame. On change and every 30 s |
| | `SHOWN` | render id applied |
| | `OP_STATE` | op id, state, result data (boot dump bytes in chunks) |
| | `EVENT` | code, unit, arguments, row uptime |
| | `LOGLINE`, `PONG` | log lines only while the master asked for them |

**Liveness.** Any frame counts. `PING` after 5 s of silence. The proven numbers stay: the master marks a row lost after 30 s without contact (#385); the row holds its text for 25 s, then is in grace, then at 120 s shows its fallback. A dropped connection is redialled with 1–8 s backoff. The row board's long unit-bus waits (about 1.1 s) no longer matter: nothing has a per-request deadline.

**Time.** The master is the wall's clock. The row estimates the offset from `PING`/`PONG` round trips (lowest round trip wins) and flips at the instant named in `SHOW` (lead 400 ms, as today). The row board no longer needs its own network time. A master without network time still flips rows together; only the wall-clock display needs it.

**Unit jobs.** The master names every job with its own op id; the row echoes it. A row that restarts sends a new boot id in `HELLO`, and the master fails every open job on that row as "row restarted". (Today the row numbers jobs itself and restarts at zero, so a forwarded job can be answered with another job's result.) Opcodes cover the full present set: home one / all, identify, jog, read and set offset, self-test, restart unit, set / clear address, burn all addresses, reset odometer, feature gates, boot info, boot dump, boot update, update one / all units (with force), re-probe, stop. Validation stays in `shared/MaintenancePolicy.h`; the unit bundle stays baked into the row image and the bundle drift gate is unchanged.

**Row firmware.** The master stores one row image (as today) and sends `UPDATE` when a row's rev differs. The row board, if no unit update is running, downloads the image from the master over plain HTTP `GET`, checks it against the checksum that arrived over the signed link, then installs. The attempt cap, rescue accounting and forgiveness rules of `ClusterRolloutPolicy.h` are kept.

**Break-glass, never removed.** The row board keeps two HTTP routes whatever else changes: `POST /firmware` (direct upload, same gate as today) and `GET /id`. Rescue mode (3 early deaths) runs WiFi, the link, the download and those two routes, nothing else. The master keeps `POST /firmware/master`, its A/B rollback and the rescue slot unchanged.

## 4. Master internals

- **One row interface.** The master's own units and an ESP-01 row sit behind the same `RowPort` (own row: the `DisplayCommand` queue; remote row: the link). Nothing above it branches on the kind of row.
- **Wall state.** A `WallState` module (one mutex, snapshot copies) holds rows, unit facts for every row in the one `UnitFacts` struct, open jobs, and firmware state. The API reads snapshots; it never owns state.
- **Verdicts.** Pure headers in `shared/` (`UnitVerdict.h`, `BoardVerdict.h`) turn facts into a level (working / note / fault) and a reason code with arguments. Natively tested. Home Assistant and the TUI use the same result.
- **Event record.** Fixed-size binary records (time, board, unit, code, two arguments) in a ring file on the `storage` LittleFS, written by netTask only. Sources: the edges already detected in `UnitEventLog.h`, starts and their causes, firmware changes, rows lost and back, job results, and `EVENT` frames from rows. Wording happens in the browser from the code.
- **Kept as they are:** grid layout (`ClusterLayout.h`), the display task and its command queue, producer gates, OTA, WiFi, TX ladder, quiet, MQTT (the cluster sensors become a wall problem sensor; `leader_lost` goes).

## 5. Operator API (master only)

Derived from the screens. Readable keys; per-unit tables are columnar (`fields` once, rows as arrays), which measured smaller than today's terse keys.

| Route | For |
|---|---|
| `GET /api/v2/wall` | Wall screen: mode, quiet, rows with text and verdicts, attention list, notes |
| `GET /api/v2/board/{row}` | Board screen: facts, units, voltage, settings |
| `GET /api/v2/unit/{row}/{address}` | Unit screen |
| `GET /api/v2/firmware` | Should-be / is for boards, units, bootloaders, rescue, stored row image |
| `GET /api/v2/history?before=` | Event record, newest first |
| `GET /api/v2/stream` | Server-sent events: wall text, verdict changes, job progress, new events |
| `POST /api/v2/action` | `{name, target, args}` → `{op}`. Showing text, mode, quiet, stop, every unit job, restart, pairing, update |
| `GET /api/v2/op/{id}` | 200 finished, 202 running, 404 unknown |
| `GET` / `PUT /api/v2/settings/wall`, `/settings/board/{row}` | |
| `POST /firmware/master`, `/firmware/row`, `/firmware/rescue` | Uploads, gates unchanged |
| `GET /api/v2/log?row=&kind=` | Raw log; the reply names the board and whether it is the RAM or the flash log |

The TUI and the `flashing/` scripts move to this surface and talk to the master only, also for units on an ESP-01 row. The old routes and the old page are deleted in the last step; there is no period with two web pages.

## 6. Web UI

As the approved mock-up. Vanilla JS in a few modules, system fonts, colour tokens for light and dark, wire strings as text nodes only. The pure model (verdict wording, wall layout, event wording) is tested with `node --test` under pytest. Firmware constants (alphabet, drum steps, limits) are generated from the headers at build time, never copied. Budget: at most 40 KB gzipped (today 52 KB).

## 7. Delivery

On a branch until bench-proven (it must not land half-built), one stage commit rule as usual.

0. **Measurements** (section 9).
1. `shared/WallLink.h`: codec, handshake, message set; native tests and host twins.
2. Row firmware: link client, download-and-install, rescue, break-glass routes. Every unit job proven over the link on the bench.
3. Master: `linkTask`, `RowPort`, `WallState`, pairing, update-by-offer. Old cluster code deleted.
4. Verdicts and the event record.
5. `/api/v2`; TUI and scripts moved.
6. Web UI.
7. Old routes and page deleted. Release `— BREAKING`.

**Moving the installed wall** (once, at the end of step 3): store the new row image on the old master and let the present rollout install it; the row then shows its fallback. OTA the master. Pair the row from Wall settings. If the new row image fails, it is replaced by a direct upload to the row's `POST /firmware`, which works in normal and in rescue mode and needs no master.

## 8. Kept on purpose

Phase timings and contact-age degrade; rollout attempt cap, rescue accounting and forgiveness; the upload gate (`OtaUploadGate.h`) and the pre-inrush OTA confirm; the packed ESP-01 image and its size guard; TX ladder; quiet policy; render stagger; mDNS discovery for finding boards to pair; everything under the unit bus (`UnitBusCore.h`, twiboot, reflash plan, boot integrity); the reflash producer gate and the twiboot probe-inhibit.

## 9. To measure before the design is fixed

- ESP-01 free memory with one open TCP connection, the frame buffers and HMAC, during a unit update (today's low-water mark is about 3.5 KB in that state).
- Flash freed on the ESP-01 by dropping the async web stack and SNTP for the in-core web server and a plain TCP client, against the 511 KB plain / about 600 KB packed ceiling.
- Whether the checksum can be SHA-256 within that flash, or stays MD5 (acceptable because the expected value arrives over the signed link).
- Offset accuracy of the link clock on the real network, against the 400 ms lead.

## 10. Risks

- There is one ESP-01 and, since the spare S3 left, no bench board of either kind. Every trial runs on the mounted wall. A second ESP-01 with a few units would remove most of this risk.
- The master is tried on the live master, protected by A/B rollback and the rescue slot.
