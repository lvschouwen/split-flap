# KiCad Netlist Hand-off

KiCad netlist files (`.net`) for the PCBs. Pure netlist format
(components + connections, no schematic graphics).

## Status

| PCB | File | Notes |
|---|---|---|
| Bus PCB | `bus_pcb/bus_pcb.net` | 4 nets, 8 nodes |
| Unit PCB | `unit/unit.net` | ~30 nets, ~80 nodes |
| Master PCB | not generated | bigger and more complex; generate on request if needed |

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
