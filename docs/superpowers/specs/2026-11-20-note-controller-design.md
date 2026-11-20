# Note-size wall: the controller board — design

Issue: #675 (epic #676; unit board #674, flaps #670) · Status: role decided by Lucas 2026-11-20 (A1: one S3, row mux, per-row eFuse); round-2 and round-3 decisions folded in (slot-coded backplane replaces the chain; second I2C bus allowed); **power input: internal mains is the target, the side-by-side with USB-C PD stays below for Lucas to decide**; layout and BOM follow

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
- A 12 V input with switched per-row eFuses, per-row power telemetry and a
  wall-total monitor; inrush
  handled by what exists (render stagger) plus the eFuses' soft start.
- A robust row bus: each row on its own mux channel, an accelerator footprint
  (not populated until measured), a mux reset and a per-row recovery ladder
  (section "Row bus robustness").
- Bench basics on the board: USB-C for data and flashing, BOOT/RESET, a factory
  reset button (GPIO 4), recessed BOOT/RESET, a row service mode, a DS3231-class
  RTC with a battery.
- No addressing job: units take their address from slot-coded backplane pins
  (#674), so there is no CHAIN head, no enumeration and no `CHAIN_CLEAR`; the
  row cable is four conductors (+12 V, GND, SDA, SCL).
- Two I2C buses with one owner each: the unit bus (`Wire`, through the mux,
  `UnitBus.cpp`) and a system bus (`Wire1`: power monitors, RTC).

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
  (`SFP_CMD_SET_I2C_ADDRESS`), kept in EEPROM today. On the Note unit the address
  is read from the slot-coded backplane pins (each slot straps its position, like
  the old DIP) with a segment id from a solder jumper on the backplane (#674
  decides the exact coding). The controller assigns nothing and enumerates
  nothing. Whether addresses are global 1..45 or 1..15 per mux channel is a #674
  choice; either fits, since each row is its own channel. The mux sits at 0x70 and
  0x70..0x77 stays reserved.
- **Geometry:** 15 x 41.5 = 623 mm and 3 x 73 = 219 mm. **The units fill the
  whole footprint**; the unit envelope is ~95 mm deep in a 101 mm case, leaving
  ~6 mm behind them. There is no free area for a controller inside the Note
  outline; the controller sits in an **external box** by default. Internal mains
  is the power target (Lucas, 2026-11-20): a 26 mm UHP-200-12 can sit inside (section
  "Power input"), and the controller moves in with it only if that fit passes.
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
  keeps the unit MCU and the bus at 5 V, so the 3.3 V to 5 V shift and the
  pull-ups live on the controller (decided with #674).
- **Hard rule today:** `UnitBus.cpp` is the sole `Wire` toucher
  (`.claude/rules/s3-hard-rules.md`). Decided 2026-11-20: a second I2C bus on the
  controller is allowed with one owner per bus; `UnitBus.cpp` keeps the unit bus, a
  new system-bus file owns `Wire1`. The rule is amended with the firmware work, not
  by this spec.

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

Chosen: **A1** (isolation matters more than broadcast reach). The 3.3 V to 5 V
step is expected to come from the mux pass gates with 5 V pull-ups on the channel
side, with no per-row BSS138 pair; that is a datasheet **verify** item (section
"Row bus robustness"), and a PCA9517A behind the channel is the fallback.

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
  a hand) and a fault it can report. Anything on the row that could back-feed an
  unpowered row is switched with that row's eFuse (see "Row bus robustness"). A
  12 V to 3.3 V buck feeds the S3 (peak ~0.4 A WiFi). Where the 12 V comes from
  (mains module or USB-C PD) is the section "Power input"; either way the controller
  sees the same 12 V rail, so the board does not change with that decision.
- **Inrush:** the render stagger already exists on the S3; with three rows on
  one controller the stagger can be wall-wide (a single schedule) rather than
  three uncoordinated row schedules, which is strictly better than today's
  rows. Boot: the confirm-pre-inrush rule (Master CLAUDE.md OTA) holds.
- **Board size and placement (external box by default):** the controller is S3
  module (18 x 25.5 mm), mux, three eFuses, four INA226 shunts, 5 V pull-up switches,
  USB-C, RTC, connectors, ~70 x 50 mm (estimate), in a box next to the 12 V supply
  unless the power decision puts both inside.
  Three row cables run from the box to the first unit of each row. Each is a
  **four-conductor cable: +12 V, GND, SDA, SCL**, with the power pair gauged for the
  eFuse limit and SDA/SCL twisted with ground. The slot pins live on the row
  backplane, not in the cable (#674). Cable length adds capacitance to
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

## Unit addressing

The controller does not address units. Each backplane slot straps its position and a
solder jumper sets the segment id (#674); the unit reads both at boot. Consequences
for this board:

- **No CHAIN head per row**, no head buffer, no `CHAIN_CLEAR`, no enumeration
  job and no `chain_break_after` / `row_long` / `row_enumeration_needed` reports.
  A swapped unit needs nothing from the controller.
- The controller only has to probe the expected addresses per channel, as today,
  and keep 0x70..0x77 reserved for the mux.
- Reflash is unchanged: twiboot at each unit's own address, one unit at a time,
  at 100 kHz. Whether a blank bootloader unit answers on its slot address is a
  #674 / twiboot question, not a controller one.

## Two I2C buses, one owner each

| Bus | Pins | Devices | Owner (firmware) |
|---|---|---|---|
| Unit bus (`Wire`) | GPIO 8, 9 | TCA9548A mux (0x70), the units behind its three channels | `UnitBus.cpp`, sole toucher |
| System bus (`Wire1`) | GPIO 11, 12 | INA226 x4, DS3231-class RTC | a new system-bus file, sole toucher |

System devices never go on the unit bus or behind the mux: their fixed addresses
(INA226 0x40..0x4F, RTC 0x68) can collide with unit addresses, and telemetry must
keep working while a row channel is stuck. The hard rule in
`.claude/rules/s3-hard-rules.md` ("`UnitBus.cpp` sole Wire toucher") is amended to
"one owner per bus" together with the firmware work, not by this spec.

## Network

The S3's own WiFi is the only network. No Ethernet (decided): PoE cannot carry the
wall, since 802.3bt delivers at most ~71 W at the device (against ~108-180 W), and a
second power path would add a board and a connector for nothing. The PCB antenna
(or the module's) must not sit under a metal PSU or box lid; keep the S3's antenna
end clear by >= 10 mm (a layout rule, estimate).

## Row bus robustness

Each row is its own TCA9548A-class mux channel (A1). On top of it:

- **Level step and pull-ups.** The pull-ups on each channel's row side run to 5 V,
  and the mux pass gates are expected to translate between those and the 3.3 V
  upstream side, so no BSS138 pair per row. **Verify** in the TCA9548A datasheet
  before layout, including which VCC to run the mux at: a 5 V VCC raises its
  input-high threshold (a proportion of VCC) above a 3.3 V S3 drive, a 3.3 V VCC
  limits the channel-side high level. If it does not hold, a PCA9517A behind each
  channel is the fallback.
- **Switch the pull-up supply with the row eFuse.** The row's 5 V pull-ups (and any
  other slot-side supply the controller provides) are powered through a small switch
  that follows that row's eFuse state. Otherwise an unpowered row is back-fed
  through the unit pins' protection diodes from the pull-ups and the SDA/SCL
  levels, and units half-wake.
- **Accelerator footprint per channel, not populated by default**: an LTC4311 /
  TCA4311A-class active pull-up on the row side (real part families, not price- or
  stock-checked here). Populate it only if the far unit's rise time exceeds
  ~1 us; measure at 100 kHz, the twiboot rate, on a prototype row.
- **Series 100 ohm-class resistors and a TVS** on each row's SDA/SCL at the
  controller connector, mirroring the unit side (#674 round 2).
- **Recovery ladder per row**, escalating; a row never blocks another:
  1. Per-channel bus rebuild (#207): select the channel and re-initialise the
     bus on it.
  2. If SDA stays stuck low: clock SCL up to 9 times from the controller's own
     pins (they reach the row through the selected channel), then STOP.
  3. Pulse the mux RESET (GPIO 13): drops all channels and clears a hung mux.
  4. Row eFuse off for at least 1 s, then back on and let it settle at least 2 s
     before the first transaction. The eFuse fault flag and that row's INA226
     reading say why.
  5. Still failing: mark the row faulted (`row_bus_stuck`), leave it off; the
     other rows carry on.
- **Mux RESET is also pulsed once at boot**, before any channel is selected, so a
  mux left mid-transaction by a reset or brownout starts clean.
- The mux deselects a channel between transactions, so a shorted SDA on one row
  cannot hold the shared upstream pair.

## Power telemetry

Chosen: **one INA226-class monitor per row, high-side, after that row's eFuse, with a
10 mohm shunt, plus one on the 12 V total (before the eFuses) with a ~2 mohm shunt.**
Four devices at distinct addresses (A0/A1 straps) on the **system bus** (`Wire1`,
GPIO 11/12), shared with the RTC, so telemetry never rides the row mux; their ALERT
pins wire-OR into one GPIO (5).

- **Why two shunt values.** The INA226 shunt input is +-81.92 mV full scale (verify
  in the datasheet): a 10 mohm shunt overflows above ~8.2 A, so it suits one row
  (3-6 A) but not the wall total at 7-9.6 A (or 15 A on the older estimate). A
  2 mohm shunt reads to ~41 A at 2.5 uV per LSB. Shunt dissipation at the design
  currents (about 0.16-0.36 W per row shunt, ~0.2 W for the total at 9.6 A;
  estimates) sets the shunt package, a 1 W-class part.
- **The INA226 bus-voltage reading is the primary sag input** (the #306 vmin
  evidence and the wall-wide stagger). The unit's own rail field (#674 12 V sense)
  is the far-end check, so the difference between the two is the cable and
  backplane drop.
- **Analog only on ADC1** (GPIO 1..10); ADC2 is shared with WiFi. No analog input is
  assigned on this board today; the rule keeps the pins free for one later.

| | INA226 per row | eFuse with an analog current-monitor pin |
|---|---|---|
| Output | current, bus voltage and power over I2C; 16-bit | an analog voltage, needs one ADC1 pin per row and a calibration |
| Rail sag (#306 vmin) | read per row, same chip | not available |
| Accuracy at ~0.5 A | good (TI lists 0.1 % max gain error, 10 uV max offset) | typically a few % at low current (estimate, part-dependent) |
| Pins | 2 shared + 1 alert | 3 ADC1 pins |
| Cost | ~EUR 1.3 at 1k (Octopart, TI part) up to ~EUR 4-5 single (RS, estimate), x4 | free with the eFuse, if the part has it |
| Extra board loss | ~0.16-0.36 W per row shunt | none |

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
- **BOOT** (GPIO 0, strapping, button to ground only), **RESET** (EN, button plus
  RC) and the **factory reset** (GPIO 4, active-low, held 5 s, reserved and never used
  for anything else, same meaning as the existing factory-reset rule). All three
  are **recessed** (pinhole or sunk tactile switch) so a case contact cannot press
  them, and reachable from the back of the box or frame.
- **Row service mode:** an operator action per row that turns that row's eFuse off
  (and its pull-up switch with it) before units are swapped on the backplane, and
  turns it back on with the normal settle on exit. Unit swaps happen with the row
  dead, never hot. The firmware part is a later issue; the board needs only the
  per-row eFuse enable it already has.
- **DS3231-class RTC** (temperature-compensated, with a **CR1220 holder reachable
  from the back**, so the cell can be replaced without opening the wall) on the system
  bus, SQW/INT on GPIO 6. Firmware reads it at boot and sets the system clock from it
  until NTP answers, and writes it after each NTP sync. It feeds the fallback clock
  when WiFi or NTP is down and keeps time across power loss (the S3's internal RTC
  does not survive a power cycle).
- A status LED (GPIO 48), a UART0 pad pair (43/44) and a JTAG pad row (39..42) stay
  as pads, not parts.

## GPIO budget (S3)

| GPIO | Use |
|------|-----|
| 0 | BOOT (strapping; button to ground only) |
| 4 | **factory reset button (reserved)** |
| 5 | INA226 ALERT, wire-OR of four |
| 6 | RTC SQW/INT |
| 8, 9 | SDA, SCL of the unit bus (`Wire`) to the row mux (the pins `UnitBus.cpp` uses today) |
| 11, 12 | SDA, SCL of the system bus (`Wire1`: INA226 x4, RTC) |
| 13 | mux RESET |
| 14, 15, 16 | eFuse enable, rows 0..2 (pull-downs: rows off until firmware turns them on) |
| 17, 18, 21 | eFuse fault, rows 0..2 |
| 19, 20 | USB D-, D+ (USB-C data, flashing) |
| 48 | status LED |

18 pins claimed, 3 fewer than before the backplane decision (the three CHAIN heads on
10/11/12 are gone, and the system bus moved from GPIO 1/2 to 11/12 so the ADC1 pins
1, 2 and 10 stay free for analog). Free spares: 1, 2, 7, 10, 38, 47, plus 3
(strapping, keep free). Never assigned: 35/36/37 (octal PSRAM). Kept free on
principle: 45, 46 (strapping), 26..34 (module flash and PSRAM lines), 39..42 (JTAG
pads) and 43/44 (console pads). Analog uses ADC1 pins only (GPIO 1..10), never ADC2.
EN is the RESET button. Verify the list against the module datasheet in the layout
issue; GPIO 11/12 are not strapping or PSRAM pins on the N16R8, and the pull-ups for
`Wire1` go on those two.

## Power input: mains module (A) vs USB-C PD (B)

Lucas's two inputs (2026-11-20): first USB-C PD, then mains like the Vestaboard
Note. **Internal mains is the target** (latest decision on #676); this side-by-side
stays so Lucas can confirm it before layout. The controller side is the same either
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
| **Mean Well UHP-200-12** | 12 V, 16.7 A, 200.4 W, enclosed, fanless convection, 93 % typ. efficiency, PFC, -30..+70 C with derating | **194 x 55 x 26** | USD 49 single / 43.30 at 25 (Digi-Key US), GBP 37.60 ex VAT (Digi-Key UK), GBP 59.65 ex VAT (RS UK); RS calls it "Slim and Low profile (26 mm)" |
| Mean Well LRS-200-12 | 12 V, 17 A, 204 W, enclosed (perforated metal), convection | 215 x 115 x 30, 660 g | ~USD 26-27 single (US distributors); GBP 30.56 ex VAT (RS UK) |
| Mean Well IRM-90-12 | 12 V, 6.7 A, 80 W, encapsulated, PCB module | 87 x 52 x 29.5 | PCB type (screw-terminal IRM-90-12ST is 109 x 52 x 33.5); price not found |
| TRACO TPP 180-112-M | 12 V, 15 A, 180 W | 76.2 x 50.8 x 34.6 | RS UK listing; price and derating (convection vs airflow at 180 W) not checked |
| TDK-Lambda NVM1-1T000-S1-C | 12 V, 15 A, 180 W, enclosed | 127 x 84.4 x 40.6 | listing; inlet not stated |

Mains side, one per wall: IEC C14 inlet module with fuse drawer and filter (class
I, earth to the PSU chassis and to a metal frame member), fuse sized to the PSU's
input rating (a slow-blow T-type, per the module datasheet), strain relief, mains
terminals behind a fixed cover, creepage kept to the PSU's own isolation. The
LRS-class unit's input terminals are exposed, so the cover is ours to make.

**Does it fit the 623 x 219 x 101 mm footprint?** The LRS-200-12 and the other
modules above do not (29.5-40.6 mm thick against the ~40 mm rear space, with the
unit boards in it). **The UHP-200-12 can**, on paper: it is 26 mm thick and 55 mm wide.

Placement (estimates, to confirm against the #674 layout):

- **Bays:** 194 mm is 4.7 columns of 41.5 mm, so it lies along **one row over 5
  adjacent bays** (207.5 mm) and 55 mm sits inside the 73 mm row pitch with ~18 mm
  to spare. Take the **top row, the five bays at the end nearest the inlet**, so the
  module's heat leaves through top vents and does not rise through other units.
- **Height budget:** rear space 40 mm - 26 mm module = 14 mm for the unit board
  (1.6 mm PCB), its tallest parts and the air gap. With parts <= 8 mm over the
  board that leaves ~4 mm clearance; with the ~10 mm first estimate only ~2.4 mm.
  This is a **go/no-go number for #674: the five boards in those bays must keep
  their tallest part (buck inductor, 5-pin connector) at or under ~8 mm**, or
  use a low-profile variant for those five boards, or sit lower on the backplane.
- **Frames:** in those five bays the frame shells end at the mechanism (z ~54) so
  the module can cross the inter-unit walls; the unit boards keep their back-face
  position. The module is carried by a printed or sheet-metal bracket fixed to the
  side walls of the outer two bays and to the case rear panel, with 4-5 mm
  insulating standoffs and a 1 mm insulating sheet (FR4 or PETG) between the
  module case and the board parts. No load on the unit boards.
- **Mains side:** the C14 inlet module (fuse drawer, filter) sits on the rear panel
  of the same row end, next to the module's AC terminals (short, fixed leads). The
  AC terminal block is covered by a fixed cover; the module's DC terminals feed
  the controller box by one 12 V cable (or the controller sits in the space beside
  it, a layout-issue question).
- **Earth bonding:** the inlet's earth pin goes to the module's FG terminal with a
  ring lug and a short wire (class I); any metal frame member or bracket in reach
  is bonded to the same point. The 12 V rail (SELV) is not bonded to earth. The
  bonding, fuse rating and wire gauge follow the module datasheet and the inlet's
  rating; this spec does not fix them.
- **Other fit options if the gate fails:** (1) a rear bay of ~+35 mm for an LRS-class
  module (101 to ~136 mm, 35 % deeper than a Note); (2) an **external 12 V brick
  with a C14 inlet** beside the controller box (RS lists 180 W / 12 V / 15 A
  adapters with a C14 inlet): trivially fits, two cables.

**Lower-wattage UHP?** None found: the UHP-200 is the smallest UHP in what I could
read (a Mean Well catalog excerpt shows three UHP case sizes, 194 x 55 x 26, 220 x 62 x 31
and 233 x 81 x 31, not labelled per model; no UHP-100/120/150 listing turned up).
Check meanwell.com for a smaller UHP before ruling it out. At a ~108 W budget the
200 W module runs at ~54 % load, which only helps heat and lifetime, and the 180 W
ceiling still fits inside its rating. Other series are thicker or unmeasured
(EPP-150-12 is listed at 12 V / 8.4 A / 100.8 W by RS UK, dimensions not found; IRM-90-12
is 29.5 mm thick), so none fits the 40 mm better than the UHP-200.

Heat (estimates, 93 % typical efficiency from the listings): the module loses
~8 W at 108 W and ~13.5 W at 180 W (~15 W at 200 W), for the seconds a full-wall
change lasts; at ~10 W idle load ~0.7 W. The module is rated for 200 W by convection
(RS), -30..+70 C with derating; it sits in the top row with vents in the case top
and bottom edge, away from the flap stack. The derating curve and the temperature
in a sealed 5-bay pocket are not verified (datasheet not fetched): measure on the
first prototype with a thermocouple at 180 W for a few minutes.

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
| Function | any load to the module rating, no cap; 12 V, 16.7 A (UHP-200-12) | capped by the negotiated level (table above); a drop resets the wall |
| User experience | one IEC cord like the Note | one USB-C cable, but must be an EPR charger and e-marked cable; wrong charger silently caps or fails |
| Safety / compliance | class I mains product: earth, fused inlet, cover, creepage; CE LVD + EMC if sold; our PCBs stay ELV | no mains in the product (the source carries it); 28-48 V is above 12 V but below 75 V DC (LVD scope as I read it, verify); our PCBs are ELV; EMC of the new buck |
| Space / heat | UHP-200-12 (194 x 55 x 26 mm) fits the rear space over 5 bays of the top row if the unit boards stay <= ~8 mm tall there (to confirm in #674); thicker modules do not (deeper bay ~+35 mm or an external brick); ~8 W loss at 108 W, ~13.5 W at 180 W | ~9 W buck loss at peak on our PCB (estimate); source is outside the wall |
| BOM cost (estimates) | UHP-200-12 ~EUR 43-55 (GBP 37.60 ex VAT to USD 49; RS UK GBP 59.65) + C14 filter inlet with fuse ~EUR 6-10 + cord, earth, bracket, cover ~EUR 8: **~EUR 60-75**; an LRS-200-12 build is ~EUR 40-50 but does not fit | AP33772S ~EUR 1-2 + 12 A-class buck stage ~EUR 10-20 + EPR cable ~EUR 15-25 + EPR charger ~EUR 60-130 (240 W seen at USD 129-145): **~EUR 90-180**, more with a 48 V controller |
| Availability | UHP-200-12 799 in stock at Digi-Key US; LRS-200-12 710 pcs at one US distributor with a 19-week factory lead time quoted there; IRM-90 and TRACO stock not checked | AP33772S and certified cables available; 140 W+ EPR chargers are fewer and marketplace-priced; TPS26750 a newer part |
| Design effort | choose and mount; mechanical bay | new 12 A-class buck, PD firmware, EPR cable policy |

### Recommendation (the target; Lucas confirms)

**(A), a mains module: the Mean Well UHP-200-12 inside the wall, laid over five
bays of the top row, with an external C14 brick as the fallback.** Numbers: ~EUR 60-75
for the mains input (UHP-200-12 ~EUR 43-55, estimate) against ~EUR 90-180 for PD; no
stagger cap forced by the source; no new power stage to design or EMC-test; and the
module is the one candidate that fits: 194 x 55 x 26 mm, 200.4 W, 93 % typical,
fanless, so it fits the 40 mm rear space with 2-4 mm to spare (estimate). It keeps the
Note's single IEC cord. The condition is the go/no-go gate in #674: the unit boards in
those five bays stay <= ~8 mm tall. If that fails, or the thermal check at 180 W
fails, fall back to an **enclosed 12 V brick with a C14 inlet next to the controller
box** (identical electronics, zero mains in the wall); the controller board is the
same in every case. The 200 W rating is the smallest UHP found, so no
lower-wattage variant is a better fit at a 108 W budget. Choose (B) only if the wall
must have no mains at all (an ELV-only product, or sold without a mains conformity
assessment); then use the 28 V EPR level with the AP33772S, a 12 A-class buck and a
closed-loop stagger, accepting the 140 W ceiling.

Because this repo is public, a mains design carries a duty to say so: publish the
mains side as "use a certified module and a fused, earthed inlet; no mains on
the PCBs", never as a schematic to copy.

### Sources

Vestaboard Note power, dimensions and cord: <https://www.vestaboard.com/help/note-power>, <https://www.vestaboard.com/help/dimensions-note>, <https://www.vestaboard.com/help/power>. Mean Well UHP-200-12 (size, efficiency, price): <https://uk.rs-online.com/web/p/switching-power-supplies/1748124>, <https://www.digikey.co.uk/en/products/detail/mean-well-usa-inc/UHP-200-12/7707237>, <https://octopart.com/part/mean-well/UHP-200-12>; UHP case sizes: <https://www.meanwell.com/catalog/product/files/basic-html/page27.html>. Mean Well LRS-200-12 price and size: <https://www.bravoelectro.com/lrs-200-12.html>, <https://www.masterelectronics.com/en/mean-well/lrs20012-47466823.html>, <https://uk.rs-online.com/web/p/switching-power-supplies/1065859>. IRM-90-12: <https://www.digikey.com.br/en/products/detail/mean-well-usa-inc/IRM-90-12/11562725>, <https://meanwell.com/newsInfo.aspx?c=1&i=830>. 180 W modules: <https://uk.rs-online.com/web/p/switching-power-supplies/2499835?gb=s>, <https://int.rsdelivers.com/product/tdk-lambda/nvm1-1t000-s1-c/7511223>, <https://uk.rs-online.com/web/p/ac-dc-adapters/2707384>. AP33772S: <https://www.diodes.com/assets/Datasheets/AP33772S.pdf>. TPS26750: <https://ti.com/product/TPS26750>. EPR cables and chargers: <https://cdw.com/product/startech.com-6ft-usb-c-charging-cable-usb-if-certified-usb-c-cable-240w-p/7847865>, <https://www.newegg.com/p/3C6-005Y-00YB2>. INA226: <https://www.ti.com/product/ina226>, <https://octopart.com/part/texas-instruments/INA226AIDGSR>. Distributor pages change; prices are single-unit snapshots, not quotes.

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
| Fits the 101 mm depth | external box by default; internal mains (UHP-200-12 over 5 top-row bays) is the target | strip x3 or wall grows |
| Failure isolation | wall-level SPOF, row-level channel/fuse | row-level |
| Cost per wall | ~EUR 15 | ~EUR 30 + a firmware port |

## Decision

**(A1), decided by Lucas 2026-11-20: one S3 controller for the whole wall on its
own WiFi (no Ethernet), a TCA9548A-class mux giving each row its own bus with a
recovery ladder (accelerator footprint unpopulated), switched per-row eFuses with an
INA226 per row and one on the 12 V total, a second I2C bus for system devices, USB-C
for data and flashing, recessed BOOT/RESET, a factory reset on GPIO 4 and a DS3231-class
RTC with a battery.** Units take their address from the slot-coded backplane, so the
controller has no CHAIN heads and no enumeration. The 12 V source: internal mains is
the target (UHP-200-12 inside the top row, external brick as the fallback); the
side-by-side with USB-C PD in "Power input" stays for Lucas to decide.

- It reuses the shipped Master firmware, which already has the hard parts
  (OTA with rollback, rescue, boot and brownout guards, twiboot, HA contract).
  The only new firmware is row-indexed bus plumbing, a system-bus file, the
  INA226 telemetry, the per-row recovery ladder and a local multi-row layout, all small next to porting the
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
   decide pull-up value then, and whether the accelerator footprint is populated
   (far-unit rise time > ~1 us at 100 kHz). If the cable pushes a row past ~400 pF,
   move the mux next to the row (the controller moves out of the box and into the
   wall) or add a PCA9517A.
2. **3.3 V bus against 5 V ATmega inputs:** VIH(5 V) = 3.0 V against a 3.3 V
   drive is little margin. Decided with #674: the pull-ups run to 5 V on the
   controller and the unit board carries none. Whether the TCA9548A pass gates give
   the 3.3 V / 5 V step (and at which mux VCC) is **unverified**; fallback PCA9517A
   behind each channel.
3. **Moving-unit count and 12 V rail sag** for a 45-unit change: size the
   supply and fuses (36-60 W per row, 3-5 A, against the 6.3 A eFuse figure) from the
   INA226 and #306 vmin readings on a row prototype, not from the 2.4 W or 4 W
   figures.
4. **Unit-count ceilings** in the Master (probe/health arrays, snapshot sizes,
   HA unit counts) must be audited for 45; not verified here.
5. **GPIOs:** see the budget above; 4 is reserved for factory reset, 35/36/37 never.
5a. **Power input:** the open decision in the section above; the eFuse family's
   current range, the unit-board height over those five bays and the pocket
   temperature at 180 W are unverified.
6. **Back-feed of an unpowered row** through the unit pin diodes from the pull-ups:
   the pull-up switch must follow the eFuse; check on the first row prototype with
   the row off and the other rows running.

## Out of scope for the choice (decided later)

PCB outline and connector choice, the exact eFuse, accelerator, level-shift (or its fallback) and
RTC parts, the pull-up switch, the mains module and inlet (or the PD level), and
the box.

## Open questions for Lucas

1. **Power input:** internal mains is the target; confirm the mains module
   (UHP-200-12 over five top-row bays) against USB-C PD (ELV-only, 140 W ceiling with the 28 V level)? If mains: accept the
   #674 height gate (unit-board parts <= ~8 mm in those five bays) and the cut-back
   frame rear walls there, or keep the PSU outside?
2. **Box and cables:** where the box sits relative to the wall and the longest row
   cable it implies (the budget assumes <= 0.5 m; a longer run moves the mux next
   to the row or adds a PCA9517A).
3. **Supply headroom:** 108 W (#674 estimate) or 180 W (older estimate)? Decides a
   120 W or a 200 W module; confirm after the INA226/#306 measurement on a row
   prototype.
4. **Telemetry in HA:** expose per-row power as HA sensors (an append-only contract
   addition and a hass-config issue first), or keep it in the operator API only?
