# PCB v2

**Revision:** 2026-04-26

Hobby-scale redesign of the split-flap display electronics.

Tracked under issue #83.

## Status

- **Phase:** FROZEN (architecture, MPNs, footprints, pinouts locked
  by issue #86 / commit 060633f). Layout in KiCad 10 is the only
  remaining work.
- **2026-07-04 review pass (#99–#105):** 2-bus architecture (rows
  paired on native UART1/UART2, SC16IS740 deleted), unit MCU →
  STM32G030K8T6, LM2937 LDO + SMAJ13A TVS, hard-gold bus contacts,
  mandatory power-off insertion. **Layout is now additionally BLOCKED
  on unit/rail geometry — issue #100.** Motor current re-measurement
  in #101; master Q1 topology in #103.
- **Branch:** `pcb-v2-rs485-48v` (branch name is historical — 48 V
  was evaluated and rejected; the design is 12 V)
- **Locked decisions:** see `KICAD_HANDOFF.md` § "Locked decisions"
  and `OPEN_DECISIONS.md`.

## Scope

- 1 master PCB (ESP32-S3, USB-C, 4 row power ports, 2 RS-485 buses —
  rows paired: Bus A = rows 0+1 on UART1, Bus B = rows 2+3 on UART2).
- 4 rows × 16 unit slots = **64 units max.**
- **DIN-rail bus PCB**: 2× 300 mm bus PCBs daisy-chained per row, units
  clip onto rail and contact bus traces via 4 pogo pins per unit. No
  cabling between unit and bus.
- **One 12 V / 15 A brick.** Master sources both power and signal to
  each row over a single 4-pin combined cable.
- **Addressing: STM32 96-bit UID** + per-unit IDENTIFY button. No DIP
  switches, no slot wiring on the bus.

Explicitly **not** in scope: rigid backplanes, DIP switches, 48 V
distribution, RJ45 power-and-data, MAX14830 SPI-UART bridge, INA237
per-bus telemetry, AS5600 absolute encoder, 128-unit capacity,
product-grade compliance framing.

## Files

| File | Purpose |
|---|---|
| `ARCHITECTURE.md` | System block diagram, voltage and bus flow, addressing |
| `MASTER.md` | Master PCB design notes |
| `MASTER_BOM.csv` | Master PCB BOM |
| `UNIT.md` | Unit PCB design notes (with pogo pins, DIN rail clip) |
| `UNIT_BOM.csv` | Unit PCB BOM |
| `BUS_PCB.md` | DIN-rail bus PCB design (2× 300 mm per row, daisy-chained) |
| `BUS_PCB_BOM.csv` | Bus PCB BOM |
| `KICAD_HANDOFF.md` | KiCad 10 workflow + per-PCB hand-off package overview |
| `SCHEMATIC_MASTER.md` | Master PCB schematic + connection spec |
| `SCHEMATIC_UNIT.md` | Unit PCB schematic + connection spec |
| `SCHEMATIC_BUS.md` | Bus PCB schematic + custom artwork spec |
| `LAYOUT_MASTER.md` | Master PCB placement + routing guide |
| `LAYOUT_UNIT.md` | Unit PCB placement + routing guide |
| `LAYOUT_BUS.md` | Bus PCB placement + routing guide |
| `OPEN_DECISIONS.md` | Locked decisions + rationale |
| `KICAD_GETTING_STARTED.md` | **Beginner**: KiCad 10 install + universal setup, library map, ERC/DRC, plot/Gerber export |
| `KICAD_HOWTO_BUS.md` | **Beginner**: click-by-click build of the Bus PCB (start here) |
| `KICAD_HOWTO_UNIT.md` | **Beginner**: build of the Unit PCB (medium hand-holding) |
| `KICAD_HOWTO_MASTER.md` | **Beginner**: build of the Master PCB (least hand-holding; assumes you've done Bus + Unit) |

The RS-485 wire format + opcode set is a firmware concern, not a hardware
one. It is being designed alongside the rewritten unit firmware and is
intentionally not committed in this directory.

The v1 reference (`PCB/v1/`) is the deployed hardware.

## Hand-off model

User runs PCB layout in **KiCad 10.0** (decision 2026-04-26 after
external review froze the design). These docs serve as the spec —
schematic capture and routing happen in KiCad using the BOMs +
SCHEMATIC_*.md + LAYOUT_*.md as ground truth. See `KICAD_HANDOFF.md`
for tool-specific workflow.

## Building in KiCad (beginner)

If this is your first KiCad project, follow this order:

1. **`KICAD_GETTING_STARTED.md`** — install + universal setup. Read once.
2. **`KICAD_HOWTO_BUS.md`** — simplest board, full hand-holding.
3. **`KICAD_HOWTO_UNIT.md`** — most-replicated board (64 instances).
4. **`KICAD_HOWTO_MASTER.md`** — most complex board.

The howtos reference `SCHEMATIC_*.md`, `LAYOUT_*.md`, and
`KICAD_HANDOFF.md` for the source-of-truth specs — they walk you
through KiCad clicks, the spec docs tell you the correct values.
