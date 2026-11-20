# Note-size wall: the controller board — design

Issue: #675 (epic #676; unit board #674, flaps #670) · Status: role decided by Lucas 2026-11-20 (A1: one S3, mux, external box); layout and BOM follow

## Intent and why

The Note wall is 3 rows x 15 units in 623 x 219 x 101 mm. The unit board (#674)
loses its ESP-01S, so something else must own the I2C buses, WiFi, the web
operator page and the Home Assistant interface. Today that is an ESP32-S3
master (`firmware/v2/Master`) plus, per extra row, an ESP-01 row board
(`firmware/v2/FollowerEsp01`) on the wall link (`firmware/v2/link`, TCP 7411).
The role is filled by **one S3 controller for the whole wall (A1)**; the
board-per-row alternative (B) is kept below as the rejected option. It does not draw the board; layout and BOM follow in #675 or a
split-off issue.

## Goals

- One controller design that drives 45 bare-ATmega328P units over I2C at
  100 kHz, including twiboot reflash.
- Keep the Home Assistant contract (`docs/HA_CONTRACT.md`, append-only) and the
  operator API unchanged for the wall.
- A 12 V input with switched per-row eFuses; inrush handled by what exists
  (render stagger) plus the eFuses' soft start.
- Own the unit addressing: one CHAIN head per row and the enumeration job
  (#674 section 9), over a five-conductor row cable.

## Non-goals

- Unit board, motor, drum, flaps (#674, #671, #673, #670).
- The 64-flap alphabet (firmware project 4).
- Changing the deployed 5- and 16-unit walls or `PCB/v1/`.

## Constraints (facts from the repo)

- **Today:** the S3 master is the sole I2C toucher of its own row (`UnitBus.cpp`,
  one `Wire` bus, 100 kHz; 400 kHz was tried and reverted, #375/#383, for twiboot
  reflash and wall signal integrity). Other rows hang off ESP-01 row boards that
  dial the master over TCP 7411 (protobuf `Show`/`Op`/`Update`/`Config`...), take
  flip instants from the master's clock, and run their own bus, probe, twiboot
  reflash, rescue image and fallback clock. The row image is served by the
  master (`GET /firmware/row`).
- **Cluster/headless:** pairing, Grace (25 s) and LeaderLost (120 s) phases,
  succession tiers, rollout of the row image one row at a time, and the
  `wall_problem` / per-board verdicts all exist because boards are separate
  devices. A one-controller wall uses none of it.
- **Unit addressing:** 7-bit, `SFP_I2C_ADDRESS_BASE` = 1, set range 1..126
  (`SFP_CMD_SET_I2C_ADDRESS`), kept in EEPROM (the DIP switch goes, #674).
  Addresses are assigned by the controller in physical order over the chain line
  (#674 section 9): row r, position p gets the global index 15r + p, 1..45, so
  45 units fit one address space. The mux sits at 0x70 and 0x70..0x77 stays
  reserved; a blank unit listens on 0x29 only while gated.
- **Geometry:** 15 x 41.5 = 623 mm and 3 x 73 = 219 mm. **The units fill the
  whole footprint**; the unit envelope is ~95 mm deep in a 101 mm case, leaving
  ~6 mm behind them. There is no free area for a controller inside the Note
  outline, so the controller and the supply are an **external box** (decided).
- **Power:** ~4 W per moving unit today (#674); the 12 V bus feeds a buck per
  unit. Worst case all 45 moving is ~180 W (15 A at 12 V), 60 W (5 A) per row.
  The render stagger (`RenderStagger.h`: 4 units, then a 100 ms rail-settle on the
  S3; 1 unit per group on the ESP-01 rows) limits the start-up *spike*, not the
  steady draw while units are moving. How many units move at once on a full
  45-unit text change is not measured; the supply and fuse sizes below are
  starting points until it is (bench with the #306 vmin telemetry).
- **Levels:** the S3 and ESP-01 are 3.3 V; v1 level-shifts to the 5 V Nano. #674
  keeps the unit MCU and the bus at 5 V, so the 3.3 V to 5 V shift, the pull-ups
  and the CHAIN head buffer all live on the controller (decided with #674).

## I2C budget (both options)

Capacitance per unit (ATmega pin + SOT/connector + stub) is estimated at
~15-20 pF; FFC/ribbon is ~50 pF/m. A 15-unit row of 623 mm is ~250-330 pF; 45
units on one unsegmented bus are ~750-1000 pF. The standard-mode limit is
400 pF with a 1 us rise time, giving Rp(max) ~ 1000 ns / (0.85 x C) ~ 3.5 kohm at
340 pF. Consequences:

- **One row (15 units) fits a single 100 kHz segment** with 2.2-3.3 kohm
  pull-ups at the controller end. Margin is thin once the hall/tail wiring and a
  cable to an external box are added; the number must be measured on a
  prototype row, not trusted from this estimate.
- **45 units on one bus do not fit** without segmentation (buffers or a mux).
- Pull-ups live once per segment at the controller, not per unit; weak
  (>= 10 kohm) per-unit pull-ups at most, or none, to avoid the parallel
  resistance stacking up at 15 units.
- 400 kHz stays off (the #383 reasons are unchanged and the units are further
  apart).

## Options

### (A) One controller for the whole wall

One S3 board drives three row buses; the wall link, the ESP-01 rows and the
cluster layer are not used on this wall. Three ways to make the buses:

- **A1 (chosen), TCA9548A (or PCA9546A) mux, one channel per row.** Each
  channel is its own <=400 pF segment, so every row meets the budget alone, and
  a shorted SDA on one row isolates to that channel (the mux has a RESET input
  the S3 can pulse). Cost: 1 chip. Firmware: `UnitBus.cpp` selects the channel
  before each transaction (the shared twiboot client and `FlapFrame` code run
  unchanged on the adapter); the general-call broadcast and the bus rebuild
  (#207) become per channel; the bus facts/counters need a row index.
- **A2, one logical bus split by buffers (PCA9517A repeater per row).**
  Single address space, no channel select, general call reaches all 45 units,
  and it level-shifts 3.3 V to 5 V for free. Cost: 3 chips. Drawbacks: a fault
  on one segment can still hang the shared backbone; stacked buffer delay and
  the repeater's raised low level limit stacking; fewer tools to isolate one
  row.
- **A3, S3's two hardware I2C controllers plus one more.** Two rows on native
  ports and a third via a mux anyway, or bit-banged: not worth it. Dropped.

Chosen: **A1** (isolation matters more than broadcast reach; the 3.3/5 V shift
is a small separate stage, or a per-row PCA9517A *behind* the mux channel if the
level margin fails, see risks). A general-call `CHAIN_CLEAR` is sent per
channel.

What carries over: the whole Master firmware (display, operator page, API, MQTT
and HA discovery, OTA A/B with rollback, rescue, brownout/boot guards, unit
reflash over twiboot with the bundled hex). What changes: `UnitBus` learns
rows (3 x 15 addressed 1..45 through one channel map); `displayWidth` and the
text layout become 3 local rows of 15 (today the master's own board is one row
and `ClusterLayout.h` places other boards; the layout needs a local multi-row
case or the wall is modelled as one 45-wide line, which is wrong for a
3-line display, so the former); the unit count/probe caps (not verified here)
must reach 45. No wall link, no `FollowerEsp01` image, no rollout.

- **OTA/reflash of units:** unchanged mechanism (twiboot over I2C at 100 kHz,
  per channel, one unit at a time, the batch throttle and #412 halt). The
  mux makes a clean per-row reflash. Reflash time scales with 45 units on one
  master: serial by design (the bundle's page-write time x 45).
- **Firmware OTA:** one image, one board. Simpler than master + rows (no row
  image store, no `Update` offer): the #440 identity-by-content and the
  release path serve one tree.
- **Power entry (decided):** one 12 V 15 A supply (180 W, in the external box)
  feeding the controller board, which has three **switched per-row eFuses**
  (TPS25940-class: dV/dt soft start, 6.3 A class current limit, a fault flag per
  row). The firmware gets soft inrush, a remote power cycle of one row (the
  power-on catch, #554, today needs a hand) and a fault it can report; the head is
  held low whenever a row is off. A 12 V to 3.3 V buck feeds the S3 (peak ~0.4 A
  WiFi). The exact eFuse part is the layout issue's.
- **Inrush:** the render stagger already exists on the S3; with three rows on
  one controller the stagger can be wall-wide (a single schedule) rather than
  three uncoordinated row schedules, which is strictly better than today's
  rows. Boot: the confirm-pre-inrush rule (Master CLAUDE.md OTA) holds.
- **Board size and placement (decided: external box):** the controller is S3
  module (18 x 25.5 mm), mux, three eFuses, head buffers, connectors, ~60 x 40
  mm, in a box next to the 12 V supply (180 W cannot be inside the wall either).
  Three row cables run from the box to the first unit of each row. Each is a
  **five-conductor cable: +12 V, GND, SDA, SCL, CHAIN**, with the power pair gauged for the
  eFuse limit and SDA/SCL twisted with ground. Cable length adds capacitance to
  the budget above: keep it <= 0.5 m per row until measured.
- **Cost:** one S3 N16R8 module (~EUR 6-8) + mux + eFuses + passives
  ~EUR 15 per wall, plus the box and the supply. No row boards.
- **Failure isolation:** worse at the top (one board down = wall down; the
  deployed multi-board wall keeps working rows through headless mode), better
  inside (per-row channel and fuse). The controller is the only single point;
  it already has rescue image, A/B OTA rollback and the boot guards.

### (B) One controller per row (three boards, wall link kept)

Three row boards on the wall link; one of them (or a fourth) is the S3
master. This is the existing cluster model with the ESP-01 replaced by a new
board.

- **B1, three S3 boards** (each runs the Master firmware, one is the
  leader, two are members). The member role today is the ESP-01 firmware
  (ESP8266), not Master; there is no S3 member image. An S3 member would be a
  new firmware tree or a Master mode (large change) to avoid.
- **B2, new row board with a smaller ESP32 part** (C3/C6 class) running a port
  of FollowerEsp01 (Arduino-ESP32 instead of ESP8266: `ESPAsyncTCP`,
  `eagle.flash.1m.ld`, RTC rescue counters and the ESP8266 Update flow all
  change). Plus a leader S3 with its own row: 4 boards, 3 + 1.
- **I2C:** each board owns one 15-unit bus; no mux, no segmentation: the
  simplest electrical case and the best signal integrity at 100 kHz and
  twiboot. Pull-ups at each board.
- **Firmware:** carries over the entire wall link, cluster phases, rollout, row
  image store, headless succession and rescue. Changes: port from ESP8266 to an
  ESP32 family (large: the follower CLAUDE.md is ~19 KB of mechanisms), plus the
  unit-side hardware abstraction. The ESP-01 constraints that shaped the code
  (1 MB flash, 41 KB static RAM, gzip ceilings) go away on an ESP32, but only
  after the port.
- **OTA/reflash:** per row, parallel across rows, row image rolled one row at a
  time as today; unit twiboot per row board, exactly as the ESP-01 does now.
- **Power entry:** one 12 V per board (three entries or one bus with three
  taps); fuse per board comes naturally. 3 x the 3.3 V bucks.
- **Inrush:** three independent stagger schedules (the ESP-01 uses 1 unit per
  group because 3-4 unit rollovers killed its bus, #490); only the master's
  `commit_at_ms` aligns *flips*, not the starts of motion. Peak rail current is
  the sum of three uncoordinated rows.
- **Board size and placement:** three small boards (~40 x 25 mm) are easy to
  place per row, but there is **no free area in the footprint** either; each
  would sit in the same ~6 mm strip or the wall grows. Three modules plus
  one supply cost three times the connectors.
- **Cost:** 3 modules (+1 leader) + 3 regulators + 3 x connectors, ~EUR 25-35
  per wall, plus the port effort (the dominant cost).
- **Failure isolation:** best at the top (a dead row board loses 15 units; the
  others show their text or the fallback clock; headless mode works). But
  three more WiFi stations, DHCP leases, pairings and a link to keep healthy
  on a 623 mm wall that is one physical object.

## Unit addressing and the CHAIN heads

The controller owns enumeration (sequence, events and the unit-side gate are in
`2026-11-20-note-unit-board-design.md` section 9):

- **One CHAIN head per row**, a GPIO through a 5 V buffer (a 74AHCT1G-class gate;
  the ATmega's VIH at 5 V is 3.0 V, a 3.3 V drive has little margin) to the row cable's
  CHAIN conductor, which is the first unit's CHAIN_IN. High while the row is
  powered and settled, low when the row is off or being re-enumerated.
- The enumeration job per row: head low, three general-call `CHAIN_CLEAR`, head
  high, then probe 0x29 and `SET_I2C_ADDRESS` unit by unit in physical order with a
  verified ping each, on that row's mux channel. It runs automatically on a blank
  row and on a single swapped unit (one gap in the scan plus one listener on
  0x29), otherwise only on the operator action "enumerate row".
- Reports per row: `chain_break_after`, `row_long`, `row_enumeration_needed`
  (names provisional; they extend the operator API, and any HA-visible attribute
  follows `docs/HA_CONTRACT.md`: append-only, a hass-config issue first).
- Reflash is unchanged: twiboot at each unit's own address, one unit at a time.

## GPIO budget (S3)

| GPIO | Use |
|------|-----|
| 8, 9 | SDA, SCL to the mux (the pins `UnitBus.cpp` uses today) |
| 10, 11, 12 | CHAIN head, rows 0..2 |
| 13 | mux RESET |
| 14, 15, 16 | eFuse enable, rows 0..2 (pull-downs: rows off until firmware turns them on) |
| 17, 18, 21 | eFuse fault, rows 0..2 |

Never assigned: 35/36/37 (octal PSRAM), 4 (factory reset), 19/20 (USB), 48
(status LED). Also kept free on principle: 0, 3, 45, 46 (strapping), 26..34
(module flash and PSRAM lines), 39..42 (JTAG) and 43/44 (console). The ten pins
above leave nothing else claimed; verify the list against the module datasheet in
the layout issue.

## Comparison

| | A1 (one S3, mux) | B (board per row) |
|---|---|---|
| Firmware change | UnitBus rows + local multi-row layout | port FollowerEsp01 to ESP32 (or S3 member mode) |
| Wall link / cluster / rollout | unused | kept, required |
| I2C per segment | 15 units behind a mux | 15 units native |
| Unit reflash path | existing twiboot, per channel | existing, per row board |
| Boards / WiFi stations | 1 | 3-4 |
| Per-row fuse | yes, on the board | yes, per board |
| Inrush coordination | one wall-wide stagger | three uncoordinated |
| Fits the 101 mm depth | external box (decided) | strip x3 or wall grows |
| Failure isolation | wall-level SPOF, row-level channel/fuse | row-level |
| Cost per wall | ~EUR 15 | ~EUR 30 + a firmware port |

## Decision

**(A1), decided by Lucas 2026-11-20: one S3 controller for the whole wall, a
TCA9548A-class mux giving each row its own bus, the controller and a 12 V 15 A
supply in an external box, switched per-row eFuses, one CHAIN head per row.**

- It reuses the shipped Master firmware, which already has the hard parts
  (OTA with rollback, rescue, boot and brownout guards, twiboot, HA contract).
  The only new firmware is row-indexed bus plumbing, the enumeration job and a
  local multi-row layout, all small next to porting the follower.
- It removes the ESP-01 wall link from this wall: one board, one WiFi station, no
  pairing or rollout, one release image.
- A per-row channel and eFuse contain a shorted bus or a locked-up row; the mux
  RESET and the switched eFuses give recoveries that do not exist today.
- Wall-level inrush gets coordinated, which B cannot do.
- The loss (one board = wall down) is the existing S3 risk profile for a master,
  mitigated by the rescue image and A/B rollback.

B would be the call only if the wall had to stay up when one board dies, or if the
rows were separable modules; neither holds for the Note.

## Risks and what to measure first

1. **Bus capacitance and rise time** of one 15-unit row on real unit boards and
   the controller cable: measure on the first #674 prototypes with a scope;
   decide pull-up value then. If the cable pushes a row past ~400 pF, move the
   mux next to the row (the controller moves out of the box and into the wall) or add a PCA9517A.
2. **3.3 V bus against 5 V ATmega inputs:** VIH(5 V) = 3.0 V against a 3.3 V
   drive is little margin. Decided with #674: the pull-ups run to 5 V behind a
   level shifter on the controller (BSS138 pair or PCA9517A, one per row); the
   unit board carries no pull-ups.
3. **Moving-unit count and 12 V rail sag** for a 45-unit change: size the
   supply and fuses (60 W per row, 6.3 A) from #306 vmin telemetry on a
   row prototype, not from the 4 W figure.
4. **Unit-count ceilings** in the Master (probe/health arrays, snapshot sizes,
   HA unit counts) must be audited for 45; not verified here.
5. **GPIOs:** see the budget above; never 35/36/37/4/19/20/48.
6. **CHAIN over the cable:** the level and the debounce of a slow DC line beside
   100 kHz SDA/SCL on a ~0.5 m cable, with the head buffer; check on the first row
   prototype with the unit's 1 k series and 100 k pull-down.

## Out of scope for the choice (decided later)

PCB outline and connector choice, the exact eFuse part, a status LED and USB
for the first flash (rescue never USB-uploaded, S3 hard rules), the head buffer
part, and the box.

## Open questions for Lucas

1. **Box and cables:** where the box sits relative to the wall and the longest row
   cable it implies (the budget assumes <= 0.5 m; a longer run moves the mux next
   to the row or adds a PCA9517A).
2. **Supply headroom:** 15 A (180 W) is the worst case for all 45 moving; confirm
   or shrink it after the #306 vmin measurement on a row prototype.
