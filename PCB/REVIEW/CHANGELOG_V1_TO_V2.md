# Changelog: v1 -> v2

Hardware-only summary of what changes from the deployed v1 hardware
(`PCB/v1/`) to v2 (`PCB/v2/`). Firmware is being rewritten and is not a
factor in any of these decisions.

Tracked under issue #83.

## System architecture

| Concern | v1 | v2 |
|---|---|---|
| Display capacity | 1 case, 16 units | 4 rows, 64 units |
| Master | 1 per system | 1 per system, 4 RS-485 row ports |
| Distribution per row | n/a | 2× 300 mm DIN-rail bus PCBs daisy-chained |
| Power input | 5 V barrel + 12 V brick (dual rail) | single 12 V / 15 A brick into master |
| Bus | I2C 400 kHz, star, 5 V, single case | 4x RS-485 half-duplex, one bus per row |
| Distribution | Daisy-chained JST-XH between adjacent units | DIN rail with bus PCB; units clip on, contact via pogo pins |
| Row fault isolation | none | per-row polyfuse on master (4 A hold) |
| Master-to-row cable | n/a (single case) | single 4-pin combined cable carrying 12V + GND + A + B |
| Cable between bus PCBs | n/a | short 4-pin daisy-chain cable per row |

## Master PCB

| Concern | v1 | v2 |
|---|---|---|
| MCU | ESP-01 (ESP8266) | ESP32-S3-WROOM-1-N16R8 |
| 4th UART for 4 row buses | n/a | SC16IS740 single-channel UART-to-SPI expander |
| Programming | OTA only | USB-C native CDC + OTA |
| Bus output | 4 wires loose (SDA/SCL/5V/GND) | 4× 4-pin shrouded headers (12V/GND/A/B per row) |
| Reverse-polarity protection | none | P-FET (AOD409 in DPAK) sized for 15 A |
| Input TVS | none | SMAJ15A |
| Bus drive | passive I2C pull-ups | 4× SN65HVD75 transceivers, 120 R termination, 1k/1k bias per bus |
| Power role | acts as power hub for case | sources 12 V to all 4 rows from a single 15 A brick (per-row polyfuse) |

## Unit PCB

| Concern | v1 | v2 |
|---|---|---|
| MCU | ATmega328P (Arduino Nano) | STM32G030K6T6 (required for 96-bit silicon UID) |
| Motor driver | ULN2003A | TPL7407L (default) or ULN2003A — pin-compatible footprint |
| Motor | 28BYJ-48 5 V | 28BYJ-48 12 V |
| Power input | 5 V + 12 V dual rail | 12 V single rail |
| Onboard regulator | 5 V LDO (7805) | 12 V → 3.3 V LDO (HT7833) |
| Reverse-polarity protection | none | P-FET (AO3401) |
| Input TVS | none | SMAJ15A |
| Hall sensor | KY-003 module (discrete) | A1101ELHL on-board (SOT-23W) |
| RS-485 transceiver | n/a (was I2C) | SN65HVD75 |
| RS-485 ESD | n/a | SM712-02HTG across A/B |
| Address source | 4-bit DIP switch (manual config per unit) | 96-bit silicon UID + IDENTIFY push-button |
| Connector to bus | JST-XH 3-pin chained between units | **4 pogo pins on underside** (no connector to bus) |
| Mounting | screws to v1 case | DIN rail clip (asymmetric for pogo polarisation) |

## Bus PCB (new in v2)

v1 had per-unit JST-XH cables daisy-chained between adjacent units.

v2 introduces a passive **DIN-rail bus PCB**:

- Two **300 mm sections per row**, daisy-chained — same PCB design,
  fits within JLC's 400 mm standard fab limit.
- 4 traces running the length: 12V (5 mm wide, outside top edge), A
  (1 mm, middle-upper), B (1 mm, middle-lower), GND (5 mm wide, outside
  bottom edge).
- ENIG plating on contact strips for pogo pin reliability.
- 4-pin shrouded box header at each end (same pinout): mate with master
  cable on one side and daisy-chain cable on the other.
- Mounts inside or alongside a 35 mm DIN rail.

8 bus PCBs per 4-row system, all from one PCB design.

## Cabling (new in v2)

- Master cable per row: 4-conductor (12V/GND/A/B), 4-pin shrouded on
  both ends.
- Daisy-chain cable per row: same 4-conductor, short (~5-15 cm).
- Terminator plug per row: 4-pin shrouded shell with 120 Ω across A/B,
  plugs into the unused connector on the last bus PCB.

## Addressing (new mechanism in v2)

- Each STM32G030K6T6 carries a unique 96-bit silicon UID.
- Each unit has an IDENTIFY push-button + LED on the board.
- Master holds a UID → (row, column) map in flash.
- Commissioning: web UI walks user through assigning each (row, column)
  by pressing IDENTIFY on the corresponding physical unit.
- Re-commissioning is a single button press per replaced unit.

No DIP switches. No SLOT_ID wiring. No solder jumpers.

## What v1 got right (kept conceptually)

- 28BYJ-48 stepper.
- Hall-sensor homing concept.
- Per-unit board so a failed unit can be replaced without case
  disassembly.
- Mechanical envelope (~75 × 35 mm unit board).

## What v1 got wrong (fixed)

- Dual 5 V + 12 V power rails. v2 is single 12 V.
- DIP-switch addressing. v2 uses silicon UID + IDENTIFY button.
- ~30 internal patch cables per case. v2 has clip-on units with
  pogo-pin contact to a passive bus PCB.
- Discrete hall-sensor module on flying leads. v2 has the hall sensor
  on the unit PCB.
- No reverse-polarity protection. v2 has P-FET on every board.
- No bus ESD on units. v2 has SM712 on RS-485 A/B at every unit.
- Single case capacity. v2 supports 4 rows × 16 = 64 units.

## Approximate cost per 4-row (64-unit) system

- Master PCB + parts: ~EUR 18
- 64× unit PCB + parts (incl. 4 pogo pins each, ~EUR 1.20): ~EUR 4.80/unit → ~EUR 307
- 8× bus PCB at JLC (300 mm × 30 mm 2-layer ENIG): ~EUR 80 (~EUR 10 each at qty 8)
- 1× 12 V / 15 A brick: ~EUR 25
- 4× DIN rail (35 mm TS35, ~600 mm each): ~EUR 15
- Cables (4× master, 4× daisy-chain) + 4× terminator plug: ~EUR 20
- 64× DIN rail clips (off-shelf or 3D-printed): ~EUR 20 (or free if 3D-printed)
- **Total: ~EUR 485** for a complete 4-row 64-unit system.

The DIN rail + bus PCB approach adds ~EUR 120 over the harness approach
in exchange for tool-free unit installation, no per-unit cabling, and
clean industrial mounting.

## Documents

| Document | Purpose |
|---|---|
| `PCB/v2/README.md` | Top-level v2 overview |
| `PCB/v2/ARCHITECTURE.md` | System block diagram, voltage and bus flow |
| `PCB/v2/MASTER.md` + `MASTER_BOM.csv` | Master PCB |
| `PCB/v2/UNIT.md` + `UNIT_BOM.csv` | Unit PCB (with pogo pins) |
| `PCB/v2/BUS_PCB.md` + `BUS_PCB_BOM.csv` | DIN-rail bus PCB |
| `PCB/v2/OPEN_DECISIONS.md` | Pending hardware decisions |
| `PCB/v1/` | v1 reference (deployed hardware) |
