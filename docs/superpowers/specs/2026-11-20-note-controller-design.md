# Note-size wall: the controller board — design

Issue: #675 (epic #676; unit board #674, flaps #670) · Status: role decided by Lucas 2026-11-20 (A1: one S3, row mux, per-row eFuse); round-2 decisions folded in; **power input (mains vs USB-C PD) compared, recommendation below, Lucas decides**; layout and BOM follow

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
- A 12 V input with switched per-row eFuses and per-row power telemetry; inrush
  handled by what exists (render stagger) plus the eFuses' soft start.
- A robust row bus: each row on its own mux channel, with a rise-time
  accelerator, a mux reset and a per-row recovery ladder (section "Row bus
  robustness").
- Bench basics on the board: USB-C for data and flashing, BOOT/RESET, a factory
  reset button (GPIO 4), an RTC with a battery.
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
  outline; the controller stays an **external box** until Lucas decides the power
  input (section "Power input"), which also decides whether a supply fits inside.
  Behind the mechanism the drum spec leaves ~40 mm per unit (z 54..94 of 95 mm,
  `2026-11-20-note-drum-frame-design.md` section 7), but the unit boards live there.
- **Power:** two estimates exist and differ. The unit board spec (#674 section 8) has
  ~0.2 A / 2.4 W per moving 24BYJ48 unit, so a row of 15 is ~3 A / ~36 W and the
  wall ~9 A / ~108 W; the earlier ~4 W per unit figure gives ~180 W (15 A), 60 W
  (5 A) per row. Both are estimates; ~180 W stays the design ceiling until the #306
  vmin telemetry measures a row prototype. The 12 V bus feeds a buck per unit; idle
  is ~0.2 W per unit (estimate), ~10 W for the wall with the controller.
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
- **Power entry:** the controller takes **12 V DC** and has three **switched per-row
  eFuses** (TPS25940-class; this spec has not checked that family's current range,
  and a 6.3 A limit may exceed it: verify in the layout issue, or pick a part that
  holds ~4-6 A with a dV/dt soft start and a fault flag per row). The firmware gets
  soft inrush, a remote power cycle of one row (the power-on catch, #554, today needs
  a hand) and a fault it can report; the head is held low whenever a row is off. A
  12 V to 3.3 V buck feeds the S3 (peak ~0.4 A WiFi). Where the 12 V comes from
  (mains module or USB-C PD) is the section "Power input"; either way the controller
  sees the same 12 V rail, so the board does not change with that decision.
- **Inrush:** the render stagger already exists on the S3; with three rows on
  one controller the stagger can be wall-wide (a single schedule) rather than
  three uncoordinated row schedules, which is strictly better than today's
  rows. Boot: the confirm-pre-inrush rule (Master CLAUDE.md OTA) holds.
- **Board size and placement (external box by default):** the controller is S3
  module (18 x 25.5 mm), mux, three eFuses, three INA226 shunts, head buffers,
  USB-C, RTC, connectors, ~70 x 50 mm (estimate), in a box next to the 12 V supply
  unless the power decision puts both inside.
  Three row cables run from the box to the first unit of each row. Each is a
  **five-conductor cable: +12 V, GND, SDA, SCL, CHAIN**, with the power pair gauged for the
  eFuse limit and SDA/SCL twisted with ground. Cable length adds capacitance to
  the budget above: keep it <= 0.5 m per row until measured.
- **Cost:** one S3 N16R8 module (~EUR 6-8) + mux + eFuses + INA226s + accelerators
  + passives ~EUR 20-25 per wall (estimate), plus the box and the supply. No row
  boards.
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

## Network

The S3's own WiFi is the only network. No Ethernet (decided): PoE cannot carry the
wall, since 802.3bt delivers at most ~71 W at the device (against ~108-180 W), and a
second power path would add a board and a connector for nothing. The PCB antenna
(or the module's) must not sit under a metal PSU or box lid; keep the S3's antenna
end clear by >= 10 mm (a layout rule, estimate).

## Row bus robustness

Each row is its own TCA9548A-class mux channel (A1). On top of it:

- **Rise-time accelerator per row**, on the row side of the channel (an LTC4311 or
  TCA4311A-class active pull-up; real part families, not price- or stock-checked
  here). It lets a 330-400 pF row at 100 kHz keep a clean 5 V edge with a weaker
  static pull-up. The 3.3 V to 5 V shift stays a separate stage (BSS138 pair or
  PCA9517A, risk 2), upstream of the mux or per channel; pick one at layout.
- **Series 100 ohm-class resistors and a TVS** on each row's SDA/SCL/CHAIN at the
  controller connector, mirroring the unit side (#674 round 2).
- **Recovery ladder per row**, escalating, all existing or new GPIO-only:
  1. UnitBus rebuild (#207): select the channel, clock SCL up to 9 times from the
     controller's own SDA/SCL pins (the pins reach the row through the selected
     channel), send STOP.
  2. Pulse the mux RESET (GPIO 13): drops all channels, clears a mux that hung.
  3. Power-cycle that row through its eFuse (GPIO 14/15/16), with the CHAIN head low
     meanwhile; the row re-enumerates only if a unit lost its address (#674
     section 9). The eFuse fault flag and the INA226 reading say why.
  4. Report `row_bus_stuck` and leave the row off; the other two rows keep running.
- A row never blocks another: the mux deselects a channel between transactions, so
  a shorted SDA on one row cannot hold the shared upstream pair.

## Per-row power telemetry

Chosen: **one INA226-class current/power monitor per row**, high-side, on the
eFuse output, with a ~5 mohm shunt (estimate: 6 A x 5 mohm = 30 mV, 0.18 W; check
the datasheet full-scale range of the part in the layout issue). Three devices at
distinct addresses (A0/A1 straps) on a **local I2C bus** (S3 second controller,
GPIO 1/2) shared with the RTC, so telemetry never rides the row mux; their ALERT
pins wire-OR into one GPIO (5).

| | INA226 per row | eFuse with an analog current-monitor pin |
|---|---|---|
| Output | current, bus voltage and power over I2C; 16-bit | an analog voltage, needs one ADC pin per row and a calibration |
| Rail sag (#306 vmin) | read per row, same chip | not available |
| Accuracy at ~0.5 A | good (TI lists 0.1 % max gain error, 10 uV max offset) | typically a few % at low current (estimate, part-dependent) |
| Pins | 2 shared + 1 alert | 3 ADC pins (the ADC1 pins are also the free pool) |
| Cost | ~EUR 1.3 at 1k (Octopart, TI part) up to ~EUR 4-5 single (RS, estimate), x3 | free with the eFuse, if the part has it |
| Extra board loss | ~0.18 W per row at 6 A | none |

It gives the firmware a wall-wide, measured stagger (hold new moves until the
measured draw falls under a budget) and the vmin evidence the supply sizing needs.
New telemetry attributes are append-only additions to the HA contract and need a
hass-config issue first (not done here).

## Bench and service basics

- **USB-C, data only:** native S3 USB on GPIO 19/20 for first flash, console and the
  USB serial/JTAG; 5.1 kohm on both CC pins (sink, no power negotiation), ESD
  protection on D+/D-. Rescue stays never USB-uploaded (S3 hard rules); USB is
  the first flash and a bench console. VBUS is diode-OR'ed into the 3.3 V buck input
  only (the buck must accept 5 V in, to verify), so the board can be flashed
  without the 12 V rail; the eFuse rows stay off on USB power.
- **BOOT** (GPIO 0, strapping, button to ground only) and **RESET** (EN, button plus
  RC).
- **Factory reset: GPIO 4**, active-low button, reserved and never used for anything
  else (same meaning as the existing factory-reset rule).
- **RTC with a battery** on the local I2C bus (PCF8563 or DS3231-class, CR1220
  holder), SQW/INT on GPIO 6. It feeds the fallback clock when WiFi or NTP is down
  and keeps time across power loss (the S3's internal RTC does not survive a power
  cycle).
- A status LED (GPIO 48), a UART0 pad pair (43/44) and a JTAG pad row (39..42) stay
  as pads, not parts.

## GPIO budget (S3)

| GPIO | Use |
|------|-----|
| 0 | BOOT (strapping; button to ground only) |
| 1, 2 | local I2C: SDA, SCL (INA226 x3, RTC) |
| 4 | **factory reset button (reserved)** |
| 5 | INA226 ALERT, wire-OR of three |
| 6 | RTC SQW/INT |
| 8, 9 | SDA, SCL to the row mux (the pins `UnitBus.cpp` uses today) |
| 10, 11, 12 | CHAIN head, rows 0..2 |
| 13 | mux RESET |
| 14, 15, 16 | eFuse enable, rows 0..2 (pull-downs: rows off until firmware turns them on) |
| 17, 18, 21 | eFuse fault, rows 0..2 |
| 19, 20 | USB D-, D+ (USB-C data, flashing) |
| 48 | status LED |

21 pins claimed. Free spares: 3 (strapping, keep free), 7, 38, 47. Never assigned:
35/36/37 (octal PSRAM). Kept free on principle: 45, 46 (strapping), 26..34 (module
flash and PSRAM lines), 39..42 (JTAG pads) and 43/44 (console pads). EN is the RESET
button. Verify the list against the module datasheet in the layout issue; GPIO 1/2
and 5..7 are chosen because they are not strapping or PSRAM pins on the N16R8.

## Power input: mains module (A) vs USB-C PD (B)

Lucas's two inputs (2026-11-20): first USB-C PD, then mains like the Vestaboard
Note, with this comparison to settle it. The controller side is the same either
way (12 V in, section "Options" A1), so the question is only what makes the 12 V.

**Load to size for.** ~108 W worst case (9 A, #674 section 8 estimate) plus ~5 W
for the controller, or ~180 W (15 A) with the older figure; idle ~10 W. Units move
for seconds, so average heat is small; the supply is sized for the peak.

### (A) Internal mains, as the Vestaboard Note

Facts about the reference: the Note is 24.5 x 8.6 x 3.9 in (623 x 219 x 101 mm),
takes 100-240 V on a standard 3-prong plug and ships a 50 cm IEC C5 cord; Vestaboard
states 73 W when updating (installation guide) and 121 W (power help page), which
disagree (the latter may be the original Vestaboard). No teardown of its supply was
found, so how it packs the PSU is unknown here.

Module class (enclosed, certified, no mains on our PCBs), real dimensions from
distributor listings:

| Module | Rating | L x W x H (mm) | Source note |
|---|---|---|---|
| Mean Well LRS-200-12 | 12 V, 17 A, 204 W, enclosed (perforated metal), convection | 215 x 115 x 30, 660 g | ~USD 26-27 single (US distributors); GBP 30.56 ex VAT (RS UK) |
| Mean Well IRM-90-12 | 12 V, 6.7 A, 80 W, encapsulated, PCB module | 87 x 52 x 29.5 | PCB type (screw-terminal IRM-90-12ST is 109 x 52 x 33.5); price not found |
| TRACO TPP 180-112-M | 12 V, 15 A, 180 W | 76.2 x 50.8 x 34.6 | RS UK listing; price and derating (convection vs airflow at 180 W) not checked |
| TDK-Lambda NVM1-1T000-S1-C | 12 V, 15 A, 180 W, enclosed | 127 x 84.4 x 40.6 | listing; inlet not stated |

Mains side, one per wall: IEC C14 inlet module with fuse drawer and filter (class
I, earth to the PSU chassis and to a metal frame member), fuse sized to the PSU's
input rating (a slow-blow T-type, per the module datasheet), strain relief, mains
terminals behind a fixed cover, creepage kept to the PSU's own isolation. The
LRS-class unit's input terminals are exposed, so the cover is ours to make.

**Does it fit the 623 x 219 x 101 footprint?** Not as drawn. The frame is ~95 mm
deep, leaving 6 mm to the case, and the ~40 mm behind the mechanism (z ~54..94) is
split into per-unit frame shells with the unit board on the back face. Every module
above is 29.5-40.6 mm thick, which is the whole remaining 40 mm minus the unit
board's own parts (buck inductor, connector, ULN2003: ~10 mm, estimate), so a
30 mm module clears them by ~0 mm (estimate) and also needs the frame rear walls
of the ~2 x 3 units it covers cut away. Three ways out:

1. **Deeper rear bay:** add ~35 mm to the case (101 to ~136 mm, estimate: 30 mm
   module + 5 mm for air and the cover) behind the unit boards. Frames untouched;
   the wall is 35 % deeper than a Note.
2. **Shared rear chamber:** end the unit frames at the mechanism (z ~55) and make
   z 55..94 one 623 x 219 chamber with the boards on a row backplane. The module
   still needs ~30 mm over the boards: ~0 to 1 mm clearance, mains and ELV in one
   volume. Not recommended.
3. **External mains brick with C14 (the earlier A1 box):** an enclosed 12 V brick
   (RS lists 180 W / 12 V / 15 A adapters with a C14 inlet) beside the controller
   box; the wall carries only 12 V. Fits trivially; two cables, so less Note-like.

Heat (estimates): at ~88-90 % efficiency 108 W costs ~12-14 W in the module, for the
seconds a full-wall change lasts; at idle ~10 W load is ~1-2 W of loss. A
convection module in a sealed plastic case behind a 41-mm-pitch wall needs vents
or a derating check against the module's datasheet curve (not fetched here).

### (B) USB-C PD

Facts: SPR is up to 20 V at 5 A = 100 W; EPR adds 28 V (140 W), 36 V (180 W) and
48 V (240 W) at 5 A, and needs an e-marked 5 A EPR cable (certified cables are on
the market at about USD 16-28 for 1-3 m; marketplace 240 W GaN chargers list at
~USD 129-145). Sink controllers: **AP33772S** (Diodes, I2C, EPR/AVS to 28 V, SPR/PPS to
21 V, 4 x 4 mm QFN-24, price not found) stops at 28 V = 140 W; the full 48 V /
240 W needs a controller such as **TPS26750** (TI, full EPR, with a reference design
pairing it with a BQ25756 charger), a larger firmware/config job. Then a buck to 12 V
of at least 9-15 A on our own board: 28 V in at 140 W needs a ~12 A-class
synchronous buck with an inductor, FETs and an EMC-clean layout (a new design, not a
part pick; ~EUR 10-20 estimate).

**The stagger cap a lower level forces** (estimates; 12 V buck efficiency 92 %,
controller 5 W; moving-unit power per #674 or the older figure):

| Level | Power | Wall at ~2.4 W per unit (108 W) | Wall at ~4 W per unit (180 W) |
|---|---|---|---|
| SPR 20 V | 100 W | ~87 W for units: ~36 of 45 moving (80 %) | ~22 of 45 (49 %) |
| EPR 28 V | 140 W | all 45 (no cap) | ~32 of 45 (71 %) |
| EPR 36 V | 180 W | all 45 | ~42 of 45 |
| EPR 48 V | 240 W | all 45 | all 45 |

Also: the source can drop the contract on over-current and the whole wall resets
(power-on catch, #554), so the firmware needs a closed-loop stagger on the
INA226 total, not just a fixed cap.

### Side by side

| | (A) internal mains module | (B) USB-C PD |
|---|---|---|
| Function | any load to the module rating, no cap; ~12 V, 17 A (LRS-200-12) | capped by the negotiated level (table above); a drop resets the wall |
| User experience | one IEC cord like the Note | one USB-C cable, but must be an EPR charger and e-marked cable; wrong charger silently caps or fails |
| Safety / compliance | class I mains product: earth, fused inlet, cover, creepage; CE LVD + EMC if sold; our PCBs stay ELV | no mains in the product (the source carries it); 28-48 V is above 12 V but below 75 V DC (LVD scope as I read it, verify); our PCBs are ELV; EMC of the new buck |
| Space / heat | 30-40 mm thick module: does not fit 101 mm without a deeper rear bay (~+35 mm) or an external brick; ~12-14 W loss at peak | ~9 W buck loss at peak on our PCB (estimate); source is outside the wall |
| BOM cost (estimates) | module ~EUR 25-30 + C14 filter inlet with fuse ~EUR 6-10 + cord/earth/cover ~EUR 5: **~EUR 40-50** | AP33772S ~EUR 1-2 + 12 A-class buck stage ~EUR 10-20 + EPR cable ~EUR 15-25 + EPR charger ~EUR 60-130 (240 W seen at USD 129-145): **~EUR 90-180**, more with a 48 V controller |
| Availability | Mean Well parts widely stocked (LRS-200-12 710 pcs at one US distributor; a 19-week factory lead time quoted there); IRM-90 and TRACO stock not checked | AP33772S and certified cables available; 140 W+ EPR chargers are fewer and marketplace-priced; TPS26750 a newer part |
| Design effort | choose and mount; mechanical bay | new 12 A-class buck, PD firmware, EPR cable policy |

### Recommendation (Lucas decides)

**(A), a mains module, with the PSU outside the wall first.** Numbers: ~EUR 40-50
against ~EUR 90-180, no stagger cap forced by the source, no new power stage to
design or EMC-test, and the stock part is proven (Mean Well LRS-200-12, 215 x 115 x
30 mm, 204 W, ~USD 26). The one thing against it is space: no module fits the 101 mm
case as drawn. So build the first wall with an **enclosed 12 V brick with a C14
inlet next to the controller box** (identical electronics, zero mains in the wall,
the wall is ELV), and take the internal Note-like variant only if Lucas accepts a
rear bay of ~+35 mm (or a bay that shrinks once the unit frames are measured);
the controller board is the same in both. Choose (B) only if the wall must have no
mains at all (an ELV-only product, or sold without a mains conformity assessment);
then use the 28 V EPR level with the AP33772S, a 12 A-class buck and a closed-loop
stagger, accepting the 140 W ceiling.

Because this repo is public, a mains design carries a duty to say so: publish the
mains side as "use a certified module and a fused, earthed inlet; no mains on
the PCBs", never as a schematic to copy.

### Sources

Vestaboard Note power, dimensions and cord: <https://www.vestaboard.com/help/note-power>, <https://www.vestaboard.com/help/dimensions-note>, <https://www.vestaboard.com/help/power>. Mean Well LRS-200-12 price and size: <https://www.bravoelectro.com/lrs-200-12.html>, <https://www.masterelectronics.com/en/mean-well/lrs20012-47466823.html>, <https://uk.rs-online.com/web/p/switching-power-supplies/1065859>. IRM-90-12: <https://www.digikey.com.br/en/products/detail/mean-well-usa-inc/IRM-90-12/11562725>, <https://meanwell.com/newsInfo.aspx?c=1&i=830>. 180 W modules: <https://uk.rs-online.com/web/p/switching-power-supplies/2499835?gb=s>, <https://int.rsdelivers.com/product/tdk-lambda/nvm1-1t000-s1-c/7511223>, <https://uk.rs-online.com/web/p/ac-dc-adapters/2707384>. AP33772S: <https://www.diodes.com/assets/Datasheets/AP33772S.pdf>. TPS26750: <https://ti.com/product/TPS26750>. EPR cables and chargers: <https://cdw.com/product/startech.com-6ft-usb-c-charging-cable-usb-if-certified-usb-c-cable-240w-p/7847865>, <https://www.newegg.com/p/3C6-005Y-00YB2>. INA226: <https://www.ti.com/product/ina226>, <https://octopart.com/part/texas-instruments/INA226AIDGSR>. Distributor pages change; prices are single-unit snapshots, not quotes.

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
| Fits the 101 mm depth | external box by default; a PSU inside needs ~+35 mm | strip x3 or wall grows |
| Failure isolation | wall-level SPOF, row-level channel/fuse | row-level |
| Cost per wall | ~EUR 15 | ~EUR 30 + a firmware port |

## Decision

**(A1), decided by Lucas 2026-11-20: one S3 controller for the whole wall on its
own WiFi (no Ethernet), a TCA9548A-class mux giving each row its own bus with a
rise-time accelerator and a recovery ladder, switched per-row eFuses with an INA226
per row, one CHAIN head per row, USB-C for data and flashing, BOOT/RESET, a factory
reset on GPIO 4 and an RTC with a battery.** The 12 V source is open: section
"Power input" recommends a mains module, outside the wall first; Lucas decides.

- It reuses the shipped Master firmware, which already has the hard parts
  (OTA with rollback, rescue, boot and brownout guards, twiboot, HA contract).
  The only new firmware is row-indexed bus plumbing, the enumeration job, the
  INA226 telemetry and a local multi-row layout, all small next to porting the
  follower.
- It removes the ESP-01 wall link from this wall: one board, one WiFi station, no
  pairing or rollout, one release image.
- A per-row channel and eFuse contain a shorted bus or a locked-up row; the mux
  RESET, the recovery ladder and the switched eFuses give recoveries that do not
  exist today.
- Wall-level inrush gets coordinated, which B cannot do.
- The loss (one board = wall down) is the existing S3 risk profile for a master,
  mitigated by the rescue image and A/B rollback.

B (board per row) would be the call only if the wall had to stay up when one board
dies, or if the rows were separable modules; neither holds for the Note.

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
   supply and fuses (36-60 W per row, 3-5 A, against the 6.3 A eFuse figure) from the
   INA226 and #306 vmin readings on a row prototype, not from the 2.4 W or 4 W
   figures.
4. **Unit-count ceilings** in the Master (probe/health arrays, snapshot sizes,
   HA unit counts) must be audited for 45; not verified here.
5. **GPIOs:** see the budget above; 4 is reserved for factory reset, 35/36/37 never.
5a. **Power input:** the open decision in the section above; the eFuse family's
   current range and the rear bay depth are unverified.
6. **CHAIN over the cable:** the level and the debounce of a slow DC line beside
   100 kHz SDA/SCL on a ~0.5 m cable, with the head buffer; check on the first row
   prototype with the unit's 1 k series and 100 k pull-down.

## Out of scope for the choice (decided later)

PCB outline and connector choice, the exact eFuse, accelerator, level-shift and
RTC parts, the head buffer part, the mains module and inlet (or the PD level), and
the box.

## Open questions for Lucas

1. **Power input:** mains module (recommended; external brick first) or USB-C PD
   (ELV-only, 140 W ceiling with the 28 V level)? If mains: accept a rear bay of
   ~+35 mm for an internal module, or keep the PSU outside?
2. **Box and cables:** where the box sits relative to the wall and the longest row
   cable it implies (the budget assumes <= 0.5 m; a longer run moves the mux next
   to the row or adds a PCA9517A).
3. **Supply headroom:** 108 W (#674 estimate) or 180 W (older estimate)? Decides a
   120 W or a 200 W module; confirm after the INA226/#306 measurement on a row
   prototype.
4. **Telemetry in HA:** expose per-row power as HA sensors (an append-only contract
   addition and a hass-config issue first), or keep it in the operator API only?
