# Note-size unit board: a compact bare-ATmega328P board — design

Issue: #674 · Epic: #676 · Status: draft for review (nothing decided beyond "Decided by Lucas")

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

- One board per unit: ATmega328P, motor driver, hall, 12 V to 5 V buck, I2C in
  and out, ICSP pads. Nothing else.
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
  address, or that needs a physical trip to recover a unit.
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
  programmed), `EFUSE=0xFD` (BOD 2.7 V).
- **Pins the firmware uses today:** stepper D8..D11 (PB0..PB3), hall D7 (PD7,
  plain `INPUT`, so the pull-up must be on the board), DIP D3..D6 (PD3..PD6,
  to be freed), LED D13 (PB5, `LED_BUILTIN`), I2C A4/A5, A0 floating for the
  random seed. Supply is measured against the internal bandgap, not a pin: no
  divider is needed.
- Public repo: no addresses, hostnames or device ids in any file.

## Block diagram

```
 12 V bus in --+--[TVS/rev-prot]--+--------------------------+----> 12 V bus out
 GND ----------+                  |                          |
 SDA/SCL (5 V) ------------------ | -----------+-------------+----> SDA/SCL out
                                  |            |
                           [buck 12 V -> 5 V]  |
                                  | 5 V        |
                +-----------------+------------+--------------+
                |                 |            |              |
           [ATmega328P] --I2C--(slave)         |          [hall] -- OUT --> PD7
            16 MHz, twiboot       |            |
            ICSP pads, LED(PB5)   +--4 GPIO--> [driver] ---> motor
                                                ^ 12 V (ULN2003A / TMC2209)
                                                  or 5 V (DRV8833)
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
- a TVS on the 12 V input; reverse-polarity protection is an open question;
- the v1 22 uF 10 V capacitor (C503893) is only valid on the 5 V node. A
  12 V input node needs a >= 25 V capacitor.
- Backfeed: with the 12 V bus absent and ICSP powered at 5 V, a synchronous buck
  can feed 5 V back up its high-side body diode onto the bus. ICSP powering is
  therefore done through the bus pins (the jig supplies 12 V) or through a VCC
  pad behind an open solder jumper (section 7).

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

A SMD open-drain, active-low, omnipolar hall (DRV5032 family or an AH3144-class
part) with a 10 k pull-up on the board (the firmware reads the pin as plain
`INPUT`; the KY-003 module the fleet uses has its own pull-up). #673 settled
the placement: the magnet (3.0 x 1.0 mm, assumed, copy the current unit's if it
differs) sits at radius 15.5 mm on the outer face of one drum disc and the
sensor faces the disc from the side wall through a 1.2 mm wall, so **a remote hall
on a tail (>= 50 mm) is required**; a hall on the board cannot reach it. The
board carries three tail pads (+5 V, GND, OUT) and the 10 k pull-up; the hall
sits on a small tail board (SOT-23 / DRV5032 pad set). A DRV5032-class part is far more sensitive than
the A3144 on the KY-003, so the marker window the firmware sees widens: bench
re-verify homing and the idle hall consistency check with the real magnet.

## 6. I2C connectors and level shifting

The ATmega328P at 16 MHz needs ~5 V (running it at 3.3 V forces 8 MHz and breaks
the 16 MHz firmware and twiboot). **The unit bus stays 5 V.** Proposal:
- the unit has **no pull-ups and no level shifter**; the controller (#675) owns
  the single 3.3 V/5 V shifter and the 5 V pull-ups for each row bus (this removes
  the v1 BSS138 pair and four 10 k resistors from every unit);
- the bus runs at 100 kHz (twiboot reflash is only safe at 100 kHz);
- one four-wire cable per unit, +12 V, GND, SDA, SCL, daisy-chained in and out
  (two connector footprints wired in parallel, or one connector plus wire pads);
- the connector sits on the board's back edge, away from the mechanism (#673:
  a body up to ~30 mm tall fits, so JST XH-class or smaller is possible again;
  family left `TBD-verify`). Either short edge works for #673; I pick the edge
  during layout and tell #673.

Interface confirmed with #675 (5 V unit bus, shifter and pull-ups on the controller, four-conductor cable, default 0x29) and #673 (board behind the mechanism).

## 7. ICSP pads

Six pads (1.27 mm or a Tag-Connect footprint), MISO/MOSI/SCK/RESET/GND plus VCC
behind an open solder jumper. The first burn writes twiboot, the unit sketch,
the fuses and the EEPROM image in one session. Notes:
- PB3 (MOSI) is the first stepper pin and PB5 (SCK) the LED, as on the Nano.
  Use a >= 1 k LED resistor; the ULN2003A input sees MOSI toggle during the burn,
  which is harmless with the bus 12 V absent. Power the jig through the bus pins
  or the VCC jumper, not both (section 3, backfeed).
- `HFUSE=0xDC` leaves EESAVE unprogrammed: a chip erase (every `avrdude` flash
  rewrite) also erases the EEPROM, including the address. The burn script
  rewrites the address every time.
- Two test points on PD0/PD1 for `SERIAL_ENABLE` debug builds.

## 8. Current budget (estimates, verify on the bench)

| Load | Rail | Estimate |
|---|---|---|
| 24BYJ48 12 V, two coils energised (120 ohm per phase from the listing) | 12 V | ~0.2 A, ~2.4 W while moving, 0 idle |
| ATmega328P at 16 MHz, 5 V | 5 V | ~15 mA |
| Hall + pull-up + LED | 5 V | ~5-10 mA |
| Buck input, logic only (85 % efficiency) | 12 V | ~15 mA |
| Row of 15, all moving | 12 V | ~3 A, ~36 W |

The render stagger limits how many units move at once; homing all 15 at once is
the worst case (the 16-unit homing inrush is the known brownout source on the S3
leader). Size the per-row 12 V feed and the bus connectors for that worst case or
confirm the stagger covers homing. For a 5 V-motor candidate the buck carries
that motor's current as well (datasheet value not available: the 15BY25-560A
listing has no coil data).

## 9. EEPROM-only addressing

### What exists today (verified)

- `getaddress()` (`UnitI2CProtocol.ino`): resolves the identity block
  (EEPROM bytes 0..3: version, address, flag, checksum, via
  `unitEeIdentityAddress`); anything invalid falls back to `SFP_I2C_ADDRESS_BASE
  + DIP`. With PD3..PD6 unconnected the pull-ups read DIP 0, so a DIP-less
  board's de facto default is **0x01**, the address of unit 0.
- twiboot (`UnitBootloader/main.c`) never reads EEPROM for its address; it
  reads PD3..PD6. With `I2C_ADDRESS_BASE == 0` it falls back to stock
  `TWI_ADDRESS` 0x29.
- The lean image the sketch carries for in-system bootloader update (#499,
  `prebuilt/twiboot-new-progmem.h`, built `-DI2C_ADDRESS_BASE=1`) also reads the
  DIP. It is 894 B of the 896 B of pages 0..6; page 7 is frozen (`do_spm`). It
  has no EEPROM access (`SF_NEW_TWIBOOT`).
- Day-0 init `eeInitIfBlank()` keys on EEPROM byte 0 (the layout version, also
  the identity block's first byte) and **clears the identity block**; a layout
  bump therefore wipes every address on a DIP-less row.
- `SET_I2C_ADDRESS` (0x94, address + complement) exists end to end: unit,
  `unitBusSetAddress`, the display task's `SetAddress` op and the wall op
  `OPC_SET_ADDRESS`; the master guards it behind a sketch-running unit at the
  address (range 1..126). It is unverifiable after the fact and its documented
  recovery is a physical re-DIP, which the new board cannot do.
- The master scans only `toI2cAddress(0..maxUnits-1)`, so a unit at an address
  outside that range is invisible, in the sketch or in twiboot.

### How twiboot gets the address

Three variants:

| | A. Read the identity block (as the app does) | B. A dedicated address pair, `addr` + `~addr` | C. A bootloader-private write by the app |
|---|---|---|---|
| Bootloader checks | version, flag, XOR checksum with mask, range | one compare, range | n/a |
| Couples to | `UNIT_EE_LAYOUT_VERSION`, identity layout | nothing but two offsets | n/a |
| Survives a layout bump | no (init clears it) | yes (init never touches it) | yes |
| Disagreement with the app on a torn write | possible if the checks differ | none, both apply the same two-byte rule | possible |
| Size (unmeasured) | the largest | the smallest | n/a |

**Recommendation: B** on the note board only: two EEPROM bytes in the reserved
area (36 and 37 by the current constants, after the bus record at 28..35, ring
at 64; the builder confirms), written by the app on `SET_I2C_ADDRESS` and by
the factory burn. The app and twiboot both resolve it by the same two-byte
rule (valid when `b == ~a` and the address is in range); blank or torn resolves
to the default address in **both** and they cannot disagree. The existing
identity block stays for the v1 builds.

Size: the note twiboot image drops the DIP block (settle loop, four pin reads,
~40-50 B by estimate) and adds one EEPROM byte read twice (~30-40 B by
estimate). **Unmeasured:** the lean image has 2 B of headroom, so build it
before committing to this; `BOOTSZ` is a fuse (ICSP only) and page 7 must stay
byte-identical, so the fallback is a leaner address read, not a bigger boot
section. No fielded unit takes this image: it is a board variant of twiboot
(`SF_NOTE_BOARD`), with its own image and CRC.

The unit sketch for the note board must carry **that** target image, not the DIP
one, or an in-system bootloader update would install a DIP-reading twiboot on
a DIP-less board (every unit on 0x01). Either the note env embeds its own
`twiboot-new-progmem.h` variant (new CRC in `BootSectionClassify`), or the
update gate is disabled for that env until it does. The masters' reflash path is
unchanged: it already enters twiboot at the unit's own address.

### How a blank board gets its first address

**Option 1: default address plus one-at-a-time.** A blank unit (sketch or
twiboot) listens at a default; the master assigns it with `SET_I2C_ADDRESS`.
**Option 2: address at the ICSP burn.** The burn script writes the address (and
the rest of the EEPROM image) with the bootloader and sketch.

| Scenario | Option 1 | Option 2 |
|---|---|---|
| Fresh batch of N blank boards on one bus | All N answer the default and the first assign hits all of them: boards must be connected one at a time, a manual process with 45 units | Each board is born with its address, nothing to do on the bus |
| Replacement unit in a populated row | Connect alone, assign, reconnect. If left on the bus blank it listens on the default, harmless | Burn the old unit's address on the bench first (label the boards) |
| Torn or corrupt identity in the field | Falls to the default, visible only if the master probes it | Same fallback; the unit is recoverable by one-at-a-time assignment |
| Layout bump | Only safe if init keeps a valid address (change below) | Same |
| Reflash of one unit | Unchanged: enter twiboot at the unit address; twiboot listens on the EEPROM address | Same |
| In-system bootloader update | Needs the note twiboot target image (above) | Same |

Selector for one-at-a-time: there is no chain-enable line on the unit board
(a connector decision, taken here); the selector is **physical** (plug units in
one at a time, or a jig supplying 12 V per board). Not adding a chain pin keeps
the four-wire bus and the connector small. The options are not either/or:
every board goes through an ICSP session anyway.

**Recommendation: both, with Option 2 as the normal path.** The factory ICSP
session writes bootloader + sketch + fuses + the full EEPROM image including the
address; the default address (0x29, stock `TWI_ADDRESS`, outside the 1..16 unit
range and not scanned today) exists for blank, torn or replaced boards, with
one-at-a-time assignment as the recovery. Option 1 alone would make a 45-unit
build a one-by-one chore and would leave a bus of identical blank boards the
first time anything goes wrong; Option 2 alone has no field recovery for a
corrupt address.

### Firmware and master changes this implies (not implemented here)

1. **Board variant** (`unit_note` env, `-DSF_NOTE_BOARD`): pin map (driver, hall,
   no DIP), address read from the dedicated pair, default `0x29`. `Unit/` and
   `shared/` change, so the **unit bundle must be restaged** (CLAUDE.md step 4).
2. **twiboot note image:** reads the pair, default 0x29, no DIP block; new CRC;
   `make_new_twiboot.py`, `prebuilt/`, the simavr proof and the
   `BootSectionClassify` known-CRC list follow. Size must be measured first.
3. **Day-0 init** must not clear a valid address, and the factory EEPROM image
   must not look blank to it (byte 0 doubles as identity version): the factory
   image is generated by a host tool built from `UnitEeprom.h` (not a
   hand-rolled second checksum implementation).
4. **`SET_I2C_ADDRESS` safety:** reject the default 0x29 as a target and require
   a verified round trip (the unit answers at the new address) before the master
   reports success; today it is unverifiable.
5. **Master:** a provisioning probe of the default address (list the blank unit,
   assign the next free address), and the `addressStored` flag
   (`UNIT_FLAG_ADDR_EEPROM`, bit 4) is always true on the note board; the web page
   copy that says "falls back to DIP" needs a note build wording.
6. **Burn script** (`flashing/`): per board, takes the address, writes flash,
   EEPROM and fuses in the correct order (chip erase first, EEPROM after).

## Decided by Lucas (2026-11-20)

- Bare ATmega328P at 16 MHz, Unit firmware, I2C slave and twiboot kept.
- No DIP switch: the address lives only in EEPROM.
- No ESP-01S on the unit board; the controller board (#675) drives the units.
- 12 V bus with a per-unit buck.
- Fit the ~41.5 x 73 x ~95 mm envelope; outline from #673.

## My calls (correctable)

- TQFP-32 for the first batch; crystal over resonator; buck over LDO.
- The unit bus stays 5 V; pull-ups and the level shifter live on the controller.
- Address pair `addr`/`~addr` read by both twiboot and the app; default 0x29;
  factory ICSP burn is the normal path, one-at-a-time assignment the recovery.
- ICSP VCC behind an open jumper; the jig powers through the bus.

## Left to the builder

- Schematic and layout in EasyEDA, LCSC part numbers for every `TBD-verify` line
  (no number in the BOM was invented), pick-and-place, a reviewer pass.
- Measuring the note twiboot image and the EEPROM offsets (36/37 are by the
  current constants).
- The buck and its inductor, the hall part, connectors: datasheet-check every
  figure marked estimate.

## Open questions for Lucas

1. **Motor:** is the 24BYJ48 12 V the final choice? That keeps the unchanged
   firmware and a motor straight from 12 V; a 5 V motor makes the buck carry it.
   Does a 1/16 batch need rejecting on arrival (`STEPS` is a protocol constant)?
2. **Provisioning:** do you accept the factory-burn address (label each board
   by position) as the normal path, with 0x29 plus one-at-a-time as recovery
   only?
3. **Bootloader size:** if the note twiboot cannot fit the EEPROM read in the
   lean image, do you accept a fuse change to a 2 KB boot section for the new
   board only (it breaks the in-system update layout and costs 1 KB of app
   flash), or drop the in-system bootloader update on the note board?
4. **Reverse-polarity protection:** protect each unit (a MOSFET, ~0.2 V drop and
   one more part) or rely on keyed wiring?
5. **Motor side:** the motor leads enter on the edge nearest the motor-side wall and #673 wants left or right; which side, and which short edge for the bus connector?
6. **Hall:** a more sensitive DRV5032-class part (new magnet behaviour to
   bench-verify) or an A3144-class part (same as the fleet)?
7. **Per-row 12 V current:** is a ~3 A worst case at the controller acceptable,
   or must the stagger also cover homing?
