# Hardware & control baseline

**Purpose.** A from-scratch software design needs one thing first: an honest,
assumption-free description of the machine it has to drive. This document is
that description. It is *not* a design, and it deliberately contains no
proposals — it records what the hardware **is** and how the units are
**actually** commanded today, so that a ground-up rewrite can be argued about
against facts instead of memory.

**How to read the markers.**

| Marker | Meaning |
|---|---|
| ✅ | Verified from source in this repo, from a build artifact, or read live off the deployed boards on 2026-07-27 |
| 📐 | Derived arithmetic from a ✅ fact (the derivation is shown) |
| ❓ | **Not established.** I could not determine this from the repository. Listed again in §10 as a question |

Nothing in this document is inferred silently. Where the repo contradicts
itself, both statements are shown and the contradiction is raised as a
question.

---

## 1. What is physically deployed, right now

Read live from the three boards on 2026-07-27 (`GET /settings`,
`GET /units/health`).

| Board | IP | Platform | Role | Units | Firmware |
|---|---|---|---|---|---|
| `split-flap-c8a746` | 192.168.15.91 | ESP32-S3 | **leader**, drives the ESP32 row | 16 (0x01–0x10) | `eaba380` ✅ |
| `split-flap-261bb6` | 192.168.15.121 | ESP-01 (ESP8266) | **follower**, drives the ESP-01 row | 5 (0x01–0x05) | `eaba380` ✅ |
| `split-flap-a47dee` | 192.168.15.20 | ESP32-S3 | standalone `headless-spare` | 0 | `eaba380` ✅ |

Rows are identified here by **which controller drives them**, not by index.

- **21 split-flap units total**, in two rows of different widths (16 + 5). ✅
- All 21 Nanos run unit firmware `d6e8a8a`, wire-protocol version `1`,
  0 faults, 0 version mismatches. ✅
- Rail health at rest: unit Vcc 4.85 V – 5.09 V across all 21 units;
  fleet minimum since boot 4850 mV. ✅
- The leader's `lastResetReason` is `Brownout` — this is the **normal** boot
  signature for the 16-unit row (homing inrush), including on a deliberate
  reboot. It is not evidence of a crash. ✅
- The two rows are joined over LAN HTTP into one logical display; the ESP-01
  row renders a 5-wide slice assigned by the leader. ✅

**Why there are two controllers at all:** a unit's address comes from 4 DIP
bits, so **one I2C bus tops out at 16 units** (§3.2). Reaching 21 units — or
any larger wall — requires a second bus with its own master. That is the
entire reason the ESP-01 is still in service: it is not a legacy remnant, it
is the second bus master. ✅

❓ The physical arrangement of the two rows on the wall (stacked? side by
side? one enclosure or two?) and whether the 5-wide row is intentionally
narrow or still being built out.

---

## 2. Hardware inventory

### 2.1 The master board — ESP32-S3-DevKitC-1-N16R8

A stock Espressif devkit; there is **no custom master PCB**. ✅

**This is the one and only hardware change ever made to the original
design:** the ESP-01S was removed from a unit board's socket and an ESP32-S3
devkit was **wired in its place**. ✅ Everything else — the unit PCBs, the
harness, the mechanics — is the v1 hardware unchanged.

That single fact settles the electrical path onto the bus (§3.1): the S3
reaches the units through the unit board's **ESP-01 socket**, which means it
sits behind that board's BSS138 level shifter and its AMS1117 3.3 V
regulator, exactly where the ESP-01 used to sit.

| Property | Value |
|---|---|
| Module | ESP32-S3-WROOM-1 N16R8 ✅ |
| Flash | 16 MB quad ✅ |
| PSRAM | 8 MB **octal** ✅ |
| Console | native USB-CDC (`ARDUINO_USB_MODE=1`, `CDC_ON_BOOT=1`) ✅ |
| Cores | 2 ✅ |

**GPIO budget — the pins that are already spoken for:** ✅

| GPIO | Use | Status |
|---|---|---|
| 8 | I2C SDA to the unit bus | in use |
| 9 | I2C SCL to the unit bus | in use |
| 4 | factory-reset button to GND (read by the **second-stage bootloader**, 5 s hold through reset) | reserved, committed in bootloader config |
| 19, 20 | native USB D-/D+ | unusable |
| 35, 36, 37 | consumed by the octal PSRAM | **unusable — never assign** |
| 48 | on-board WS2812 status LED | in use |

Everything else on the header is free. ✅

❓ Whether a physical GPIO 4 button is actually wired on the deployed
boards, or whether the factory-reset path is theoretical today.

❓ Whether the USB port of the wall-mounted leader is physically reachable
(this decides whether USB is a real recovery path or a fiction).

### 2.2 The second bus master — ESP-01S (ESP8266)

An ESP-01S still sitting in its socket on the other row's unit board, acting
as that row's bus master. It exists to get past the 16-unit-per-bus ceiling,
not for historical reasons. ✅

| Property | Value |
|---|---|
| Module | ESP-01S (Ai-Thinker), part of the v1 unit PCB BOM ✅ |
| Flash | 1 MB, `eagle.flash.1m.ld`, **no filesystem** ✅ |
| I2C | **GPIO1 (TX) = SDA, GPIO3 (RX) = SCL** ✅ |
| Serial | **none** — the UART pins *are* the I2C bus ✅ |
| Cores | 1, superloop ✅ |

This is a hard constraint that shapes everything about that board: it has no
console, so all diagnostics must be reachable over the network, and it has
~500 KB of usable app space because OTA needs room for a second image.

### 2.3 The unit board — the v1 PCB (`PCB/v1/`)

One board per flap unit. From the committed BOM and EasyEDA schematic: ✅

| Ref | Part | Role |
|---|---|---|
| U1 | Arduino Nano (ATmega328P, 16 MHz) | the unit MCU |
| U2 | **ESP-01S footprint** | populated on the *row master* board only |
| U3 | L7805CV (TO-220) | VIN → +5 V linear regulator |
| U9 | AMS1117-3.3 | +5 V → +3.3 V (for the ESP-01, where fitted) |
| U4 | ULN2003A | stepper darlington driver |
| Q1, Q2 | BSS138 ×2 | 2-channel bidirectional level shifter (3.3 V ↔ 5 V) |
| R1–R4 | 10 kΩ ×4 | pull-ups (level-shifter and/or DIP) |
| SW1 | DSWB04L 4-way DIP | I2C address |
| U6 | JST-XH 3-pin | hall sensor |
| U7, U8 | JST-XH 4-pin ×2 | bus in / bus out (daisy-chain) |
| U10 | JST-XH 5-pin | stepper |

**Only three power nets exist in the v1 schematic: `VIN`, `+5V`, `+3V3`
(plus `GND`). There is no 12 V net anywhere on the board.** ✅

> ✅ **A repo error, resolved 2026-07-27.** `README.md` says the units drive
> a "**28BYJ-48 12 V** stepper". The schematic has no 12 V net anywhere, and
> the operator confirmed the rows are fed **5 V** (§2.5). The motors are the
> **5 V** variant. **`README.md` is wrong on this point and should be
> corrected.**

> 🚫 **`PCB/v2/` is not evidence about this machine.** It documents a
> cancelled paper redesign (RS-485, STM32 units, custom boards) that was
> never built and never will be. It is cited nowhere in this document and
> must not be used to reason about the deployed hardware.

**Nano pin assignment (from `Unit.ino`):** ✅

| Pin | Use |
|---|---|
| D3, D4, D5, D6 | DIP address switches (bit 0 … bit 3, active-low) |
| D7 | KY-003 hall sensor (LOW = magnet present) |
| D8, D9, D10, D11 | ULN2003 → stepper coils |
| D13 | `LED_BUILTIN`, used for the IDENTIFY blink |
| A4, A5 | I2C SDA / SCL (hardware TWI) |
| A0 | read once at boot only, as a randomness seed |

📐 Nominally free on the Nano: D0/D1 (UART), D2, D12, A1, A2, A3, A6, A7.
❓ Whether any of those are actually wired to something on the PCB or in the
harness.

### 2.4 Mechanics, per unit

| Property | Value |
|---|---|
| Motor | 28BYJ-48 geared stepper via ULN2003 ✅ |
| Steps per drum revolution | **2038** (`STEPS`) ✅ |
| Flaps per drum | **45** ✅ |
| Steps per flap | 📐 2038 / 45 = **45.29** — non-integer, so the firmware carries a fractional-step accumulator |
| Home sensor | KY-003 hall + magnet, one marker per revolution ✅ |
| Measured hall window | 29 – 68 steps wide across the fleet (`stw0`/`stw1`) ✅ |
| Measured steps/rev, self-tested | 2048 – 2057 across the fleet (`str0`/`str1`) ✅ |

**The alphabet is fixed in the physical flap order** and is the single source
of truth shared by every firmware: ✅

```
" ABCDEFGHIJKLMNOPQRSTUVWXYZ$&#0123456789:.!?-"
```

- Index 0 is blank.
- `$ & #` are the wire encodings of `ä ö ü`.
- The tail order `: . ! ? -` is **physical drum order, verified against
  hardware** — the intuitive `: . - ? !` is wrong. ✅
- The master never sends a character; it sends an **index** into this table. ✅

### 2.5 Power

Confirmed by the operator on 2026-07-27; the repository records none of this.

| Fact | Source |
|---|---|
| **One supply per row**, independent of the other row | ✅ operator |
| Supply voltage: **5 V** | ✅ operator |
| **The row's master shares that same rail** with its units | ✅ operator |
| Each unit board *has* an L7805 (`VIN` → `+5V`) | ✅ schematic |
| Measured ATmega Vcc across the wall: 4850 – 5091 mV | ✅ live |
| A full row's simultaneous stepper inrush browns out the master | ✅ — it is why boot-homing, render batching and OTA-confirm ordering all exist |

📐 **The L7805 cannot be in the path.** A 7805 needs roughly 7 V in to hold
5 V out; from a 5 V input it would sit in dropout at ~3.5 V, which would not
run an ATmega328P reliably — yet every unit measures 4.85 – 5.09 V. So on the
deployed boards the 5 V rail reaches `+5V` **directly** (regulator
unpopulated, bypassed, or fed downstream of it).

Three consequences that any design must live with:

1. **There is no regulator headroom anywhere.** Rail sag is pure PSU
   behaviour plus wiring IR drop. The 150 mV worst-case sag measured under
   load says the wiring is currently adequate — that number is the budget.
2. **Master and units brown out together.** The master cannot ride out an
   inrush event its own units caused. This is not a fault to engineer away;
   it is the topology.
3. **A row can be power-cycled without disturbing the other row — but not
   without resetting its own master.** This directly bounds the unit-recovery
   story (§6).

❓ Still unknown: the current rating of each supply, wire gauge, and whether
any fusing exists. ❓ Whether the L7805 is physically populated on the
deployed boards.

---

## 3. The bus

### 3.1 Electrical

| Property | Value |
|---|---|
| Bus | I2C, single shared bus per row ✅ |
| Clock | **100 kHz** ✅ |
| Master side | ESP32-S3 GPIO 8/9 at 3.3 V ✅ |
| Unit side | ATmega328P hardware TWI at 5 V ✅ |
| Topology | daisy-chain through the per-unit 4-pin JST-XH in/out connectors ✅ |
| Wire buffer | forced to 256 B on both master platforms (`I2C_BUFFER_LENGTH`) ✅ |

100 kHz is a **deliberate, bench-proven decision, not a default**: an attempt
to run at 400 kHz was reverted because twiboot reflash became unreliable and
signal integrity on the full 16-unit run was marginal (units dropped, and the
bus locked up on a master reboot). ✅

**How each master joins the bus — settled.** Both masters attach at a unit
board's **ESP-01 socket**: the ESP-01 by still being in one, the S3 by being
wired into the other in the ESP-01's place. ✅ So in both cases the bus path
runs through that board's BSS138 two-channel level shifter (3.3 V master side
↔ 5 V Nano TWI side), which is the same translation the design always had.
No master sits directly on the 5 V bus.

❓ Where the I2C pull-ups actually live, and their value. Each unit board
carries four 10 kΩ resistors, but their nets are not established from the
BOM alone — 16 boards' worth of parallel pull-ups is electrically very
different from one pair at the master, and it bears directly on whether the
100 kHz ceiling is a wiring limit or a bus-loading one.

❓ Total bus length end-to-end on the 16-unit row.

### 3.2 Addressing

| Property | Value |
|---|---|
| Source | 4-bit DIP switch + `SFP_I2C_ADDRESS_BASE = 1` ✅ |
| Range | DIP `0000` → 0x01 … DIP `1111` → 0x10 ✅ |
| 0x00 | reserved — I2C general call ✅ |
| Override | an address may be burned into unit EEPROM; it takes precedence, with DIP as the fallback ✅ |
| Ceiling | **16 units per bus**, hard-limited by 4 DIP bits ✅ |

**This ceiling is the reason the system has two controllers.** A wall wider
than 16 units needs a second I2C bus, and a second bus needs its own master.
Any design that wants more units must either add bus masters or find address
bits — and the DIP switch is physical, so the second option means burning
EEPROM addresses and accepting that twiboot still answers on the DIP-derived
address (§3.2 below). ✅

**twiboot is patched to use the same DIP-derived address** rather than
stock twiboot's hardcoded 0x29 — so multiple empty Nanos can sit in
bootloader mode on one bus without colliding. ✅ The consequence: if a unit's
EEPROM address differs from its DIP setting, **over-I2C reflash cannot reach
it**, because the sketch answers on one address and the bootloader on
another. ✅

Broadcast: a write to 0x00 reaches every unit. By convention only `HOME` is
ever broadcast. ✅

### 3.3 Wire contract

**Direction of control is absolute: units are I2C slaves and can never
initiate.** Every piece of information the master has, it polled for. ✅

A transaction is one of two shapes:

- **Display command** — 2 bytes: `[letterIndex, speedRPM]`. `letterIndex` is
  0…44. ✅
- **Command opcode** — first byte ≥ 45 (so it can never alias a letter
  index), optionally followed by arguments. Queries are `0x8X` and are
  followed by a separate read; mutations are `0x9X` and are fire-and-forget
  on the wire. ✅

| Opcode | Name | Reply |
|---|---|---|
| 0x80 | ENTER_BOOTLOADER | — (**fixed forever**) |
| 0x81 | GET_VERSION | 10 B: 8 B git rev + protocol version + checksum (**fixed forever**) |
| 0x82 | GET_OFFSET | 3 B: int16 LE + checksum |
| 0x83 | GET_STATUS | 9 B: flags, reset cause, lifetime brownout/watchdog counts, uptime, bad-command count, last homing steps + checksum |
| 0x84 | GET_LETTER | 2 B: index + ~index |
| 0x85 | GET_ODOMETER | 5 B: uint32 LE revolutions + checksum |
| 0x86 | GET_DIAG | 6 B: physical letter, drift flags/events/magnitude + checksum |
| 0x87 | GET_SELF_TEST | 9 B: state, steps/rev, hall window, rev time + checksum |
| 0x88 | GET_VITALS | 8 B: Vcc now/min, commanded position, min free SRAM + checksum |
| 0x89 | GET_EXT_DIAG | 11 B: step excess, per-move Vcc sag, hall edges/rev, duty, status bits + checksum |
| 0x8A | GET_LIFETIME | 15 B: EEPROM layout version, lifetime home failures, feature gates, lifetime step-excess max, first/last self-test measurements + checksum |
| 0x90 | HOME | — |
| 0x91 | JOG | +1 signed byte |
| 0x92 | REBOOT | — |
| 0x93 | SET_OFFSET | +2 B int16 LE, complement-protected |
| 0x94 | SET_I2C_ADDRESS | +1 B, complement-protected |
| 0x95 | CLEAR_I2C_ADDRESS | — |
| 0x96 | IDENTIFY | — |
| 0x97 | RESET_ODOMETER | — |
| 0x98 | START_SELF_TEST | — |
| 0x99 | SET_GATES | +2 B gates + ~gates |

Three properties worth carrying forward regardless of what a rewrite does: ✅

1. **Every read reply is checksummed** with a *per-opcode* mask. This is what
   lets a master detect an old/mismatched unit, because an AVR TWI slave pads
   an over-clocked read rather than truncating it — **reply length can never
   be used to detect firmware age.**
2. **Every dangerous write carries the bitwise complement of its value.**
   `SET_I2C_ADDRESS` in particular used to be one unprotected byte after
   which the unit persisted and rebooted; a corrupted value relocated the
   unit to an address nobody was watching, recoverable only by a physical
   trip.
3. **One protocol version number** rides `GET_VERSION`, compared for
   equality. `GET_VERSION` and `ENTER_BOOTLOADER` are frozen forever,
   because they are the only way to interrogate and recover a unit whose
   contract you do not yet know.

---

## 4. How a unit is actually controlled

This section is the heart of the baseline. It describes the real behaviour of
the deployed Nano firmware, not the intent.

### 4.1 The drum is forward-only

The stepper is driven in one direction. To show a letter: ✅

- **Target index ≥ current index** → step forward by the difference.
- **Target index < current index** → the unit performs a **full homing
  calibration first**, then steps forward from zero.

So a "backwards" move costs up to a full revolution plus a homing seek.
Every rewrite must budget for this: it is mechanical, not a software choice.

### 4.2 Homing

- Seek forward at a **fixed 10 RPM** — deliberately *not* the last commanded
  speed, because a fast homing overshoots the hall window and produces an
  inconsistent zero. ✅
- Give up after 3 full revolutions without seeing the marker, and set
  `homeFailed` / `hallNeverTriggered` status bits. ✅
- On success, step forward by the per-unit **calibration offset** stored in
  EEPROM; that lands index 0 (blank) at the marker. ✅
- Offset is clamped to ±2038 steps (one revolution) on **both** sides of the
  wire. Beyond ~2700 steps the post-homing `stepper.step(offset)` — a single
  blocking library call — outruns the Nano's 8 s watchdog and resets the MCU
  mid-rotation. ✅

### 4.3 Speed and thermal limits

- Wire speed is RPM, clamped by the master to **1 … 12 RPM**. ✅
- The web slider's 1…100 is mapped onto that range. ✅
- **Anti-overheat gate:** after a rotation, the unit refuses to move again for
  2 seconds. While a target is pending but the gate is closed, the unit
  reports *busy* so the master waits rather than concluding the move
  finished. ✅

### 4.4 Rendering a frame (master side)

`unitBusShowFrame` is a strictly sequential, blocking procedure: ✅

1. Wait until **no** unit reports rotating (30 s cap, watchdog fed
   throughout — a jammed flap is survived, not rebooted through).
2. For each unit that the probe found running the sketch: write
   `[letterIndex, speed]`.
   **After every 4 units, pause 100 ms** to let the rail settle — a full
   row's steppers spinning up at once browns out the master.
3. Wait again for all motion to stop.
4. **Verify**: read `GET_LETTER` from each unit, and resend once to any unit
   that disagrees. This is a genuine closed loop.

Absent units and units on an unknown protocol version are skipped rather than
written — a render is the loudest thing to get wrong against an unknown
contract. ✅

### 4.5 Boot homing is staggered, and the unit self-heals

Units **do not home in `setup()`**. They boot unhomed and blank, and home on
the first of four triggers: ✅

1. An explicit `HOME`.
2. A letter command arrives while unhomed (home first, then show).
3. No master contact within 30 s + `address × 600 ms` + up to 250 ms jitter →
   self-home (the address stagger keeps a standalone row from homing all at
   once).
4. A master *is* present but never commanded a home → hard cap at 120 s,
   home anyway so a row is never permanently dark.

The master's own boot sequence homes units **one at a time**, with a 500 ms
rail-settle between each and an 8 s per-unit cap. ✅

**Drift self-heal:** the unit watches the hall sensor on every forward step.
Because index 0 sits at the marker and forward moves never wrap past it, an
unexpected marker crossing is a direct observation of physical slip. Past a
threshold it arms an automatic re-home, rate-limited to once per minute so a
mechanically failing unit does not spend its life homing. ✅

**Feature gates:** two motion behaviours are compiled in but ship **disabled**,
switchable over the wire via `SET_GATES` and verified by reading
`GET_LIFETIME` back. A unit refuses to persist a gate bit it has no code for. ✅

### 4.6 Liveness

Since slaves cannot speak first, the master synthesises liveness: it reads
`GET_STATUS` from **one unit per 3 s tick**, round-robin. 📐 For a 16-unit row
that is a 48 s full-fleet cadence. Three consecutive misses mark a unit
stale/lost. ✅

### 4.7 Idle behaviour

When idle for 2 s with nothing pending, the Nano enters `SLEEP_MODE_IDLE` —
the CPU clock stops but TWI keeps running, so it still ACKs the master. ✅

---

## 5. Non-volatile state

### 5.1 Unit EEPROM — 1024 B, layout version 1

```
  0      u8    layout version      (anything else = blank → initialise)
  1      u8    I2C address         1..126, else DIP fallback
  2      u8    flags               bit0 = address provisioned
  3      u8    checksum over 0..2
  4..5   i16   calibration offset  clamped ±2038
  6      u8    checksum over 4..5
  7      -     reserved
  8      u8    lifetime brownout count      (saturating)
  9      u8    lifetime watchdog count      (saturating)
 10      u8    lifetime home-failure count  (saturating)
 11      u8    feature gates
 12..13  u16   lifetime step-excess max
 14..21        first/last self-test measurements (hall window, steps/rev)
 22      u8    checksum over 8..21
 23      -     reserved
 24..63        40 B reserved scalars   ← currently written by nothing
 64..703       odometer ring (interleaved slots)
704..1023      320 B free
```
✅

Three design properties that are load-bearing:

- **One version byte, no magic constants.** Erased EEPROM reads 0xFF, so
  "not the current version" *is* the blank test.
- **Every block independently checksummed, with distinct masks** so a block
  read at the wrong offset cannot validate.
- **Every failure degrades to a safe default**: identity → DIP address
  (always reachable by twiboot), calibration → 0, health → zeros.

⚠️ **Known bug (#417):** the day-0 initialiser deliberately skips the odometer
ring, and it hardcodes the calibration offset to 0. Bumping the layout version
therefore **destroys every unit's calibration** and forces a full
re-calibration campaign. The 40 reserved bytes at 24..63 are written by
nothing and are the obvious place to fix this without a version bump.

### 5.2 Master flash — the partition table is immutable

```
nvs       64 KB     @ 0x009000   settings, WiFi credentials, cluster state
otadata    8 KB     @ 0x019000
app0       4 MB     @ 0x020000   ← A/B OTA slot
app1       4 MB     @ 0x420000   ← A/B OTA slot
coredump  64 KB     @ 0x820000
factory    2 MB     @ 0x830000   rescue image
storage  ~5.8 MB    @ 0xA30000   one shared LittleFS
```
✅ The table can only ever be written over USB. An OTA can never move or
resize a partition. Its offsets are pinned by a test.

---

## 6. Firmware update paths — three separate problems

The single stated constraint ("it needs to be OTA flashable") is actually
three independent mechanisms with different physics. ✅

**1. The S3 master.** Native ESP32 A/B slots. An uploaded image goes to the
inactive slot with a mandatory MD5, boots `PENDING_VERIFY`, and is rolled
back by the bootloader if it cannot confirm. Confirmation happens
**pre-inrush at the end of `setup()`** — deliberately before the units start
homing, because a later confirm loses the race to the boot brownout. There is
also a rescue image in the factory slot, reachable via GPIO 4 or an endpoint.

**2. The ESP-01 row master.** ESP8266 `Update` flow inside 1 MB with no
filesystem. 📐 Roughly half the flash must stay free for the incoming image,
so the app ceiling is ~500 KB; the current build is 423 KB.

**3. The Nano units — twiboot over I2C.** The interesting one.

| Property | Value |
|---|---|
| Bootloader | vendored + patched twiboot, ~930 B ✅ |
| Boot section | 1024 B (HFUSE `0xDC`, `BOOTRST` set → powers up into the bootloader) ✅ |
| Listen address | DIP-derived, same as the sketch ✅ |
| Boot window | ~1 s; any I2C activity holds it open, otherwise it jumps to the app ✅ |
| Page size | 128 B; a page write is a single 132 B transaction ✅ |
| Installation | **ICSP, once per Nano** — the only step that is not over-the-air ✅ |

The sketch itself is what enters the bootloader: the master sends
`ENTER_BOOTLOADER`, the sketch triggers a watchdog reset, and twiboot takes
over. **The bootloader never erases itself**, so a failed mid-write leaves the
unit sitting in twiboot and the master re-pushes on the next probe. ✅

#### 6.1 How far the safety net actually reaches

This decides how bold a unit-firmware rewrite is allowed to be, so it is worth
stating precisely.

**twiboot carries a stay-alive-on-empty-flash patch.** If the application
section's first instruction word reads `0xFFFF` — what erased AVR flash looks
like — the boot window **never counts down**. The bootloader waits
indefinitely for the master to push a sketch. ✅

📐 Therefore the failure modes split cleanly:

| Failure | Recoverable over I2C? |
|---|---|
| Flash aborted, app section erased | ✅ **Always.** twiboot waits forever |
| Flash aborted mid-stream | ✅ Unit stays in twiboot for that session; master re-pushes |
| Complete sketch that still answers `ENTER_BOOTLOADER` | ✅ Normal path |
| **Complete sketch that hangs, crashes, or never answers I2C** | ⚠️ **Only by catching twiboot's ~1 s boot window at reset** |

That last row is the whole risk surface of a unit rewrite. Today nothing
catches that window reliably: the heartbeat polls one unit per 3 s, 📐 a 48 s
full-fleet cadence against a 1 s opening.

Because master and units share a rail and boot together (§2.5), the master
*could* be designed to grab the bus in the first few hundred milliseconds
after reset — before WiFi, before anything — and hold every DIP address open.
That would make the unit firmware **unbrickable by software**: any power cycle
of the row lands every unit in twiboot, and the master then decides whether to
release them or reflash. It costs boot latency and needs a timeout so a wedged
master cannot leave the row dark.

This is an *option*, not a decision — but it is the mechanism that would let a
ground-up unit firmware be written without an ICSP campaign as the fallback.

There is one delicate mechanism: `isUnitInBootloader()`'s probe *pins twiboot
alive*, so a 1500 ms pre-probe delay is load-bearing, and a probe-inhibit
deadline exists to stop runtime probes from interfering. ✅

The compiled unit sketch is **bundled inside the master image** (PROGMEM) and
staged by a script, with a CI gate that fails the build if the bundle drifts
from the built unit hex. ✅

---

## 7. Constraints any design must obey

Collected in one place. Each is verified above.

1. **Units cannot initiate.** All state is polled. There is no interrupt line,
   no attention pin, no second wire.
2. **16 units per bus, hard.** 4 DIP bits.
3. **100 kHz.** 400 kHz was tried on this exact wall and reverted.
4. **The drum only turns forward.** A backwards move costs a homing seek plus
   up to a full revolution.
5. **Homing is the only absolute reference.** One magnet, one marker per
   revolution. There is no encoder.
6. **Inrush is a real, measured limit.** Whole-row simultaneous motion browns
   out the master, which shares the row's 5 V rail. Homing is one-at-a-time;
   rendering is four-at-a-time with a 100 ms settle.
6b. **There is no regulator headroom.** The rail is 5 V end to end; measured
   worst-case sag under load is 150 mV. That is the entire budget for any
   increase in simultaneous motion.
6c. **A row can be power-cycled independently of the other row, but not
   independently of its own master.**
7. **The Nano's watchdog is 8 s**, and stepping is *blocking* — any long move
   must feed the watchdog from inside the loop.
8. **Nano budgets:** 31,744 B app flash (below twiboot), 2 KB SRAM, 1 KB
   EEPROM. Current use: 13,848 B flash 📐(43.6%), 445 B static SRAM,
   1559 B measured free SRAM at runtime.
9. **A 2 s per-unit thermal gate** limits how often a flap can move.
10. **The ESP-01 has no serial port** — its UART pins are the I2C bus.
11. **The S3 partition table is immutable over OTA.**
12. **twiboot's contract is fixed by hardware already deployed.** Changing it
    means an ICSP trip to 21 physically mounted Nanos.
13. **The 45-flap alphabet and its physical order are fixed by the drums.**
14. **A wall-mounted board must always be able to accept an OTA.** An image
    with no working update endpoint is a board that needs a screwdriver.

---

## 8. Measured budgets

| Artifact | Size | Capacity | Fill |
|---|---|---|---|
| Master `firmware.bin` | 1,569,888 B ✅ | 4,194,304 B slot | 📐 37.4 % |
| Rescue `firmware.bin` | 1,061,072 B ✅ | 2,097,152 B slot | 📐 50.6 % |
| Follower `firmware.bin` | 423,008 B ✅ | ~512 KB usable | 📐 ~83 % |
| Unit `firmware.hex` | 38,967 B (hex) → 13,848 B flash ✅ | 31,744 B | 📐 43.6 % |
| Unit SRAM (static) | 445 B ✅ | 2,048 B | 📐 21.7 % |
| Unit SRAM (min free, live) | 1,559 B ✅ | 2,048 B | — |

The ESP-01 is the tight one. Everything else has room.

---

## 9. What the software does today

Not a recommendation to keep any of it — an inventory, so that a rewrite is a
deliberate choice about each item rather than an accidental loss.

**Display:** text rendering with left/center/right alignment; a clock mode
ticked once per minute and gated on NTP sync; a transient "show then revert"
overlay; live change events over SSE.

**Units:** bus probe and width derivation; per-unit health polling; interactive
calibration wizard (show a letter, type what the drum shows, apply the
delta); jog; identify blink; address burn/clear; on-demand self-test; wear
odometer with relative-wear flagging; an event log for health transitions.

**Network:** WiFi join with a captive setup portal; NTP with a full IANA
timezone table; mDNS advertisement; MQTT with Home Assistant discovery
(24 entities); a three-tab web UI baked into the binary as PROGMEM.

**Fleet:** an N-row cluster over LAN HTTP — grid layout with wrap/align/slice,
leader/follower phase machines, HMAC-authenticated wire, automatic firmware
convergence across members, promote-on-leader-death, a single-pane health
digest piggybacked on the ping.

**Reliability:** A/B OTA with rollback; a rescue image in the factory slot; a
custom bootloader for factory reset; a 30 s task watchdog; a flash-backed log
that survives reboots; coredump capture.

**Tooling:** OTA fan-out scripts, a unit-offset capture/replay campaign, a
gated per-unit commissioning script, host-side test suites for every pure
header, and CI gates against firmware-bundle drift.

---

## 10. Open questions

**The hardware is fixed and is not up for discussion.** Nothing here asks
whether to change it — these are gaps in *my knowledge of it*, plus scope
decisions about the software.

### Resolved on 2026-07-27

| Question | Answer |
|---|---|
| What feeds the wall? | **One 5 V supply per row**, each row independent; **the row's master shares that rail** (§2.5) |
| 5 V or 12 V motors? | **5 V** — `README.md` is wrong (§2.3) |
| ICSP access to the Nanos? | Physically reachable, but a 21-unit ICSP campaign is **not acceptable**. Treat twiboot's contract as fixed; a one-time flag day is possible only for a large payoff |
| How does the S3 join the bus? | Wired into a unit board's **ESP-01 socket** — behind that board's BSS138 level shifter (§2.1, §3.1) |
| Why two controllers? | The 16-unit DIP ceiling. The ESP-01 is the **second bus master**, not a leftover (§1, §3.2) |
| Is `PCB/v2/` relevant? | **No.** Cancelled paper design, never built. Struck from this document |

### A. Gating — the design cannot be finished without these

1. **Flag day.** May all 21 units be reflashed to a new wire protocol in one
   campaign, or must a new master also drive units still running today's
   firmware? (Reflashing is over-I2C and does not need ICSP — this is a
   question about acceptable downtime and rollback, not access.)
2. **Wall growth.** Is 21 units the end state, or does this grow? Every extra
   16 units means another bus master, and the answer decides whether
   multi-controller coordination is a core abstraction or an artefact to be
   minimised.

### B. Shapes the architecture

3. **Where the display lives.** Should one controller own the whole logical
   display and treat the others as dumb bus adapters, or should each keep
   local autonomy and coordinate as peers? Today it is the latter, and that
   is where most of the complexity sits.
4. **The ESP-01's job.** Keeping it means any protocol it speaks must fit
   inside ~500 KB of flash, ~40 KB of heap, one core, and **no console**.
   Is it acceptable for it to be a strictly thinner client than the S3
   (fewer capabilities, not just less code)?
5. **Unit recovery guarantee.** Do you want the "grab the bus at boot so a
   power cycle always lands units in twiboot" property (§6.1)? It makes unit
   firmware unbrickable without ICSP, at the cost of boot latency and a
   master that must behave.

### C. Scope

6. **Must-keep features.** Of §9 — which are genuinely used? Specifically:
   MQTT/Home Assistant, clock mode, the rescue slot, multi-controller
   coordination, the calibration wizard.
7. **Interface.** Is a browser UI required, or is an API plus Home Assistant
   enough? (The previous UI arc was discarded; the stated plan is API first.)
8. **Network posture.** LAN-only, VLAN, or internet-reachable? Direct NTP or
   time from a local source?
9. **Physical layout.** How are the two rows arranged, and is the 5-wide row
   deliberate or still being built out?
10. **Non-goals.** Anything already ruled out (the repo records that a
    seconds-resolution clock was declined, and that UDP syslog and Prometheus
    were cut).

### D. Minor — would sharpen the design, none are blocking

11. Current rating of each row's 5 V supply; wire gauge; any fusing.
12. Is the L7805 populated on the deployed unit boards, or bypassed?
13. Where the I2C pull-ups sit and their value (bears on the 100 kHz ceiling).
14. Total I2C bus length on the 16-unit row.
15. Is a GPIO 4 factory-reset button physically wired?
16. Is the S3's USB port reachable without dismounting it?
17. Are any of the Nano's free pins (D2, D12, A1–A3, A6, A7) used by the
    harness?
17. Total I2C bus length on the 16-unit row.

---

## Appendix: where the facts came from

| Area | Source |
|---|---|
| Fleet state, unit health, revisions | live `GET /settings` and `GET /units/health`, 2026-07-27 |
| Master hardware, GPIO budget, partitions | `firmware/v2/Master/platformio.ini`, `partitions_splitflap_16MB.csv`, `UnitBus.cpp`, `StatusLed.cpp` |
| Unit hardware, pins, motion, EEPROM | `firmware/v2/Unit/Unit.ino`, `UnitMotion.ino`, `UnitI2CProtocol.ino`, `UnitEeprom.h` |
| Wire contract | `firmware/v2/shared/SplitFlapProtocol.h`, `UnitWireContract.h`, `UnitVitals.h` |
| Render/home/heartbeat policies | `firmware/v2/shared/RenderStagger.h`, `BootHomePlan.h`, `HeartbeatPolicy.h`, `Unit/BootHomePolicy.h` |
| Bootloader | `firmware/v2/UnitBootloader/README.md`, `Makefile`, `main.c` |
| ESP-01 follower | `firmware/v2/FollowerEsp01/platformio.ini`, `FollowerBus.cpp` |
| Unit PCB | `PCB/v1/BOM_PCB_Splitflap_2022-03-11.csv`, `PCB/v1/EasyEDA_Source/SCH_SplitFlap_2022-06-04.json` |
| Build sizes | `.pio` build artifacts + `avr-size`, 2026-07-27 |
| Power, ESP-01→ESP32 swap, ICSP policy | operator, 2026-07-27 |

**Deliberately not cited:** `PCB/v2/` — a cancelled paper redesign of hardware
that was never built. `README.md` — contains at least one hardware error
(§2.3); the schematic and the running boards win.
