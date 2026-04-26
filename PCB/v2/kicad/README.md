# KiCad Netlist Hand-off

KiCad netlist files (`.net`) for the PCBs. Pure netlist format
(components + connections, no schematic graphics).

## ⚠️ STALE — do not use as ground truth

The `unit.net` file references the **superseded PA0/PA1/PA2/PA3 USART2
pin assignment** that was withdrawn 2026-04-25 in favour of USART1 on
PA9/PA10/PA12 (with `/RE` tied to GND), and still references the
**HT7833 LDO** that was replaced with **LDL1117S33** on the same date
(HT7833 = 6.5 V max VIN, would be destroyed at 12 V). It has not been
regenerated.

The **`SCHEMATIC_*.md` files are the ground truth.** Treat the `.net`
files as historical artifacts only. A freelancer should build the
schematic from `SCHEMATIC_UNIT.md`, `SCHEMATIC_BUS.md`, and
`SCHEMATIC_MASTER.md` directly rather than importing these netlists.

## Status

| PCB | File | Notes |
|---|---|---|
| Bus PCB | `bus_pcb/bus_pcb.net` | 4 nets, 8 nodes — still matches SCHEMATIC_BUS.md |
| Unit PCB | `unit/unit.net` | **STALE** — uses old PA0–PA3 USART pinout |
| Master PCB | not generated | spec is large; never machine-generated |

## Caveat

These `.net` files were hand-written from the SCHEMATIC_*.md specs.
They have not been validated against KiCad's parser — there is a
real chance EasyEDA's importer will reject some fields.

## How to import into EasyEDA Pro

1. Open EasyEDA Pro: https://pro.easyeda.com
2. Create a new project for the PCB.
3. `File -> Import -> KiCad...` → select the `.net` file.
4. EasyEDA imports components + ratsnest into the PCB editor (no
   schematic — `.net` is a netlist-only format).
5. Place components + route per the floorplan in the corresponding
   `SCHEMATIC_*.md`.

## If imports fail

Fall back to manual schematic capture in EasyEDA using the
`SCHEMATIC_*.md` files as the connection reference. Those are the
ground truth — connections are unambiguous, just mechanical to wire
up by hand.

## Why no `.kicad_sch` files

Earlier attempts to hand-write `.kicad_sch` schematic files failed
KiCad's parser ("Expecting '(' " errors) due to schema differences
between KiCad versions. The simpler `.net` format avoids most of
those parsing pitfalls. The schematics live in markdown form
(`SCHEMATIC_*.md`).
