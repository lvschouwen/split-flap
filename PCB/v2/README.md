# PCB v2

Hobby-scale redesign of the split-flap display electronics.

Tracked under issue #83.

## Status

- **Phase:** design (pre-schematic-capture)
- **Branch:** `pcb-v2-rs485-48v` (will be renamed once architecture stabilises)
- **Top open decisions:** see `OPEN_DECISIONS.md`.

## Scope

- 1 master PCB (ESP32-S3, USB-C, 4 RS-485 row ports).
- 4 rows × 16 unit slots = **64 units max.**
- 4 per-row harnesses (no rigid backplane).
- **One 12 V / 15 A brick.** Master sources both power and signal to
  each row over a single 6-pin combined cable.
- **Addressing: STM32 96-bit UID** + per-unit IDENTIFY button. No DIP
  switches, no slot wiring on the harness.

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
| `UNIT.md` | Unit PCB design notes |
| `UNIT_BOM.csv` | Unit PCB BOM |
| `HARNESS.md` | Per-row wiring spec (cable, drops, terminator) + parts list |
| `OPEN_DECISIONS.md` | Pending decisions blocking schematic capture |

The RS-485 wire format + opcode set is a firmware concern, not a hardware
one. It is being designed alongside the rewritten unit firmware and is
intentionally not committed in this directory.

The v1 reference (`PCB/v1/`) is the deployed hardware. The v1->v2
hardware-only changelog lives at `PCB/REVIEW/CHANGELOG_V1_TO_V2.md`.

## Hand-off model

The user does not run PCB layout. These docs target a freelance PCB
designer or JLCPCB EasyEDA importer. Schematic capture and routing are
done by the implementer using the BOMs + design notes here as the spec.
