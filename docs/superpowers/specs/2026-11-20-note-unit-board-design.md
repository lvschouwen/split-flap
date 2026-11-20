# Note-size unit board: a compact bare-ATmega328P board — design

Issue: #674 · Epic: #676 · Status: decisions of 2026-11-20 folded in; schematic and layout follow

## Intent and why

The Note-size wall (3 rows x 15 columns, ~41.5 mm column pitch, 73 mm row
pitch, ~95 mm deep) needs a unit board that fits the unit envelope. The v1
board (`PCB/v1/`, BOM 2022-03-11) cannot: it carries an Arduino Nano module, an
L7805 in TO-220, a through-hole DIP switch, an ESP-01S socket, a BSS138 level
shifter and JST XH connectors. (The v1 BOM already uses an SMD ULN2003A, SOP-16;
only the Nano, L7805, DIP switch, ESP-01S and the XH connectors are through-hole
or module parts.) This spec fixes the architecture and the draft BOM
(`PCB/v2-note/BOM-draft.csv`) so schematic and layout can start once #671
(motor) and #673 (outline, hall position) land.

This is the third project of the wall: #670 flaps, #671 drive, #673 mechanics,
this board, then the firmware alphabet. `PCB/v1/` stays the deployed board.

## Goals

- One board per unit: ATmega328P, motor driver, hall, 12 V to 5 V buck, reverse-polarity
  FET, I2C and chain in and out, ICSP pads. Nothing else.
- The existing Unit firmware, I2C slave and twiboot carry over with a board
  variant (pin map, address source), not a rewrite.
- Assembled by JLCPCB from LCSC parts, like v1.
- A target outline that fits behind the mechanism inside 39.1 x 70.6 mm (#673).

## Non-goals

- The controller board, the 3.3 V/5 V shifter and row-link wiring (#675).
- The motor decision (#671) and the hall/outline numbers (#673): this spec
  designs for the three #671 candidates and takes the outline as provisional.
- Firmware changes: this spec lists them; it implements none.
- Touching `PCB/v1/`, the deployed 45-flap units or `firmware/v1/`.

## Anti-goals

- A part that needs a firmware rewrite on the first bring-up (TMC2209 is the
  explicit exception that only wins if #671 picks a bipolar motor).
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
  wall, pattern set by this board (#673 parameter `board_hole_xy`, default none).
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
- **Pins the firmware uses today:** stepper D8..D11 (PB0..PB3), hall D7 (PD7,
  plain `INPUT`, so the pull-up must be on the board), DIP D3..D6 (PD3..PD6,
  freed: PD3 and PD4 become CHAIN_IN and CHAIN_OUT, section 9), LED D13 (PB5, `LED_BUILTIN`), I2C A4/A5, A0 floating for the
  random seed. Supply is measured against the internal bandgap, not a pin: no
  divider is needed.
- Public repo: no addresses, hostnames or device ids in any file.

## Block diagram

```
 12 V bus in --+--[TVS]--+--------------------------------+----> 12 V bus out (straight through)
 GND ----------+         +--[P-MOSFET rev-prot]--+
 SDA/SCL (5 V) ----------|-----------+----------|-------------> SDA/SCL out (straight through)
                         |           |          |
 CHAIN in --[1k]--+--[100k to GND]   |   [buck 12 V -> 5 V]
                  |                  |          | 5 V
                  +--> PD3 (CHAIN_IN)|          |
                [ATmega328P] --I2C--(slave)     +--[A3144 hall] -- OUT --> PD7
                 16 MHz, twiboot     |
                 ICSP pads, LED(PB5) +--4 GPIO--> [ULN2003A] ---> motor (12 V)
                  +--> PD4 (CHAIN_OUT) ----------------------------> CHAIN out
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
height reason to prefer QFN; switch only if placement runs out of area. Both are in the ATmega328P
family with the same pinout for the signals used (PB0..PB5, PC4/PC5, PD0/PD1,
PD7, reset, AVCC, AREF); the extra ADC6/ADC7 pads are not used.

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
| 24BYJ48 12 V + ULN2003A, or TMC2209 | raw 12 V, bypassing the buck | logic only |
| 5 V motor (15BY25) or DRV8833 (max 10.8 V, not 12 V) | the 5 V rail | logic + motor |

For the baseline the buck only feeds the 328P, hall, LED and pull-ups (~30 mA
estimated), so a small converter works. A 12 V to 5 V LDO at ~30 mA would
dissipate ~0.2 W; the issue asks for a buck, and a 5 V-motor candidate needs one
anyway, so the draft keeps a buck and sizes it per the final motor.

Buck selection rules (part left `TBD-verify` in the BOM):
- input rating at least 32 V (hot-plug spikes on a 12 V bus with a long
  daisy chain), fixed 5 V output, synchronous, SOT-23-6 class;
- continuous current >= logic load for the baseline, >= logic + motor for a
  5 V-motor candidate (decide after #671);
- a TVS on the 12 V input (also keeps the reverse-protection FET's gate-source below its 20 V limit);
- the v1 22 uF 10 V capacitor (C503893) is only valid on the 5 V node. A
  12 V input node needs a >= 25 V capacitor.
- Backfeed: with the 12 V bus absent and ICSP powered at 5 V, a synchronous buck
  can feed 5 V back up its high-side body diode onto the bus. ICSP powering is
  therefore done through the bus pins (the jig supplies 12 V) or through a VCC
  pad behind an open solder jumper (section 7).

**Reverse polarity (decided):** a P-channel MOSFET in the unit's own 12 V branch
(drain to the bus junction, source to the buck and the ULN2003A COM, gate to GND
through 10 k), ~0.2 V drop at 0.2 A. It is **not** in the through path between J4 and J5: that
trace carries the whole downstream row (~3 A) and has no FET, so one
unit's FET only carries that unit's own ~0.2 A. A reversed cable leaves every
unit unpowered and blocked. The 5-pin connectors are keyed; the FET covers
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
| GM10-10BY-class planetary | DRV8833 (5 V rail) or TMC2209 (12 V) | As above for DRV8833. TMC2209 is STEP/DIR/EN: the Stepper library is replaced by pulse generation, microstep configuration, a new `STEPS`. |

"Firmware unchanged" is conditional. `STEPS 2038` is `static_assert`ed equal to
`SFP_OFFSET_LIMIT_STEPS` in `firmware/v2/shared/SplitFlapProtocol.h`, so any
other step count (1/16 gearbox, planetary, microstepping) touches `shared/`, the
masters' offset clamp and the unit bundle; `AMOUNTFLAPS` for 64 flaps is the
alphabet project's change either way. The home policy, stall policy and
supply-hold timings are tuned on the 28BYJ-48 and must be bench-checked after a
motor change. Footprints: the board carries the ULN2003A and, depopulated, an
alternate DRV8833 footprint; a TMC2209 (QFN-28, two sense resistors) is not
worth carrying unless #671 selects a bipolar motor that needs it.

Idle: the firmware pulls all four stepper pins low after a move (coil release),
so the idle motor current is zero; keep that.

## 5. Hall sensor

An **A3144-class** SMD or TO-92 open-collector hall switch (the part the KY-003
module the fleet uses carries), 10 k pull-up on the board (the firmware reads the
pin as plain `INPUT`). Same sensor family as the fleet, so the marker window the
firmware sees and the homing and idle hall-consistency checks are unchanged;
bench-verify with the real magnet anyway. A3144 is unipolar (one pole face
operates it) and needs 4.5 V or more: the 5 V rail has the margin, and the magnet
in its drum pocket (#673) must face the sensor with the operating pole.
#673 settled the placement: the magnet (3.0 x 1.0 mm, assumed, copy the current unit's if it
differs) sits at radius 15.5 mm on the outer face of one drum disc and the
sensor faces the disc from the side wall through a 1.2 mm wall, so **a remote hall
on a tail (>= 50 mm) is required**; a hall on the board cannot reach it. The
board carries three tail pads (+5 V, GND, OUT) and the 10 k pull-up; the hall
sits on a small tail board.

## 6. I2C connectors and level shifting

The ATmega328P at 16 MHz needs ~5 V (running it at 3.3 V forces 8 MHz and breaks
the 16 MHz firmware and twiboot). **The unit bus stays 5 V.** Proposal:
- the unit has **no pull-ups and no level shifter**; the controller (#675) owns
  the single 3.3 V/5 V shifter and the 5 V pull-ups for each row bus (this removes
  the v1 BSS138 pair and four 10 k resistors from every unit);
- the bus runs at 100 kHz (twiboot reflash is only safe at 100 kHz);
- one **five-conductor** cable per unit, +12 V, GND, SDA, SCL, CHAIN, daisy-chained in and
  out (two connector footprints, or one connector plus wire pads). Pins 1..4 are wired
  straight through J4 to J5 in parallel; pin 5 is not: J4 pin 5 is CHAIN_IN to the
  MCU, J5 pin 5 is CHAIN_OUT from the MCU (section 9);
- the connector sits on the board's back edge, away from the mechanism (#673:
  a body up to ~30 mm tall fits, so JST XH-class or smaller is possible again;
  family left `TBD-verify`). Either short edge works for #673; I pick the edge
  during layout and tell #673.

Interface with #675: 5 V unit bus, shifter and pull-ups on the controller, five-conductor cable (the CHAIN head is driven by the controller, one per row). Interface with #673: board behind the mechanism.

## 7. ICSP pads

Six pads (1.27 mm or a Tag-Connect footprint), MISO/MOSI/SCK/RESET/GND plus VCC
behind an open solder jumper. One burn per board, the same image for every board (a blank chip has no bootloader
to update over I2C): twiboot, the unit sketch, the fuses and a blank-address EEPROM
image in one session. After it the address, the sketch and the bootloader all update over I2C. Notes:
- PB3 (MOSI) is the first stepper pin and PB5 (SCK) the LED, as on the Nano.
  Use a >= 1 k LED resistor; the ULN2003A input sees MOSI toggle during the burn,
  which is harmless with the bus 12 V absent. Power the jig through the bus pins
  or the VCC jumper, not both (section 3, backfeed).
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
|---|---|---|
| 24BYJ48 12 V, two coils energised (120 ohm per phase from the listing) | 12 V | ~0.2 A, ~2.4 W while moving, 0 idle |
| ATmega328P at 16 MHz, 5 V | 5 V | ~15 mA |
| Hall + pull-up + LED | 5 V | ~5-10 mA |
| Buck input, logic only (85 % efficiency) | 12 V | ~15 mA |
| Row of 15, all moving | 12 V | ~3 A, ~36 W |

The ~3 A row worst case is accepted (decided). The render stagger limits how many units
move at once and **homing is staggered the same way** (the unit's self-home deadline
uses the address index, section 9): homing all 15 at once is
the worst case (the 16-unit homing inrush is the known brownout source on the S3
leader). Size the per-row 12 V feed and the bus connectors for 3 A. For a 5 V-motor candidate the buck carries
that motor's current as well (datasheet value not available: the 15BY25-560A
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
Neither pin is on ICSP (PB3..PB5, RESET), I2C (PC4/PC5), the stepper (PB0..PB3), the
hall (PD7) or the debug pads (PD0/PD1); PD2 (INT0) stays spare.

- Active high, 5 V. CHAIN_IN has a 1 k series resistor and a 100 k pull-down: an
  unplugged cable, an unpowered neighbour or a unit in reset reads **deasserted**
  (the chain fails closed). CHAIN_OUT is a plain push-pull output; the MCU in reset or
  blank leaves it Hi-Z, which the next unit's pull-down reads as deasserted.
- Cable: five conductors, +12 V, GND, SDA, SCL, CHAIN. J4 pin 5 is CHAIN_IN, J5 pin 5
  is CHAIN_OUT, pins 1..4 straight through (section 6). The CHAIN level is slow DC;
  no more than 2 firmware samples of debounce.
- The first unit of a row has its CHAIN_IN on the controller's CHAIN head for that
  row (one GPIO per row through a 5 V buffer, #675). The head is held high while the
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
| **Removed** unit | Its address stops answering, the chain breaks there, units behind it keep working at their addresses. Reported as absent (as an absent unit today); no renumbering. After the cable is closed up the operator runs a full enumeration. |
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

1. **Board variant** (`unit_note` env, `-DSF_NOTE_BOARD`): pin map (driver, hall, no
   DIP, CHAIN_IN/OUT on PD3/PD4), address from the pair, the gate, `CHAIN_CLEAR`,
   boot-home stagger from the address. `Unit/` and `shared/` change, so the **unit
   bundle must be restaged** (CLAUDE.md step 4).
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

## Decided by Lucas (2026-11-20)

- Bare ATmega328P at 16 MHz, Unit firmware, I2C slave and twiboot kept.
- No DIP switch: the address lives only in EEPROM, assigned in physical order over
  a chain line (CHAIN_IN/CHAIN_OUT, five-conductor cable); the gate lives in the
  sketch and in twiboot; a swapped unit re-enumerates.
- One ICSP burn per board with one image for all boards; day-0 fuses 2 KB boot
  section and EESAVE programmed; the note board has its own twiboot image and
  in-system update variant.
- No ESP-01S on the unit board; the controller (#675) drives the units and the heads.
- 12 V bus with a per-unit buck; MOSFET reverse-polarity protection per unit.
- A3144-class hall; ~3 A row worst case, homing staggered like rendering.
- Fit the ~41.5 x 73 x ~95 mm envelope; outline from #673.

## My calls (correctable)

- TQFP-32 for the first batch; crystal over resonator; buck over LDO.
- The unit bus stays 5 V; pull-ups and the level shifter live on the controller.
- CHAIN_IN = PD3, CHAIN_OUT = PD4 (section 9); ripple rule (CHAIN_OUT = CHAIN_IN when
  addressed) so only one blank unit listens on 0x29; single-gap swap assigned
  automatically, everything else an operator action.
- Address pair `addr`/`~addr` read by both twiboot and the app; default 0x29 gated.
- HFUSE 0xD2; twiboot linked at 0x7800 with `do_spm` born in the last page.
- ICSP VCC behind an open jumper; the jig powers through the bus.

## Left to the builder

- Schematic and layout in EasyEDA, LCSC part numbers for every `TBD-verify` line
  (no number in the BOM was invented), pick-and-place, a reviewer pass.
- Measuring the note twiboot image and the EEPROM offsets (36/37 are by the
  current constants).
- The buck and its inductor, the MOSFET, the hall, connectors: datasheet-check every
  figure marked estimate.

## Open questions for Lucas

1. **Motor:** is the 24BYJ48 12 V the final choice? That keeps the unchanged
   firmware and a motor straight from 12 V; a 5 V motor makes the buck carry it.
   Does a 1/16 batch need rejecting on arrival (`STEPS` is a protocol constant)?
2. **Motor side:** the motor leads enter on the edge nearest the motor-side wall and
   #673 wants left or right; which side, and which short edge for the bus connector?
