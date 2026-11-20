# Note-size unit board: a compact bare-ATmega328PB board — design

Issue: #674 · Epic: #676 · Status: decisions of 2026-11-20 (rounds 1 and 2) folded in; schematic and layout follow

## Intent and why

The Note-size wall (3 rows x 15 columns, ~41.5 mm column pitch, 73 mm row
pitch, ~95 mm deep) needs a unit board that fits the unit envelope. The v1
board (`PCB/v1/`, BOM 2022-03-11) cannot: it carries an Arduino Nano module, an
L7805 in TO-220, a through-hole DIP switch, an ESP-01S socket, a BSS138 level
shifter and JST XH connectors. (The v1 BOM already uses an SMD ULN2003A, SOP-16;
only the Nano, L7805, DIP switch, ESP-01S and the XH connectors are through-hole
or module parts.) This spec fixes the architecture and the draft BOM
(`PCB/v2-note/BOM-draft.csv`) so schematic and layout can start once #671
(motor) and #673 (outline, encoder placement) land.

This is the third project of the wall: #670 flaps, #671 drive, #673 mechanics,
this board, then the firmware alphabet. `PCB/v1/` stays the deployed board.

## Goals

- One board per unit: ATmega328PB, motor driver, AS5600-class encoder interface,
  12 V to 5 V buck, reverse-polarity FET, 12 V sense divider, one plug into the row
  backplane (power, I2C, chain), Tag-Connect pads. Nothing else.
- The existing Unit firmware, I2C slave and twiboot carry over with a board
  variant (pin map, address source, encoder instead of hall), not a rewrite.
- Assembled by JLCPCB from LCSC parts, like v1.
- A target outline that fits behind the mechanism inside 39.1 x 70.6 mm (#673).

## Non-goals

- The controller board, the 3.3 V/5 V shifter and row-link wiring (#675).
- The motor decision (#671) and the encoder/outline numbers (#673): this spec
  designs for the three #671 candidates and takes the outline as provisional.
- Firmware changes: this spec lists them; it implements none.
- Touching `PCB/v1/`, the deployed 45-flap units or `firmware/v1/`.

## Anti-goals

- A part that needs a firmware rewrite on the first bring-up .
- An address scheme that can leave a populated row with two units on one
  address, or that needs a physical trip to recover a unit (a chain line fixes
  both, section 9).
- A board that ships with twiboot listening somewhere the sketch does not.

## Constraints

- **Envelope (from #673, commit 3400f82 on `docs/673-drum-frame-spec`):** there
  is no room beside the drum (4.5 mm between neighbouring drum faces holds two
  1.2 mm walls plus gaps). The board sits **behind the mechanism**, parallel to
  the back wall (the 41.5 x 73 face), in the ~40 mm of depth behind the
  mechanism's rear reach (z = 53.1 mm from the display plane). Keep clear of
  z < 54 (flap tips). Height budget ~38 mm along z, both faces usable.
- **Outline:** at most 39.1 wide x 70.6 tall (frame inner size); it must not
  protrude past the 41.5 x 73 footprint. Mounting: four M2 bosses on the back
  wall, pattern set by this board (#673 parameter `board_hole_xy`, default none); the
  board's plug reaches the row backplane through a slot in that wall (section 6).
  Still provisional: #673's pocket and wall detail is not settled.
- **Target outline (provisional):** ~35 x 65 mm, which leaves room for the board
  edge margin inside 39.1 x 70.6; refine once the part list is placed.
- **MCU facts verified in the repo:** `F_CPU` 16 MHz (PlatformIO
  `nanoatmega328new`; `UnitBootloader/Makefile` `CFLAGS_TARGET` and
  `make_new_twiboot.py` use `-DF_CPU=16000000ULL`, `TIMER_IRQFREQ_MS=16`; the
  prebuilt image is `twiboot-atmega328p-16mhz.hex`). Fuses `LFUSE=0xFF`
  (full-swing crystal/resonator, 16K CK + 65 ms start-up), `HFUSE=0xDC`
  (BOOTRST set, BOOTSZ=10 = 512 words = 1 KB boot section, EESAVE not
  programmed), `EFUSE=0xFD` (BOD 2.7 V). That is the fielded Nano; the note
  board burns `HFUSE=0xD2` (section 7).
- **Pins the firmware uses today (Nano):** stepper D8..D11 (PB0..PB3), hall D7 (PD7,
  plain `INPUT`), DIP D3..D6 (PD3..PD6), LED D13 (PB5, `LED_BUILTIN`), I2C A4/A5, A0
  floating for the random seed. Supply is measured against the internal bandgap
  (`readVccMv()`), which on this board no longer sees the motor rail (section 10).
- **Pins on the note board:** stepper PB0..PB3 (D8..D11, unchanged); LED PB5; TWI0
  slave PC4/PC5 to the backplane; CHAIN_IN PD3 and CHAIN_OUT PD4 (section 9); **TWI1
  master PE0/PE1 (pins 3 and 6) to the encoder** (section 5); 12 V sense ADC7 (section
  10); PD7 (the old hall pin) free; ADC6 takes the encoder's analog OUT as a bring-up
  fallback; PD0/PD1 test points; PD2, PD5, PD6, PB4, PC1..PC3 spare.
- Public repo: no addresses, hostnames or device ids in any file.

## Block diagram

```
 backplane plug (section 6)
 12 V ------+--[TVS]--+--[P-MOSFET rev-prot]--+------------------> ULN2003A COM (motor 12 V)
 GND -------+                                 +--[buck 12 V -> 5 V]--> 5 V rail
                                              +--[120k]--+--[10k]--GND
                                                         +--[100n]--> ADC7 (12 V sense)
 SDA/SCL (5 V) --[47R]--[ESD]--> TWI0 slave (PC4/PC5)
 CHAIN_IN --[ESD]--[1k]--+--[100k to GND]
                         +--> PD3 (CHAIN_IN)      [ATmega328PB]  16 MHz, twiboot
 CHAIN_OUT <--[ESD]------------ PD4                  PB0..PB3 --> [ULN2003A] --> motor
                                                     PB5 --> status LED, Tag-Connect pads
 encoder tail (FFC, J3) <-- 5 V, GND, SDA1/SCL1 (PE0/PE1, 4k7 pull-ups), OUT --> ADC6
```

## 1. MCU package: TQFP-32 vs QFN-32

| | TQFP-32 (7x7, 0.8 mm pitch) | QFN-32 (5x5, 0.5 mm pitch) |
|---|---|---|
| Footprint | ~9 x 9 mm with leads | ~5 x 5 mm |
| Height | <= 1.2 mm | <= 1.0 mm |
| JLC assembly | fine, extended part | fine, extended part |
| Rework | soldering iron, probe the leads | hot air only, no visible joints |
| Pad | none | exposed pad to GND |

**Recommendation: TQFP-32 for the first batch** (hand-reworkable, probe-able
leads; ~9 x 9 mm of a ~35 x 65 mm board). The ~38 mm height budget removes any
height reason to prefer QFN; switch only if placement runs out of area. Both
packages carry the ATmega328PB pinout. Pins 3 and 6 are power pins on the 328P but
PE0/PE1 on the 328PB, and PE2/PE3 are ADC6/ADC7 (section 12): the footprint follows the
328PB datasheet, not the 328P symbol. Signals used: PB0..PB5, PC4/PC5, PD0/PD1,
PD3/PD4, PE0/PE1, ADC7 (12 V sense), ADC6 (encoder OUT fallback), reset, AVCC, AREF.

## 2. Clock: crystal vs resonator

The firmware and twiboot assume 16 MHz (verified above). The slave side of I2C
is clocked by the master, so the CPU clock only matters for the stepper
timing, the bootloader timeout window and watchdog-based timers: a +-0.5 %
ceramic resonator would work, a crystal matches what the fleet runs today.

**Recommendation: 16 MHz SMD crystal (3.2 x 2.5 mm) plus two load capacitors**,
because it behaves exactly like the Nano the simulation proofs and the whole
fleet were validated on, and the part count is irrelevant at JLC. Fallback: a
3-pin SMD ceramic resonator with built-in capacitors (saves two parts and ~4
mm2); the same `LFUSE=0xFF` suits both.

## 3. Power: 12 V bus and a per-unit buck

The motor dominates. Two cases, set by #671:

| Case | Motor supply | Buck load |
|---|---|---|
| 24BYJ48 12 V + ULN2003A | raw 12 V, bypassing the buck | logic only |
| 5 V motor (15BY25) or DRV8833 (max 10.8 V, not 12 V) | the 5 V rail | logic + motor |

For the baseline the buck only feeds the 328PB, encoder, LEDs and pull-ups (~30 mA
estimated), so a small converter works. A 12 V to 5 V LDO at ~30 mA would
dissipate ~0.2 W; the issue asks for a buck, and a 5 V-motor candidate needs one
anyway, so the draft keeps a buck and sizes it per the final motor.

Buck selection rules (part left `TBD-verify` in the BOM):
- input rating at least 32 V (hot-plug spikes on a 12 V row feed), fixed 5 V output, synchronous, SOT-23-6 class;
- continuous current >= logic load for the baseline, >= logic + motor for a
  5 V-motor candidate (decide after #671);
- a TVS on the 12 V input (also keeps the reverse-protection FET's gate-source below its 20 V limit);
- the v1 22 uF 10 V capacitor (C503893) is only valid on the 5 V node. A
  12 V input node needs a >= 25 V capacitor.
- Backfeed: with the 12 V bus absent and ICSP powered at 5 V, a synchronous buck
  can feed 5 V back up its high-side body diode onto the bus. ICSP powering is
  therefore done through the plug (the jig supplies 12 V on the backplane pins) or through a VCC
  pad behind an open solder jumper (section 7).

**Reverse polarity (decided):** a P-channel MOSFET in the unit's own 12 V branch
(drain to the bus junction, source to the buck and the ULN2003A COM, gate to GND
through 10 k), ~0.2 V drop at 0.2 A. It is **not** in the row's through path: the backplane's 12 V copper carries the
whole downstream row (~3 A) and has no FET, so one unit's FET only carries that unit's
own ~0.2 A. A reversed unit stays unpowered and blocked. The plug is keyed; the FET covers
the supply only, not SDA/SCL/CHAIN pulled up from the controller side, so keying
stays the first defence. Pick a MOSFET with Vds >= 30 V, Vgs max >= 20 V,
Rds(on) < 100 mohm at Vgs -10 V (`TBD-verify` in the BOM).

## 4. Motor driver, per #671

#671's baseline is the **24BYJ48 12 V** (24 x 19 mm, 120 ohm per phase, same
4-phase unipolar 1/64 drive as the 28BYJ-48; some batches ship 1/16, check on
arrival).

| Candidate | Driver | Firmware impact |
|---|---|---|
| 24BYJ48 12 V (baseline) | SMD ULN2003A (SOP-16, v1 part C107221), COM to 12 V | None to the motion code **if the batch is 1/64** (`STEPS 2038`, same sequence, same four pins, coils idle low). A 1/16 batch changes `STEPS`. |
| 15BY25-560A + printed 1:8-10 stage | DRV8833, motor from the 5 V rail (10.8 V max) | Same four GPIO as the Stepper library (AIN1, AIN2, BIN1, BIN2), different pin order in the constructor. `STEPS` and the gear ratio change. |
| GM10-10BY-class planetary | DRV8833 (5 V rail) | As above. |

"Firmware unchanged" is conditional. `STEPS 2038` is `static_assert`ed equal to
`SFP_OFFSET_LIMIT_STEPS` in `firmware/v2/shared/SplitFlapProtocol.h`, so any
other step count (1/16 gearbox, planetary, microstepping) touches `shared/`, the
masters' offset clamp and the unit bundle; `AMOUNTFLAPS` for 64 flaps is the
alphabet project's change either way. The home policy, stall policy and
supply-hold timings are tuned on the 28BYJ-48 and must be bench-checked after a
motor change. Footprints: the board carries the ULN2003A and, depopulated, an
alternate DRV8833 footprint.

Idle: the firmware pulls all four stepper pins low after a move (coil release),
so the idle motor current is zero; keep that.

## 5. Absolute angle encoder (replaces the hall)

An **AS5600-class** 12-bit magnetic rotary position sensor reading a diametric magnet on
the drum's far axle end replaces the A3144 hall (decided 2026-11-20): the unit reads its
absolute drum angle instead of waiting for one marker per revolution. The chip figures below are from
memory of the datasheet and are **datasheet-verify**: 12-bit (4096 counts per
revolution, ~2.0 counts per motor step at `STEPS` 2038), I2C address 0x36 fixed on the
AS5600 (the AS5600L is configurable), a status register with magnet detected / too weak
/ too strong bits and an AGC reading, an OUT pin that is analog or PWM, 5 V or 3.3 V
supply, ~6.5 mA, magnet gap 0.5 to 3 mm, reference magnet diametric Ø6 x 2.5 mm.

### Interface: I2C on the 328PB's second TWI

| | I2C | analog OUT | PWM OUT |
|---|---|---|---|
| Resolution | 12-bit raw angle | 10-bit ADC over ~90 % of the supply: ~0.44 deg, ~2.5 steps per count | 12-bit by pulse width, ~1.1 ms per reading, needs a capture pin |
| Diagnostics | magnet detected / weak / strong, AGC | none | none |
| Pins | PE0/PE1 (TWI1) and two pull-ups | one ADC pin | one capture pin |

I2C wins on resolution (finer than one motor step) and on diagnostics: "magnet missing"
and "magnet too weak" become readable states, which the hall could never report. The
fixed address cannot share the row bus (TWI0 is a slave there), so the encoder gets a
private master bus on the 328PB's second TWI (TWI1, PE0/PE1, MiniCore `Wire1`). The 328P
has no second TWI, so an I2C encoder there needs a bit-banged bus: **this choice makes the
board 328PB-only** (section 12). The analog OUT goes to a bring-up pad (ADC6) as a
fallback, not a design path.

### Where the sensor sits

The magnet turns on the drum axis at the far (non-motor) wall: y = 0, z = 18.5 from the
display plane (#673). The chip face must look at it along the axis, coaxially to a
fraction of a millimetre. The unit board lies in the x-y plane at z >= 54, 35 mm or more
away and turned 90 degrees, so a chip soldered to it cannot see the magnet. Nothing fits
outboard of the far wall (the 4.5 mm between neighbouring drums is 3.2 mm of walls and
gaps already, #673 section 6), so the sensor sits inboard, inside the hub (#673 section 7).

| Option | Verdict |
|--------|---------|
| Sensor on a peninsula or rigid-flex tail of the unit board | Rejected: the tail has to cross the 0.4 mm drum-to-wall gap and end in a stiffened flex that holds the chip centred; rigid-flex adds cost for no gain. |
| **Small sensor board and a FFC tail** | **Recommended.** A round Ø8.4 mm, 0.8 mm board (AS5600 SOIC-8, 1 uF on its 3V3 pin, 100 nF) at the tip of a fixed post inside the hub; a 5-way 0.5 mm-pitch FFC (5 V, GND, SDA1, SCL1, OUT), ~60 mm, leaves the post and runs in a groove of the far wall to a 5-pin FPC connector J3 on the unit board. The same topology as the hall tail it replaces. The TWI1 pull-ups (R9, R10, 4.7 kohm) sit on the unit board. |

### What an absolute encoder does to the firmware (listed, not implemented)

Today `drumPosition` counts steps past the hall's entering edge and is resynced at every
observed edge (#263); a unit that does not know its position seeks up to three
revolutions for the edge (`HOMING_RPM` 10, ~18 s). With the encoder:

1. **Home becomes a read.** `calibrate()` reads the angle, converts it to steps (angle x
   `STEPS` / 4096), sets `drumPosition` and moves to the target; no seek in the normal
   case. The boot-home stagger (`BootHomePolicy`, #309) and its inrush mostly go away; a
   seek stays as the recovery for an encoder fault.
2. **Per-assembly zero.** The magnet sits at an arbitrary rotation in the drum, so each
   unit stores a zero (raw angle to home flap). That is a new EEPROM field, and **the
   EEPROM layout is per-device truth** (`firmware/v2/Unit/CLAUDE.md`: migration and
   invariant rules, layout version, the factory image built from `UnitEeprom.h`), so the
   byte assignment belongs to the firmware issue, not to this spec. The existing
   calibration offset keeps its meaning relative to the zero.
3. **`UnitIdleHall.h` is replaced** by an idle-angle check. The position is known at every
   instant, so a drum turned by hand at rest is caught at once (the hall's window covered
   ~2.3 % of a revolution) and the WRONG-WINDOW false positive disappears with the window
   model. Pure logic in a header with a native test; the pin sampling stays glue.
4. **`UnitHomePolicy.h` and the #667 stand-down change trigger, not purpose.** "The hall
   never fired" cannot tell a dead sensor from a jammed drum; the encoder can. No magnet
   (status bits) means no seek and no drive, a flag instead; an angle that does not change
   while stepping is a stall, seen within tens of steps instead of after a full failed
   seek, and one detection stands the unit down instead of three seeks. The backoff
   constants stay for a failed seek that did read the sensor.
5. **Wire contract.** The protocol and the HA contract are append-only. The hall-named
   fields (`statusHallNeverTriggered`, `selfTestFirstHallWindow` / `LastHallWindow`,
   `extHallEdgesLastRev`, `idleHallStoodDown` and their readers in `UnitSelfTest.h`,
   `UnitExtDiag.h`, `UnitLifetime.h`, `UnitFactsWidest.h`, `UnitHealth.h` and the Master's
   `UnitVerdict.h`, `UnitFactsJson.h`, `UnitApiJson.h`, `UnitEventLog.h`) keep their
   positions and carry a defined "not applicable" value on this board; new append-only
   fields report the raw angle, the encoder status and the AGC. The exact list is the
   first task of the firmware issue (a case-insensitive `hall` grep over `Unit/`,
   `shared/` and `Master/`).
6. **Drift (#266).** The mod-2038 sync stays; the encoder gives the true position at any
   time, so drift becomes a measurement instead of an inference between edges.

All of it changes `Unit/` and `shared/`: restage the unit bundle (CLAUDE.md step 4).

## 6. Row backplane, connector and level shifting

The ATmega328PB at 16 MHz needs ~5 V (running it at 3.3 V forces 8 MHz and breaks
the 16 MHz firmware and twiboot). **The unit bus stays 5 V.** Decided 2026-11-20: the
units have no cables. Each row has a **backplane**, a passive PCB the units plug into:
12 V, GND, SDA, SCL and the chain run along it. The only cable left is the **row feed
cable** from the controller box to the first backplane segment (five conductors, +12 V,
GND, SDA, SCL, CHAIN; #675), kept <= 0.5 m.

- the unit has **no pull-ups and no level shifter**; the controller (#675) owns the 5 V
  pull-ups of each row bus (this removes the v1 BSS138 pair and four 10 k resistors from
  every unit);
- the bus runs at 100 kHz (twiboot reflash is only safe at 100 kHz);
- **plug:** one 6-pin plug per unit board: +12 V, GND, SDA, SCL, CHAIN_IN, CHAIN_OUT. The
  backplane's 12 V, GND, SDA and SCL are through lines with a stub to each socket; it
  wires CHAIN_OUT of position k to CHAIN_IN of position k+1. The plug carries only the
  unit's own current (~0.2 A moving), not the row's ~3 A: a pulled unit does not
  interrupt power or I2C for the units behind it. **Type: a 1.27 mm pitch 1 x 6 pin
  header on the unit board and an SMD socket on the backplane** (0.2 A per pin is far
  inside the rating); part numbers `TBD-verify`. Spring-contact (pogo) pins on the
  backplane against pads on the unit board's rear face are the alternative if the
  stack-up tolerance is too tight: they tolerate it and need no tall socket;
- interface with #675: the row feed cable enters the first segment and the controller's
  CHAIN head drives that segment's first CHAIN_IN.

### Geometry and registration

The backplane lies behind the frames' back walls, in the ~6 mm strip behind the ~95 mm
unit envelope (#675: 101 mm case); the unit board's plug reaches it through a slot in the
frame's back wall (#673 section 6). Stack budget from the back wall's outer face: the
backplane 1.6 mm plus an SMD 1.27 mm socket of ~4 mm (datasheet-verify the mated
height), ~5.6 mm of the 6 mm, which leaves ~0.4 mm; the pin length follows from the board
standoff, the 1.2 mm wall and the socket. If the mated height does not fit, the pogo
alternative above is the fallback. The backplane also sets the column pitch: each frame registers on it with two
pegs or M2 bosses (one round hole, one slot along y), so the 41.5 mm pitch is the board's
hole tolerance (+-0.1 mm) instead of the sum of 15 print tolerances.

### Segments (15 x 41.5 mm = 622.5 mm per row)

| Option | Length | Verdict |
|--------|--------|---------|
| One piece | 622.5 mm | no: over JLCPCB's ~500 mm standard FR-4 limit (datasheet-verify on the capabilities page at order time; assembly has a smaller envelope still) |
| 2 segments (7 + 8) | 290.5 / 332 mm | possible, but two board designs |
| **3 segments of 5** | **207.5 mm** | **recommended:** one design for all 9 segments of the wall, two joints per row |
| 5 segments of 3 | 124.5 mm | four joints per row, more than the length saving is worth |

The joint between segments is a board-to-board link with 12 V and GND on two conductors
each (3 A at the first joint) plus SDA, SCL and CHAIN: 7 conductors, family
`TBD-verify` (2.54 mm pin and socket pairs are typically rated ~3 A per pin; check).
CHAIN crosses a joint as one conductor (CHAIN_OUT of the 5th unit to CHAIN_IN of the
6th). Copper: 2 layers, 2 oz; a 10 mm 12 V pour over 622.5 mm is ~15 mohm, so the last
unit sees ~25 mV of drop per leg (the load falls along the row).

### I2C budget with the new parts

Each unit adds to the row bus a socket stub, a series resistor (47 ohm) and an ESD
diode on SDA and SCL, 15 times per row. The 400 pF budget (#675) assumed ~15-20 pF per
unit, so the ESD part must be **<= 3 pF per line**; the backplane trace adds a few pF
per 41.5 mm pitch (estimate). The series resistor sits in the stub, not in the through
line, so it does not accumulate. The budget is re-measured with a scope on the first
assembled segment (#675 risk 1), not trusted from this estimate.

## 7. Programming pads: Tag-Connect and the day-0 burn

A **Tag-Connect TC2030-class footprint** (six pads at 1.27 mm and alignment holes, no
connector on the board; pinout per the cable's AVR ICSP adapter, verify) carries
MISO/MOSI/SCK/RESET/GND plus VCC behind an open solder jumper. It costs no part and no
height. One burn per board, the same image for every board (a blank chip has no
bootloader to update over I2C): twiboot, the unit sketch, the fuses and a blank-address
EEPROM image in one session. After it the address, the sketch and the bootloader all
update over I2C. Notes:
- PB3 (MOSI) is the first stepper pin and PB5 (SCK) the LED, as on the Nano.
  Use a >= 1 k LED resistor; the ULN2003A input sees MOSI toggle during the burn,
  which is harmless with the 12 V absent. Power the jig through the plug pins or the
  VCC jumper, not both (section 3, backfeed).
- **Day-0 fuses: `LFUSE=0xFF`, `HFUSE=0xD2`, `EFUSE=0xFD`.** HFUSE bits (0 = programmed):

  | bit | 7 RSTDISBL | 6 DWEN | 5 SPIEN | 4 WDTON | 3 EESAVE | 2 BOOTSZ1 | 1 BOOTSZ0 | 0 BOOTRST |
  |-----|---|---|---|---|---|---|---|---|
  | value | 1 | 1 | 0 | 1 | **0** | **0** | **1** | 0 |

  `1101 0010` = **0xD2**. BOOTSZ = 01 is a 1024-word (2 KB) boot section, starting at
  byte 0x7800 (the 1 KB section of 0xDC starts at 0x7C00); BOOTRST programmed
  resets into it. **EESAVE programmed** keeps the EEPROM through chip erase, so an
  ICSP reflash does not lose the address. Why 2 KB: the app is 20.5 KB of the
  remaining 30 KB, and the lean 1 KB twiboot image has 2 B of headroom, no room for
  the chain gate and the address read. BOOTSZ can only be changed over ICSP, so the
  2 KB layout is permanent for the board; the image must be validated against it
  (section 9).
- Two test points on PD0/PD1 for `SERIAL_ENABLE` debug builds.

## 8. Current budget (estimates, verify on the bench)

| Load | Rail | Estimate |
|------|------|----------|
| 24BYJ48 12 V, two coils energised (120 ohm per phase from the listing) | 12 V | ~0.2 A, ~2.4 W while moving, 0 idle |
| ATmega328PB at 16 MHz, 5 V | 5 V | ~15 mA |
| Encoder (AS5600 class, datasheet-verify ~6.5 mA) + TWI1 pull-ups + LEDs | 5 V | ~10-15 mA |
| 12 V sense divider (section 10) | 12 V | ~92 uA, continuous |
| Buck input, logic only (85 % efficiency) | 12 V | ~15-20 mA |
| Row of 15, all moving | 12 V | ~3 A, ~36 W |

The ~3 A row worst case is accepted (decided). It runs on the backplane's copper, not
through the plugs (section 6). The render stagger limits how many units move at once and
**homing is staggered the same way** (the unit's self-home deadline uses the address
index, section 9): homing all 15 at once is the worst case (the 16-unit homing inrush is
the known brownout source on the S3 leader). With the absolute encoder a boot normally
needs no seek, so that worst case shrinks to the recovery path (section 5). Size the
per-row 12 V feed and the backplane copper for 3 A. For a 5 V-motor candidate the buck
carries that motor's current as well (datasheet value not available: the 15BY25-560A
listing has no coil data).

## 9. Addressing: a chain line and EEPROM-held addresses

The unit has no DIP switch; the address lives in EEPROM and is **assigned by the
controller in physical order** over a chain line. Address = position. Normal
operation never depends on the chain: an addressed unit answers at its address
whether or not the chain is intact.

### What exists today (verified)

- `getaddress()` (`UnitI2CProtocol.ino`): resolves the identity block
  (EEPROM bytes 0..3: version, address, flag, checksum, via
  `unitEeIdentityAddress`); anything invalid falls back to `SFP_I2C_ADDRESS_BASE
  + DIP`. With PD3..PD6 unconnected the pull-ups read DIP 0, so a DIP-less
  board's de facto default is **0x01**, the address of unit 0.
- twiboot (`UnitBootloader/main.c`) never reads EEPROM for its address; it
  reads PD3..PD6. With `I2C_ADDRESS_BASE == 0` it falls back to stock
  `TWI_ADDRESS` 0x29.
- The in-system update image (#499, `prebuilt/twiboot-new-progmem.h`, built
  `-DI2C_ADDRESS_BASE=1`) also reads the DIP, is 894 B of the 896 B of pages
  0..6 of a 1 KB section at 0x7C00, and has no EEPROM access (`SF_NEW_TWIBOOT`).
  Its stage 1 drives the fielded image's own write handler at addresses locked by
  `tests/test_twiboot_entry_points.py`; page 7 holds `do_spm` (design:
  `2026-10-02-in-system-twiboot-update-design.md`).
- Day-0 init `eeInitIfBlank()` keys on EEPROM byte 0 (the layout version, also
  the identity block's first byte) and **clears the identity block**.
- `SET_I2C_ADDRESS` (0x94, address + complement) exists end to end: unit,
  `unitBusSetAddress`, the display task's `SetAddress` op and the wall op
  `OPC_SET_ADDRESS`; the master guards it behind a sketch-running unit at the
  address (range 1..126). It is unverifiable after the fact.
- The master scans only `toI2cAddress(0..maxUnits-1)`.

### The chain line

**Pins: CHAIN_IN = PD3 (D3), CHAIN_OUT = PD4 (D4).** PD3..PD6 are free once the DIP
goes; PD3 is INT1, so CHAIN_IN changes raise an interrupt instead of being polled.
Neither pin is on ICSP (PB3..PB5, RESET), TWI0 (PC4/PC5), the stepper (PB0..PB3), the
encoder bus (PE0/PE1) or the debug pads (PD0/PD1); PD2 (INT0) stays spare.

- Active high, 5 V. CHAIN_IN has a 1 k series resistor and a 100 k pull-down: an
  empty slot, an unpowered neighbour or a unit in reset reads **deasserted**
  (the chain fails closed). CHAIN_OUT is a plain push-pull output; the MCU in reset or
  blank leaves it Hi-Z, which the next unit's pull-down reads as deasserted.
- Wiring: the unit has no cable. Its plug carries +12 V, GND, SDA, SCL, CHAIN_IN and
  CHAIN_OUT (section 6); the backplane runs the bus lines along the row and wires each
  position's CHAIN_OUT to the next position's CHAIN_IN. The CHAIN level is slow DC;
  no more than 2 firmware samples of debounce.
- The first unit of a row has its CHAIN_IN on the controller's CHAIN head for that
  row (one GPIO per row through a 5 V buffer, #675), carried by the row feed cable's
  CHAIN conductor into the first backplane segment. The head is held high while the
  row is powered and low when it is off.

**Unit rule (sketch and twiboot alike):**

| State | Answers I2C | CHAIN_OUT |
|-------|-------------|-----------|
| ADDRESSED (valid pair in EEPROM) | at its own address, always | = CHAIN_IN |
| UNADDRESSED (no valid pair, or CHAIN_CLEAR) | at 0x29 **only while CHAIN_IN is high** | low |

An unaddressed unit blocks the chain behind it, so on a row at most one unit,
the first unaddressed one, ever sees CHAIN_IN high and listens on 0x29: blank
units do not collide, in the sketch or in twiboot. A unit is ADDRESSED after a
verified `SET_I2C_ADDRESS` at 0x29; a **general-call `CHAIN_CLEAR`** puts every
unit in UNADDRESSED in RAM only (the EEPROM pair stays; a reset restores it).
Dropping the head low clears CHAIN_OUT down the whole row; raising it ripples it
back through the addressed units in a few milliseconds.

The gate lives in both programs. The sketch drives TWAR from an INT1 handler (and
re-arms it with the existing TWI heal path, which also re-arms the general-call
bit). twiboot polls CHAIN_IN in its main loop, honours the general call, answers at
the stored address when it has one and asserts CHAIN_OUT by the same table, so a
blank unit sitting in the bootloader no longer collides at 0x29.

### Enumeration (run by the controller, per row)

Preconditions: the row is powered and has settled (>= 2 s: boot window plus start),
the mux channel selected.

1. Head low (every CHAIN_OUT in the row falls).
2. Broadcast `CHAIN_CLEAR` by general call, three times, for loss tolerance.
3. Head high: only unit 1 sees CHAIN_IN high and listens on 0x29.
4. For position p = 1..N (N configured for the row, 15), address a = the row's first
   address + p − 1 (the global index, 1..45 over three rows):
   a. probe 0x29; no ack means the chain ended or broke: stop and report
      `chain_break_after = p − 1`;
   b. `SET_I2C_ADDRESS(a)` to 0x29; the unit writes the pair only if it differs, switches
      TWAR to a and drives CHAIN_OUT = CHAIN_IN;
   c. verify with a ping at a (the round trip, not an ack of the write); wait >= 5 ms for
      the ripple before the next probe.
5. After N units probe 0x29: an answer means the row holds more than N units; report
   `row_long`, leave the extra unit unaddressed.
6. Scan 1..N and check each unit's identity reply.

An address-keyed state (the unit's self-home stagger, `BootHomePolicy`) starts at
the moment the unit becomes ADDRESSED, so staggered homing follows position.

### Events

| Event | What happens |
|-------|--------------|
| First power-up, row all blank | No unit is addressed: the controller runs the full enumeration automatically. |
| Normal power-up | Stored addresses, the chain ripples through, the controller only scans and checks identity. No enumeration. |
| **Swapped** unit (a blank board replaces unit k) | Units 1..k−1 ripple, the blank blocks, units k+1..N keep their addresses and work. The scan shows address k missing and 0x29 answering: **exactly one gap and one listener** is assigned automatically (`SET_I2C_ADDRESS(k)`, verified). Anything less clear raises `row_enumeration_needed`. |
| **Added** unit (inserted or appended, row now N+1) | The newcomer is blank and blocks; no free address in the row's range: the controller reports `row_long` and waits for the operator to raise the row size and run a full enumeration. |
| **Removed** unit | Its address stops answering, the chain breaks at the empty slot, units behind it keep working at their addresses. Reported as absent (as an absent unit today); no renumbering, because slots are fixed. A new unit in that slot is the swap case. |
| Duplicate address (a used board from elsewhere) | Collision shows as scan failures; a full enumeration heals it (`CHAIN_CLEAR` reaches both). |
| Unit stuck in twiboot (app blank) | Same rules: addressed ones answer at their address, blank ones are gated at 0x29, so enumeration and reflash both work. |

Full enumeration is an operator action (and the first-boot case), never a side
effect of a scan. Reflash is unchanged: enter twiboot at the unit's own address;
twiboot then listens at the stored address, so no two units share 0x29 and the
one-unit-at-a-time constraint on the bootloader address no longer applies.

### How twiboot gets the address

The dedicated pair (variant B): two EEPROM bytes in the reserved area (36 and 37 by
the current constants, after the bus record at 28..35, ring at 64; the builder
confirms), `addr` and `~addr`, valid when `b == ~a` and the address is in range.
The app writes it on `SET_I2C_ADDRESS`; the app and twiboot resolve it by the same
two-byte rule, so they cannot disagree on a torn write. The existing identity
block stays for the v1 builds, and the **factory EEPROM image leaves the pair
blank (0xFF)**. Day-0 init never touches the pair.

### The note board's twiboot (2 KB section)

The current in-system image is tied to the 1 KB layout (base 0x7C00, pages 0..6,
`do_spm` page 7 at 0x7F80, stage 1 locked to the fielded image's addresses). The
note board gets **its own image and update variant**, selected by `SF_NOTE_BOARD`:

- Linked at `0x7800`: pages 0..14 (0x7800..0x7F7F, 1920 B) for twiboot, page 15
  (0x7F80) for `do_spm` plus the generation marker at 0x7FFE. The image is **born with
  `do_spm` installed**, so there is no stage 1.
- Content: the address pair read, the chain gate, general-call `CHAIN_CLEAR`, no DIP
  block; the lean image's MCUSR stash and bounded pin stay. Size is unmeasured (the
  fielded image is 890 B, the budget 1920 B): measure before layout is frozen.
- `make_new_twiboot.py` takes the base and page count per variant, with its own
  prebuilt hex and generated PROGMEM header; the simavr proof is rerun on it.
- The note unit sketch embeds **that** header. `BootSectionClassify` gains the
  variant's CRC over 2 KB and `GET_BOOT_INFO` hashes 2 KB on this board. Stage 2
  rewrites pages 0..14 by the existing trampoline protocol (page 0 at 0x7800).
- Guard: stage 2 refuses unless the fuse read-back (`GET_BOOT_INFO`) shows BOOTSZ = 01
  (HFUSE 0xD2); a 1 KB image must never reach a 2 KB board nor the reverse, and each
  build knows only its own CRC.

### Firmware and master changes this implies (not implemented here)

1. **Board variant** (`unit_note` env, `-DSF_NOTE_BOARD`): pin map (driver, encoder on
   TWI1, no hall, no DIP, CHAIN_IN/OUT on PD3/PD4, 12 V sense on ADC7), address from the
   pair, the gate, `CHAIN_CLEAR`, boot-home stagger from the address. `Unit/` and `shared/`
   change, so the **unit bundle must be restaged** (CLAUDE.md step 4).
2. **twiboot note image** and update variant (above): `make_new_twiboot.py`,
   `prebuilt/`, simavr proof, `BootSectionClassify`, `BootUpdateAvr.h` page count.
3. **Day-0 init** must not clear a valid pair, and the factory image must not look
   blank to it (byte 0 doubles as identity version): the image is generated by a
   host tool built from `UnitEeprom.h`, not a second checksum implementation.
4. **Protocol:** `CHAIN_CLEAR` is a new general-call op appended to
   `SplitFlapProtocol.h` (opcode chosen from the unused range by the builder; additive,
   check the `SFP_PROTOCOL_VERSION` rule). `SET_I2C_ADDRESS` rejects 0x29 as a target,
   is accepted at 0x29 only while gated, and the controller requires the verified
   round trip.
5. **Controller (Master firmware on #675):** one CHAIN head GPIO per row, the
   enumeration job as a pure state machine in a header with native tests (sequence,
   single-gap auto-assign, `row_long`, `chain_break_after`), an operator action
   "enumerate row", and the `addressStored` flag always true on this board; page
   wording that says "falls back to DIP" needs a note-build text.
6. **Burn script** (`flashing/`): one identical image for every board; order: fuses
   (`lfuse 0xFF`, `hfuse 0xD2`, `efuse 0xFD`), chip erase, flash (twiboot at 0x7800 plus
   sketch), EEPROM image, fuse read-back check.
7. **Encoder** (section 5): `calibrate()` becomes an angle read, the stored zero (EEPROM
   migration), the idle-angle check replacing `UnitIdleHall.h`, the stall detection feeding
   the `UnitHomePolicy.h` stand-down (#667), the hall-named wire fields kept and defined as
   not applicable, new append-only angle/status/AGC fields. `Unit/` and `shared/`: restage.
8. **12 V sense** (section 10): `readRailMv()`, a new `rail` wire field, a board-aware
   supply-wait threshold (survey) and a Master threshold for the new field.
9. **328PB** (section 12): the `unit_note` env with `platform_packages` pinned, the twiboot
   aliases and guards, `SPMEN`, `shared/` signature check, a bundle with two images.
10. **Serial number** (section 12): a `GET_SERIAL` op (additive), the facts field, the
    burn-script log, the controller's position-to-serial record.

## 10. 12 V sense and the supply hold (#505)

The 5 V rail is a buck output here, so `readVccMv()` (the 1.1 V bandgap read against
AVcc, `UnitMotion.ino`) stays at ~5 V whatever the 12 V motor rail does. On the Nano the
motor and the chip shared one rail and `UnitSupplyWait.h` (`SUPPLY_WAIT_DROP_MV`, a dip
against the unit's own idle level, bounded by `SUPPLY_WAIT_MAX_MS`) saw the neighbours'
sag; on this board it would never fire. The unit therefore reads the 12 V node itself.

- **Divider:** R7 120 kohm (12 V side) and R8 10 kohm to GND, 100 nF (C10) at the pin,
  to **ADC7** (pin 22, datasheet-verify). Taken **after Q1**, the node the motor and the
  buck see (a reversed unit reads 0). Ratio 1/13: 12 V gives 0.92 V; full scale 14.3 V.
- **Reference:** the internal 1.1 V (`REFS1:0 = 11`), so the reading does not depend on
  the buck's 5 V: 12 V is ~839 counts, ~14 mV per count at 12 V. The 9.2 kohm source is
  carried by C10; the first conversion after the reference switch is discarded and the
  reference gets the same settling wait as the bandgap read. The reference voltage and
  the divider tolerance differ per chip, which does not matter: the rule is relative to
  the unit's own idle level, like the 5 V one.
- **Threshold:** not 300 mV scaled to 12 V. `UnitSupplyWait.h` derived 300 mV as twice
  the largest dip recorded across 21 units (survey of 2026-10-09); the same method sets
  the value for this board: record the idle-level dips on a prototype row through homes
  and full frames, take twice the largest. `SUPPLY_WAIT_MAX_MS` stays; a rail that
  stays low must not park the wall.
- **Reporting:** `vccMin` and `vmin` are in the HA contract (`docs/HA_CONTRACT.md`,
  append-only) and the Master's sag-adaptive budget compares `vmin` with the 5 V
  constant `MOTION_SAG_LOW_MV` (`shared/MotionBudget.h`). So the 12 V reading is a
  **new** append-only unit field (provisionally `rail`), not a redefinition of `vmin`;
  `vmin` keeps the bandgap reading. On the controller the row's INA226 bus voltage is the
  primary sag input (#675); the unit's `rail` is the far-end check (backplane and plug
  drop).
- **Firmware:** `readRailMv()` and its settle logic in `UnitMotion.ino`; the supply
  wait and the vitals use it under `SF_NOTE_BOARD`; a new wire field and a board-aware
  threshold on the Master. `Unit/` and `shared/` change: restage the bundle.

## 11. Protection and service parts

| Part | Value | Why |
|------|-------|-----|
| Series R on SDA and SCL | 47 ohm in the stub (not in the through line), starting value | limits ESD and hot-plug current into the pins; does not accumulate over 15 units; scope on the first segment |
| ESD array at the plug | 4-line, 5 V working voltage, **<= 3 pF per line** | SDA, SCL, CHAIN_IN, CHAIN_OUT; the capacitance criterion keeps the row inside the I2C budget (section 6) |
| TVS D1 on 12 V | as before (section 3) | hot-plug and motor spikes |
| Bulk capacitor at the 12 V input (C11) | 47 uF 25 V class, <= 5 mm tall | local reservoir for the motor start; 15 units are ~700 uF per row, charged by the eFuse's dV/dt ramp (#675); C1 (330 nF) stays as HF decoupling |
| Status / identify LED D2 | PB5 as before | blink codes from firmware (identify, encoder fault, stand-down) |
| Power LED D3 | on the 5 V rail, 4.7 kohm | shows the buck is alive when the MCU is not; ~0.6 mA, 27 mA over 45 units |
| Tag-Connect footprint | section 7 | programming without a connector on the board |
| Test points | PD0/PD1, 5 V, GND | debug and bring-up |

Hot-plug: the plug has no make-first ground, so a unit is swapped with the row off (the
controller's per-row eFuse, #675 service mode).

## 12. ATmega328PB (decided 2026-11-20): what changes, serial number, toolchain, twiboot, go/no-go

**Verified locally (2026-11-20, scratch builds, nothing run on hardware):**

- avr-libc `iom328pb.h` in the pinned `toolchain-atmelavr` (avr-gcc 7.3.0): signature
  **1E 95 16** (the 328P is 1E 95 0F); flash 32 KB, page 128 B; EEPROM `E2END` 0x03FF
  (1 KB, as the P); `RAMEND` 0x08FF (the #542 crash-record bytes are unaffected);
  SPMCSR at I/O 0x37, as the P (`do_spm.S`'s `out 0x37` stays valid); TWI0 registers at
  the same addresses as the P but **named `TWBR0`, `TWSR0`, `TWAR0`, `TWDR0`, `TWCR0`,
  `TWAMR0`**; a second TWI (TWI1); `SELFPRGEN` does not exist, the bit is `SPMEN`; port
  E (PE0..PE3) exists.
- PlatformIO: `atmelavr@5.3.0` (the pin) ships a board `ATmega328PB` on the MiniCore
  core. The stock `framework-arduino-avr` 5.4.0 also has a 328PB variant (board
  `elektor_uno_r4`) but **cannot build the sketch** (bare `TWCR`/`TWAR` undefined), so
  the board needs `platform_packages = platformio/framework-arduino-avr-minicore@3.1.2`
  as a new exact pin (#648; the platform's own range resolves to 3.1.2, the registry
  also has 3.0.2). MiniCore aliases `TWCR` to `TWCR0` in `variants/pb-variant` and has
  `Wire1` on PE0/PE1.
- Scratch build of the Unit sketch on `ATmega328PB` + MiniCore 3.1.2: the only error is
  `SELFPRGEN` in `BootUpdateAvr.h`; with `-DSELFPRGEN=SPMEN` it links at **21,918 B flash
  and 528 B RAM**, against 20,552 B and 556 B for the Nano build (MiniCore's core is
  +1,366 B). With the 2 KB boot section the ceiling is 30,720 B (0x7800), not the board
  JSON's 32,384 B: 71 % used.
- Fuses: PlatformIO's fuse calculator lists the 328PB; its EFUSE is `0xF5 | cfd` for
  BOD 2.7 V (BODLEVEL 101, as on the P), where bit 3 is the PB's clock-failure-detection
  bit. The spec's `EFUSE=0xFD` keeps that bit at 1; datasheet-verify its polarity and
  whether CFD should be programmed. The HFUSE boot-size encoding and `LFUSE=0xFF` are
  assumed unchanged: datasheet-verify.

**Pin differences (datasheet-verify the pin numbers when drawing the symbol):** on the
328P TQFP-32, pins 3 and 6 are GND and VCC; on the 328PB they are PE0 and PE1, and
pins 19 and 22 (ADC6, ADC7 on the P) are PE2 and PE3 with the same analog function. A
footprint wired from the 328P symbol would tie PE0/PE1 to the rails (harmless while they
stay inputs) and lose TWI1. Supply pins left: VCC 4, AVCC 18, GND 5 and 21, so the
decoupling is one 100 nF per supply pin plus AREF (the BOM drops one). PB, PC, PD pins
are in the same places.

**Factory serial number (datasheet-verify):** the 328PB carries a 10-byte unique serial
number in the signature row (addresses 0x0E..0x17). The sketch reads it with
`boot_signature_byte_get()` (avr-libc `boot.h`, `SIGRD` exists in `iom328pb.h`); the
repo already reads the signature row for #552. Over ICSP a recent avrdude may expose it
as a memory (verify in the pinned avrdude), so the burn script can log serial against
board. **It complements the chain, it cannot replace it:** a serial says which chip a
unit is, not where it sits, and blank units would still collide on 0x29 without the
gate. Uses: the controller records position to serial at enumeration, so a replaced unit
is detected by a changed serial (today only a blank swap is detected); duplicates and
inventory; the odometer follows the chip. It needs a new append-only op (`GET_SERIAL`)
and a facts field; twiboot does not need it.

**twiboot port (compiling `UnitBootloader/main.c` with the pinned toolchain):**

- With `-mmcu=atmega328pb` it fails loudly: `#error "No TWI/USI peripheral found"`,
  because `#if defined(TWCR)` is false (only `TWCR0` exists). The patch is a set of
  `TWCR`/`TWSR`/`TWDR`/`TWAR` aliases to the `...0` registers under `__AVR_ATmega328PB__`.
- Quietly: `disable_wdt_timer` (the `.init3` watchdog clear) and the DIP block are
  guarded by `__AVR_ATmega328P__` and would compile out on the PB, leaving the watchdog
  running after a watchdog reset (the enter-bootloader path resets by watchdog). The
  guard must include the PB; the DIP block is replaced by the chain gate anyway
  (section 9).
- The Makefile sets `MCU = atmega328pb`, `AVRDUDE_MCU = m328pb`; the chip-info reply
  takes its signature from the compiler's `SIGNATURE_x`, so it answers 1E 95 16.
- `shared/TwibootProtocol.h` (`isAtmega328pSignature`) and `shared/TwibootFlash.h`
  (`ChipBadSignature`, "not an ATmega328P") must accept 1E 95 16. That is a `shared/`
  edit: the unit bundle is restaged, and the bundle needs a second image picked by the
  chip signature (today one). The fielded Nano keeps 1E 95 0F.
- `BootUpdateAvr.h`: `SELFPRGEN` becomes `SPMEN` (present on the P too).
- Flash page, EEPROM size and SPM are the same; the 2 KB note image (section 9) is
  linked at 0x7800 and measured on the PB. `tests/test_twiboot_entry_points.py` locks
  the fielded P image only. Proof by emulation: whether the simavr in use models the
  328PB is unchecked; the TWI0 register addresses equal the P's, so a P core runs the
  TWI path.

**Recommendation: GO, with conditions.** (1) The twiboot patch, the `shared/` signature
change and the second bundle image are in the firmware issue before the first board is
ordered. (2) The first prototype proves TWI0 as slave and TWI1 as master together, the
serial read, the 2 KB note twiboot on the PB and the fuse read-back. (3) LCSC stock and
price are checked (not verified here). (4) Accept that the encoder interface makes the
board **328PB-only**: there is no 328P fallback without a bit-banged encoder bus.
Risks: one supplier (Microchip) for both parts; MiniCore is a third-party core under a
new pin, so the Unit build gains a second core to keep current.


## 13. Interface to the controller (#675) and the height gate

**Electrical interface, per row (one board design for all 45):**

| Item | Value |
|------|-------|
| Row feed | 5-conductor cable (12 V, GND, SDA, SCL, CHAIN) from the controller's per-row eFuse to the first backplane segment; the unit plug is 6-pin (CHAIN_IN and CHAIN_OUT separate, section 6) |
| 12 V input | one 12 V rail per row, ~3 A worst case per row (15 units), on backplane copper; each unit sees ~0.2 A moving through its own plug; the unit's bulk capacitor (C11, 47 uF) is ~700 uF per row for the eFuse's soft start to charge |
| Bus | 5 V SDA/SCL, 100 kHz, pull-ups on the controller only; <= 3 pF per line of ESD per unit |
| Chain head | the controller drives the first CHAIN_IN high while the row is powered and settled (>= 2 s), low when off or re-enumerating (section 9) |
| Service | a unit is swapped with its row off (no make-first ground on the plug) |

**Power per moving unit (the number the controller sizes with):** ~2.6 W on the 12 V
rail: ~2.4 W motor (24BYJ48 12 V, 120 ohm per phase from the supplier listing, two
coils energised, 12 V x 0.2 A) plus ~0.2 W of logic through the buck (~25 mA at 5 V,
85 % efficiency). The ULN2003A's saturation drop makes the motor a little lower (~2.1 W)
and also dissipates ~0.3-0.4 W in the driver itself. **This is an estimate from a
datasheet-class listing, not a measurement**; a 4 W per-unit ceiling is also unmeasured.
Idle is ~0.15 W per unit. 45 x 2.4 W = 108 W is consistent with this (117 W with the
logic).

**Height gate for every unit board:** a mains PSU module lies over the unit boards of
adjacent bays (controller spec, UHP-200-12 class: 194 x 55 x 26 mm), with a 1 mm
insulating sheet between. Constraint for all boards: **the tallest part on the PSU-facing
face is <= ~8 mm above the board.** Assumption (tell me if wrong): the PSU is on the
mechanism side of the board and the plug side faces the back wall, so the plug and the
backplane are not under the PSU.

| Part (PSU-facing face) | Height | Verdict |
|------------------------|--------|---------|
| Buck inductor (BOM: <= 2 mm class) | ~2 mm | passes |
| Bulk capacitor C11 (BOM: <= 5 mm) | <= 5 mm | passes; this is the tallest part |
| TQFP-32, ULN2003A SOP-16, crystal, ESD, passives | 0.5 to 1.75 mm | pass |
| FPC connector J3 for the encoder tail | ~2 mm; the FFC leaves toward the far wall, not toward the PSU | passes; keep the FFC fold flat |
| Plug header (1.27 mm 1 x 6, body ~2.5 mm, pins through the wall slot) | on the rear face | outside the gate if the assumption holds; if the PSU is on the plug side the header pins (~6 mm) still pass the 8 mm gate |
| AS5600 sensor board | inside the hub, not on the unit board | not affected |

**Depth stack (z from the display plane, mm; envelope 101):**

| Layer | z from | z to | Note |
|-------|--------|------|------|
| Front wall, mechanism, flap tips | -1.2 | 53.1 | #673 section 6 |
| Clearance (flap tips) | 53.1 | 54.1 | 1.0 |
| PSU module (UHP-200-12 class, 26 mm) | 54.1 | 80.1 | over the boards of 5 top-row bays |
| Insulating sheet | 80.1 | 81.1 | 1.0 |
| Unit-board components (gate <= 8 mm; C11 is the tallest at <= 5) | 81.1 | 89.1 | gate height reserved in full |
| Board | 89.1 | 90.7 | 1.6 |
| Rear standoff: header body (~2.5), rear-face parts | 90.7 | 93.8 | 3.1 available |
| Back wall (1.2) | 93.8 | 95.0 | plug slot |
| Backplane strip: SMD socket (~4) + backplane (1.6) | 95.0 | 100.6 | 0.4 spare in 101 |

It closes: 100.6 of 101 mm, with **0.4 mm spare at the back** and 3.1 mm of rear standoff.
Consequences: rear-face parts other than the plug header are <= ~2.5 mm; anything tall
(C11, inductor) sits on the PSU-facing face; the header's pins must reach the socket
across standoff + wall + insertion (~6.8 mm of pin below the board, datasheet-verify
availability; the pogo alternative in section 6 avoids it). Bays without a PSU above
them have 26 mm more room, but one board design is used everywhere.

Nothing found that cannot meet the gate; the budget is set by C11 and must be held in the
BOM when its part is chosen. Heat: a board dissipates ~0.4-0.6 W while moving (ULN2003A,
buck) and ~0.15 W idle, so under a PSU lid the tolerable assumption is no airflow; the
controller spec owns the module's own dissipation. Verify on the prototype with the
real PSU and its sheet.

## Decided by Lucas (2026-11-20)

- Bare ATmega328PB at 16 MHz, Unit firmware, I2C slave and twiboot kept.
- No DIP switch: the address lives only in EEPROM, assigned in physical order over
  a chain line (CHAIN_IN/CHAIN_OUT, routed through the row backplane); the gate lives in the
  sketch and in twiboot; a swapped unit re-enumerates.
- One ICSP burn per board with one image for all boards; day-0 fuses 2 KB boot
  section and EESAVE programmed; the note board has its own twiboot image and
  in-system update variant.
- No ESP-01S on the unit board; the controller (#675) drives the units and the heads.
- 12 V bus with a per-unit buck; MOSFET reverse-polarity protection per unit.
- ~3 A row worst case, homing staggered like rendering (an encoder makes most boots seek-free).
- Fit the ~41.5 x 73 x ~95 mm envelope; outline from #673.

**Round 2 (2026-11-20):**

- 12 V sense divider to an ADC pin, for the #505 supply hold (section 10).
- AS5600-class magnetic encoder and a diametric magnet on the axle end replace the
  hall (section 5); the A3144 is gone from the BOM.
- Per-row backplane replaces the unit cables (section 6).
- Protection and service parts: series R on SDA/SCL, ESD at the plug, bulk capacitor,
  status LED, Tag-Connect pads (sections 7 and 11).
- ATmega328PB instead of the 328P: go with conditions after the toolchain and twiboot check (section 12).
- Motor driver stays the ULN2003A (no TMC2209).

## My calls (correctable)

- TQFP-32 for the first batch; crystal over resonator; buck over LDO.
- The unit bus stays 5 V; pull-ups and the level shifter live on the controller.
- CHAIN_IN = PD3, CHAIN_OUT = PD4 (section 9); ripple rule (CHAIN_OUT = CHAIN_IN when
  addressed) so only one blank unit listens on 0x29; single-gap swap assigned
  automatically, everything else an operator action.
- Address pair `addr`/`~addr` read by both twiboot and the app; default 0x29 gated.
- HFUSE 0xD2; twiboot linked at 0x7800 with `do_spm` born in the last page.
- ICSP VCC behind an open jumper; the jig powers through the plug pins.
- Encoder on I2C through the 328PB's second TWI (PE0/PE1), a small sensor board on a
  FFC tail, analog OUT only as a bring-up pad; the board is therefore 328PB-only.
- Backplane behind the back walls in 3 segments of 5 per row, a 6-pin plug per unit
  (CHAIN_IN and CHAIN_OUT separate), frames registered on the backplane.
- 12 V sense: 120 k / 10 k divider, 1.1 V reference, ADC7; the hold threshold comes
  from a survey, not from scaling the 5 V number.
- ATmega328PB: GO with the conditions of section 12.

## Left to the builder

- Schematic and layout in EasyEDA, LCSC part numbers for every `TBD-verify` line
  (no number in the BOM was invented), pick-and-place, a reviewer pass.
- Measuring the note twiboot image and the EEPROM offsets (36/37 are by the
  current constants).
- The buck and its inductor, the MOSFET, the encoder and its magnet, connectors:
  datasheet-check every figure marked estimate.

## Open questions for Lucas

1. **Motor:** is the 24BYJ48 12 V the final choice? That keeps the unchanged
   firmware and a motor straight from 12 V; a 5 V motor makes the buck carry it.
   Does a 1/16 batch need rejecting on arrival (`STEPS` is a protocol constant)?
2. **Motor side:** which side wall carries the motor? The encoder post, the sensor
   tail and the far-end bearing go on the other wall (#673 sections 3 and 7).
3. **Encoder mounting:** is the proposal in section 5 (a printed non-magnetic post
   from the far wall carrying a small sensor board inside the hub, a 0.5 mm FFC in a
   groove of the far wall) the direction? It replaces the 3 mm steel far-end pin and
   is the one mechanical part that does not yet close with margin (#673 section 7).
4. **Slot-coded backplane instead of the chain?** With a backplane, four or five
   position-strap pins per socket would give every unit its address from its slot
   (no enumeration, no chain gate in twiboot). It costs those pins on the plug and
   per-slot jumpers on the backplane, and it changes the 2026-11-20 chain decision, so
   it is only raised here, not adopted.
5. **EEPROM field for the encoder zero:** the layout is per-device truth (Unit
   CLAUDE.md); the migration and the byte assignment belong to the firmware issue.
   Confirm that a layout-version bump on the note board is acceptable.
6. **Two unit images in one bundle** (Nano 328P, note 328PB, picked by the chip
   signature) and the `shared/` signature check that allows it (section 12): a bundle
   design decision before the firmware issue starts.
7. **ATmega328PB price and stock:** not checked here; the "cheaper" premise needs one
   look at LCSC before the BOM freezes.