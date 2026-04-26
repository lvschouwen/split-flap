# PCB v2

**Revision:** 2026-04-26

Hobby-scale redesign of the split-flap display electronics.

Tracked under issue #83.

## Status

- **Phase:** FROZEN (architecture, MPNs, footprints, pinouts locked
  by issue #86 / commit 060633f). Layout in KiCad 10 is the only
  remaining work.
- **Branch:** `pcb-v2-rs485-48v`
- **Locked decisions:** see `KICAD_HANDOFF.md` § "Locked decisions"
  and `OPEN_DECISIONS.md`.

## Scope

- 1 master PCB (ESP32-S3, USB-C, 4 RS-485 row ports).
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

The RS-485 wire format + opcode set is a firmware concern, not a hardware
one. It is being designed alongside the rewritten unit firmware and is
intentionally not committed in this directory.

The v1 reference (`PCB/v1/`) is the deployed hardware. The v1->v2
hardware-only changelog lives at `PCB/REVIEW/CHANGELOG_V1_TO_V2.md`.

## Hand-off model

User runs PCB layout in **KiCad 10.0** (decision 2026-04-26 after
external review froze the design). These docs serve as the spec —
schematic capture and routing happen in KiCad using the BOMs +
SCHEMATIC_*.md + LAYOUT_*.md as ground truth. See `KICAD_HANDOFF.md`
for tool-specific workflow.
