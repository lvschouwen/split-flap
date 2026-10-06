# Wall link and console — ground-up rebuild (#559)

Status: **confirmed by the owner 2026-10-05; measurements done (#560, section 9).** Nothing here is built yet.

Rebuilds three things together: how the boards of one Split-Flap talk to each other, the operator API, and the web UI. Existing mechanisms are kept only where their history says they work (section 8).

## 1. Decisions taken (owner, 2026-10-05)

- One ESP32-S3 (the **master**) per Split-Flap. It may drive a row of units itself. Every other row is an ESP-01 (a **row board**). No S3 members, no backup masters, no promotion.
- Up to 8 boards per Split-Flap.
- The boards do not have to speak HTTP to each other.
- All boards of a Split-Flap are on the owner's LAN, always. Messages between boards are not signed or encrypted. What stays: the LAN-origin check on the master's browser-facing routes (a website must not be able to drive the wall through the owner's browser), the row board accepting nothing inbound except a direct firmware upload, and the checksum and size guards on firmware (they protect against a damaged image, not against people).
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
| Per-member keys, HMAC on every request, epoch, NTP time window, persisted replay mark, `ClusterHmac.h` | Trusted LAN (section 1). |
| Leader pushing firmware (multipart) | #292, #340, #419. |
| ~30 HTTP routes and JSON building on the ESP-01 | The row board sends unit facts as data; the master words them. |
| The terminal tool (`cli/`: TUI and client library) | Retired by the owner; one console (the web UI) and curl. |

## 3. The link

**Transport.** One TCP connection per row board, opened by the row board to the master, kept open. The master listens; it never dials a row. A dedicated `linkTask` on the master owns the listening socket and all row sockets (plain lwIP sockets with `select`), replacing `clusterTask`.

**Finding the master.** The row board stores the master's id and address at pairing and dials that address. A master whose address moved posts its pairing again from the new one (see Pairing); the row takes it once the old address has been silent for 25 s. A lookup of the id over mDNS (`_splitflap._tcp`) on the row is not built: the re-pair covers a DHCP move without it.

**Format.** Messages are described once in `firmware/v2/link/wall_link.proto` and generated for both boards with nanopb (Protocol Buffers; fixed-size fields, no heap). Each message on the connection is one envelope, `ToRow` or `ToMaster`, length-delimited. This is the pattern ESPHome's native API uses on the same chips. Field numbers are the contract; messages grow only at the end.

**Pairing (once).** The operator adds a row in Wall settings. The master posts its own id to the row board's `POST /pair` (form field `master`); its address is the caller's, and the answer is the row's identity. The row obeys one master, known by id and address (`FollowerPairPolicy.h`): an unpaired row stores the caller; its own master at its own address changes nothing; its own master's id from another address is taken only once the master has been out of contact for 25 s, because the id is no secret and must not be enough to redirect a working row; another master is refused (409, naming the present one) until the row has written its own off (120 s without contact), so a replaced master needs no step on the row. A stored pairing starts the contact window afresh: the new master has those 25 s to connect, and the 120 s count from there. A row is released by a `Release` message from its master. The ESP-01 has no reset button and its setup portal opens only when the WiFi cannot be joined, so there is no reset that clears a pairing; the lost-master rule is what frees a row.

**Hello (every connection).** `Hello{row id, protocol, rev, boot id, rescue flag}` → `Welcome{master id}`. A row that reaches a master it is not paired with closes the connection.

**Pacing.** A row board in a long unit job does not read its socket (measured: up to 12.4 s during one unit update), and unread messages cost it memory until TCP's own receive window stops the sender (section 9). There are no message counters. The master keeps only the latest text per row, never a queue; it sends a ping only when the connection has been idle; and it holds pings while a row reports itself busy.

**Messages.** One job each, as listed in the schema file. Host-side stand-ins (`fake_master.py`, `fake_row.py`) use stock Python protobuf generated from the same file.

| Direction | Message | Carries |
|---|---|---|
| master → row | `Welcome` | master id |
| | `Show` | render id, text for this row, speed, flip instant (Unix ms) |
| | `Quiet` | on/off |
| | `Config` | what to show when the master is lost (blank, time, date), update-units-at-start, tz rule |
| | `Op` | op id, opcode, unit address, arguments |
| | `Update` | rev, size, MD5, packed flag, HTTP port |
| | `LogCtl`, `Ping`, `Restart`, `Release` | |
| row → master | `Hello` | row id, protocol, rev, boot id, rescue flag, width |
| | `Status` | vitals: memory, signal, TX level, uptime, bus state, escalations, busy flag, image size, time synced. Every 10 s and on change |
| | `UnitsJson` | the row's unit facts as JSON, exactly as the shared serializer writes them (calibration offset included as `ofs`), in pieces. On change and every 30 s. The master reads values out with ArduinoJson |
| | `Shown` | render id applied, and how late if it missed its instant |
| | `OpState` | op id, phase, reason, result data (boot dump bytes in pieces) |
| | `Event` | code, unit, arguments, row uptime |
| | `LogLine`, `Pong` | log lines only while the master asked for them with `LogCtl`, per connection: first what the row's 4 KB ring still holds and has not sent before, then each new line. Lines logged while the master was away follow after the reconnect |

**Liveness.** Any message counts. `Ping` after 10 s of silence, not while the row is busy. The proven numbers stay: the master marks a row lost after 30 s without contact (#385); the row holds its text for 25 s, then is in grace, then at 120 s shows its fallback. A dropped connection is redialled with 1–8 s backoff. The row board's long unit-bus waits (about 1.1 s) no longer matter: nothing has a per-request deadline.

**Time.** The row board takes its time from the master: its built-in SNTP client is pointed at the master, which answers time requests. Rows flip at an instant the master names in `Show` (Unix milliseconds), so flipping together needs the row to agree with the master, not with the internet. Content the master knows in advance (the clock's minute change) is sent at least 2 s ahead; typed text keeps the 400 ms lead, and a message that arrives after its instant flips on arrival. The accuracy of the built-in client against the master is to be measured on the row; if rows visibly flip apart, the fallback is round-trip timing inside the connection, which measured under 1 ms (#560). Not used: ESPNtpClient (no release since June 2022, and it crashes on current ESP32 cores, arduino-esp32 #10902).

**Unit jobs.** The master names every job with its own op id; the row echoes it. A row that restarts sends a new boot id in `HELLO`, and the master fails every open job on that row as "row restarted". (Today the row numbers jobs itself and restarts at zero, so a forwarded job can be answered with another job's result.) Opcodes cover the full present set: home one / all, identify, jog, read and set offset, self-test, restart unit, set / clear address, burn all addresses, reset odometer, feature gates, boot info, boot dump, boot update, update one / all units (with force), re-probe, stop. Validation stays in `shared/MaintenancePolicy.h`; the unit bundle stays baked into the row image and the bundle drift gate is unchanged.

**Row firmware.** The master stores one row image (as today) and sends `UPDATE` when a row's rev differs. The row board, if no unit update is running, downloads the image from the master over plain HTTP `GET`, checks it against the MD5 from the `UPDATE` frame, then installs. The download is `GET /firmware/row` on the port the `UPDATE` frame names; the answer's length and the file's first bytes are checked before anything is erased. The row answers every `UPDATE` with `UpdateState` (downloading, installed, failed or refused, with a reason), and does not read the link while it downloads (about 25 s measured). A row in rescue mode takes the offered image even at the rev it runs. The attempt cap, rescue accounting and forgiveness rules of `ClusterRolloutPolicy.h` are kept.

**Row board stack.** The row board keeps the web stack it has: the async server, the WiFi setup portal with its join-retry rules, and the present firmware upload route with its gates and packed-image checks. All three are proven on the wall, the upload route is the recovery path, and the space a lighter stack would free is not needed (section 9: about 435 KB after the rebuild against 511 KB plain and 602 KB packed). What goes is every other route. The link is a `WiFiClient` polled from `loop()`, as in the #560 trial, so link traffic touches state and the unit bus from `loop()` only; the existing rule that web handlers stage and `loop()` acts stays for the few routes left.

**Second heap (adopted after the trial of #564).** The row image is built with the core's second heap (`PIO_FRAMEWORK_ARDUINO_MMU_CACHE16_IRAM48_SECHEAP_SHARED`): the code memory the image leaves unused, 19.7 KB of the 48 KB block. The price is half the flash-code cache and slow byte-wise access to that heap (about 3 µs a byte). Three buffers live there: the log ring (4.1 KB, for good), the unit facts document while it travels and the boot dump block while a dump is held. The link's message structs stay in fixed memory: they are small and read field by field on every message. Section 9 has the trial's numbers.

**Two-stage update (not built; the way out if the image outgrows one step).** The 511 KB plain and 602 KB packed limits belong to the one-step update, where the running image and the downloaded copy share the 1,028 KB firmware area. A small helper image (WiFi and fetch only, about 270 KB; the rescue mode is close to one) installed first leaves room for a packed download of about 750 KB, i.e. a full image approaching the whole area — the pattern Tasmota uses on 1 MB devices. Before anything relies on it: the build's guard that the unpacked image must end below its stored copy has to be replaced by a check of the real overlap, proven with the host test that runs the core's boot copier (`tests/test_ota_gzip_eboot.py`); the master has to store two images; and a row left in the helper by a power cut has to be finished by the master. Trigger: the row image passing about 600 KB. The rebuilt image is expected around 435 KB.

**Break-glass, never removed.** The row board keeps the routes it needs without a master, and no others: the firmware upload (`POST /firmware/master`, unchanged, so `ota-flash.sh` keeps working), the identity read (`GET /settings`: name, rev as `version`, `plat`, width, rescue flag, master id and address, link up, uptime, free memory, image size and free space, flash mode and chip id; `ota-flash.sh` reads `version` and `plat`), `POST /pair`, and the WiFi setup portal. Rescue mode (3 early deaths) runs WiFi, the link, the download and the same routes, nothing else. Without a master there is no way to restart the row short of its power, and its log, reset history and bus detail are read only over the link. The master keeps `POST /firmware/master`, its A/B rollback and the rescue slot unchanged.

## 4. Master internals

- **One row interface.** The master's own units and an ESP-01 row sit behind the same `RowPort` (own row: the `DisplayCommand` queue; remote row: the link). Nothing above it branches on the kind of row.
- **Wall state.** A `WallState` module (one mutex, snapshot copies) holds rows, unit facts for every row in the one `UnitFacts` struct, open jobs, and firmware state. The API reads snapshots; it never owns state.
- **JSON on the master.** New API responses are built with ArduinoJson, and a row's unit facts are read with it. The MQTT text parser moves to it as well (its hand-written parser appears to mis-decode `\u` escapes; a test confirms that first).
- **Verdicts.** Pure headers in `shared/` (`UnitVerdict.h`, `BoardVerdict.h`) turn facts into a level (working / note / fault) and a reason code with arguments. Natively tested. Home Assistant uses the same result.
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

The `flashing/` scripts move to this surface and talk to the master only, also for units on an ESP-01 row; they stay curl-only, like `ota-flash.sh`, because recovery must need nothing else. The terminal tool (`cli/`: the TUI and its client library) is retired (owner, 2026-10-06): the web UI and curl are the operator surfaces. The old routes and the old page are deleted in the last step; there is no period with two web pages.

## 6. Web UI

As the approved mock-up. Vanilla JS in a few modules, system fonts, colour tokens for light and dark, wire strings as text nodes only. The pure model (verdict wording, wall layout, event wording) is tested with `node --test` under pytest. Firmware constants (alphabet, drum steps, limits) are generated from the headers at build time, never copied. Budget: at most 40 KB gzipped (today 52 KB).

## 7. Delivery

On a branch until bench-proven (it must not land half-built), one stage commit rule as usual.

0. **Measurements** (section 9).
1. `firmware/v2/link/`: the schema, the stream reader, unit facts in pieces; native tests; stock protobuf reads the boards' bytes. **Done (#562).**
2. Row firmware: link client, download-and-install, rescue, break-glass routes. Every unit job proven over the link on the bench.
3. Master: `linkTask`, `RowPort`, `WallState`, pairing, update-by-offer. Old cluster code and `cli/` deleted. The first part of `/api/v2` lands here (`POST /api/v2/action` for pairing, release and unit jobs, `GET /api/v2/op/{id}`), so rows and their units have an operator path, by curl, before the new page exists; the present page loses its cluster parts and keeps working for the master's own row.
4. Verdicts and the event record.
5. `/api/v2`; scripts moved.
6. Web UI.
7. Old routes and page deleted. Release `— BREAKING`.

**Moving the installed wall** (once, at the end of step 3): store the new row image on the old master and let the present rollout install it; the row then shows its fallback. OTA the master. Pair the row with the `pair` action (by curl until the new page has Wall settings). If the new row image fails, it is replaced by a direct upload to the row's `POST /firmware`, which works in normal and in rescue mode and needs no master.

## 8. Kept on purpose

Phase timings and contact-age degrade; rollout attempt cap, rescue accounting and forgiveness; the upload gate (`OtaUploadGate.h`) and the pre-inrush OTA confirm; the packed ESP-01 image and its size guard; TX ladder; quiet policy; render stagger; mDNS discovery for finding boards to pair; everything under the unit bus (`UnitBusCore.h`, twiboot, reflash plan, boot integrity); the reflash producer gate and the twiboot probe-inhibit.

## 9. Measured (#560, on the live ESP-01 row, 2026-10-05)

| Question | Result | Consequence |
|---|---|---|
| Free memory with the link open, during a one-unit update | Low-water 17.5 KB at one frame per 5 s, against 18.3 KB without the link. With 5 frames per second it fell to 11.0 KB: about 80 B per frame left unread while the row is busy. | The link itself is affordable. The pacing rule in section 3 is required. |
| How long a row does not read its socket | 12.4 s during a one-unit update, about 5 s during homing | Nothing on the link may assume a quick answer from a row with a job running. |
| Web stack, like-for-like reference builds with the three routes the row keeps (flash / fixed memory over a bare WiFi sketch) | in-core `ESP8266WebServer` +31.4 KB / +0.4 KB; async server +30.2 KB / +1.4 KB; async server with its WiFi manager (today) +56.0 KB / +2.0 KB; hand-written on `WiFiServer` +15.3 KB / +0.1 KB; hand-written with setup form and DNS +21.9 KB / +0.2 KB; `ArduinoOTA` +42.8 KB / +0.5 KB | A hand-written server would free 34 KB and 1.8 KB of fixed memory, but neither is needed, and it would mean rewriting the setup portal and the upload route. The present stack stays. Separately, the present route handlers are 35.3 KB and the API index 7.6 KB, of a 468 KB image (ceiling 511 KB plain, 602 KB packed). |
| mDNS, SNTP | mDNS 20 KB, SNTP 1 KB | Dropping SNTP is a simplification, not a saving. mDNS stays unless flash runs short. |
| Checksum | SHA-256 would cost nothing extra today, but with signing gone the built-in MD5 is enough | MD5. |
| nanopb on the ESP-01 | about 11 KB of flash, no heap; the largest message the row must buffer is 72 B | Affordable; the row's receive buffer is tiny. |
| Round trip of a small frame, idle row, wired peer | median 10.5 ms, 90 % under 55 ms, 99 % under 307 ms, worst 772 ms (976 frames) | A single frame can be late against a 400 ms lead about once in a hundred: hence the 2 s lead for scheduled content. |
| Round-trip timing inside the connection | Lowest round trip of 25: spread 0.35 ms, worst 0.9 ms; of 8: spread 1.2 ms, worst 10 ms. Drift 15 ppm. | Kept as the fallback for flip timing; the first choice is the built-in time client pointed at the master. |
| Second heap (#564; same pass on the image before and after, same evening) | Free memory idle 28 to 29 KB against 23 to 25 KB; low-water after a self-test, a boot dump and a one-unit update 24.0 KB against 17.2 KB; second heap 15.6 KB free at rest, 12.4 KB with the unit facts in flight. Three fixed renders 3.8 / 3.8 / 8.9 s against 3.4 to 4.3 / 3.7 to 3.9 / 8.9 to 9.1 s; one-unit update 12.3 s against 12.1 and 12.3 s; boot dump 5.9 s against 5.7 s, same checksum; image download 21.9 s against 21.1 s; clock flips on time; no unit-bus error (one on the image without it); stack low-water equal; image +1.3 KB, fixed memory −4.1 KB. | Adopted. The smaller code cache costs nothing that shows on the wall. |

The peer in these runs was a wired machine on the LAN; with the S3 as the other end both ends are on WiFi, so round trips will be somewhat longer. The clock method does not depend on that.

## 10. Risks

- There is one ESP-01 and, since the spare S3 left, no bench board of either kind. Every trial runs on the mounted wall. A second ESP-01 with a few units would remove most of this risk.
- The master is tried on the live master, protected by A/B rollback and the rescue slot.
