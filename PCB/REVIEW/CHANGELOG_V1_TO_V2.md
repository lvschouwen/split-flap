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
| Distribution PCB per row | n/a | none — daisy-chain harness |
| Power input | 5 V barrel + 12 V brick (dual rail) | own 12 V brick per row + small 12 V brick for master |
| Bus | I2C 400 kHz, star, 5 V, single case | 4x RS-485 half-duplex, one bus per row |
| Distribution | Daisy-chained JST-XH between adjacent units | Per-row daisy-chain harness with trunk + drops |
| Per-unit fault isolation | none | per-unit P-FET + TVS + on-unit fuse considerations (no per-slot polyfuse, no backplane to host one) |
| Inter-row cable | n/a (single case) | 3-conductor (A, B, GND) from master to each row's harness master end |

## Master PCB

| Concern | v1 | v2 |
|---|---|---|
| MCU | ESP-01 (ESP8266) | ESP32-S3-WROOM-1-N16R8 |
| 4th UART for 4 row buses | n/a | SC16IS740 single-channel UART-to-SPI expander |
| Programming | OTA only (web `/firmware/master`) | USB-C native CDC + OTA |
| Bus output | 4 wires loose (SDA/SCL/5V/GND) | 4x 3-pin shrouded headers (A/B/GND per bus) |
| Reverse-polarity protection | none | P-FET (AO3401) |
| Input TVS | none | SMAJ15A |
| Bus drive | passive I2C pull-ups | 4x SN65HVD75 transceivers, 120R termination, 1k/1k bias per bus |
| Power role | acts as power hub for case | signal-only; does not source row power |
| Stack-up | 2-layer | 2-layer |

## Unit PCB

| Concern | v1 | v2 |
|---|---|---|
| MCU | ATmega328P (Arduino Nano) | STM32G030K6T6 (required for 96-bit silicon UID) |
| Motor driver | ULN2003A | TPL7407L (default) or ULN2003A — pin-compatible footprint |
| Motor | 28BYJ-48 5 V | 28BYJ-48 12 V |
| Power input | 5 V + 12 V dual rail | 12 V single rail |
| Onboard regulator | 5 V LDO (7805) | 12 V -> 3.3 V LDO (HT7833) |
| Reverse-polarity protection | none | P-FET (AO3401) |
| Input TVS | none | SMAJ15A |
| Hall sensor | KY-003 module (discrete) | A1101ELHL on-board (SOT-23W) |
| RS-485 transceiver | n/a (was I2C) | SN65HVD75 |
| RS-485 ESD | n/a | SM712-02HTG across A/B |
| Address source | 4-bit DIP switch (manual config per unit) | 96-bit silicon UID + IDENTIFY push-button (commissioning fallback) |
| Connector to bus | JST-XH 3-pin chained between units | 4-pin shrouded box header to harness drop |

## Per-row harness (new in v2)

v1 had per-unit JST-XH cables daisy-chained between adjacent units, ~30
cables per case.

v2 uses one daisy-chain harness per row:

- 4-conductor trunk (12V, GND, A, B) running the length of the row.
- 16 drops (one 4-pin connector per unit position).
- Master end connects to row 12 V brick (12V/GND) + master row port (A/B
  via 3-conductor cable).
- Far end terminated with a 120 ohm "terminator plug" across A/B.
- 22 AWG conductors for rows up to ~2 m.

Four harnesses per 4-row system. No PCB component — pure cable assembly.

## Addressing (new mechanism in v2)

- Each STM32G030K6T6 carries a unique 96-bit silicon UID (`UID_BASE`
  register).
- Each unit has an IDENTIFY push-button + LED on the board.
- Master holds a UID -> (row, column) map in flash.
- Commissioning: web UI walks user through assigning each (row, column)
  by pressing IDENTIFY on the corresponding physical unit.
- Re-commissioning is a single button press per replaced unit.

No DIP switches. No SLOT_ID wiring. No solder jumpers. No backplane.

## What v1 got right (kept conceptually)

- 28BYJ-48 stepper. Mechanically identical between voltage variants.
- Hall-sensor homing concept.
- Per-unit board so a failed unit can be replaced without case
  disassembly.
- Mechanical envelope (~75 x 35 mm unit board, 4 mounting holes).

## What v1 got wrong (fixed)

- Dual 5 V + 12 V power rails. v2 is single 12 V per row.
- DIP-switch addressing required physical reconfiguration before
  insertion. v2 uses silicon UID + IDENTIFY button — replace a unit, press
  IDENTIFY when prompted, done.
- ~30 internal patch cables per case. v2 has one harness per row with
  uniform drops.
- Discrete hall-sensor module on flying leads. v2 has the hall sensor
  on the unit PCB.
- No reverse-polarity protection. v2 has P-FET on every board.
- No bus ESD on units. v2 has SM712 on RS-485 A/B at every unit.
- Single case capacity. v2 supports 4 rows × 16 = 64 units.

## Approximate cost per 4-row (64-unit) system

- Master PCB + parts: ~EUR 14 (4 transceivers, SC16IS740, 4 row ports)
- 64x unit PCB + parts: ~EUR 4.50/unit -> ~EUR 288
- 4x 12 V / 5 A row brick: ~EUR 60
- 1x 12 V / 1 A master brick: ~EUR 8
- 4x row harness (cable + connectors): ~EUR 8/row -> ~EUR 32
- **Total: ~EUR 402** for a complete 4-row 64-unit system.

## Documents

| Document | Purpose |
|---|---|
| `PCB/v2/README.md` | Top-level v2 overview |
| `PCB/v2/ARCHITECTURE.md` | System block diagram, voltage and bus flow |
| `PCB/v2/MASTER.md` + `MASTER_BOM.csv` | Master PCB |
| `PCB/v2/UNIT.md` + `UNIT_BOM.csv` | Unit PCB |
| `PCB/v2/HARNESS.md` | Per-row harness spec (cable + connectors) |
| `PCB/v2/OPEN_DECISIONS.md` | Pending hardware decisions |
| `PCB/v1/` | v1 reference (deployed hardware) |
