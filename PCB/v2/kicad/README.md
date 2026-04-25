# KiCad Schematic Hand-off

Importable KiCad schematic files for the 3 PCBs. EasyEDA Pro accepts
these via `File -> Import -> KiCad`.

## Status

| PCB | Status |
|---|---|
| `bus_pcb/` | proof-of-concept ready (2 components, 4 nets) |
| `unit/` | not yet generated — pending bus PCB import validation |
| `master/` | not yet generated — pending bus PCB import validation |

## How to import into EasyEDA Pro

1. Open EasyEDA Pro: https://pro.easyeda.com
2. Create a new project for the PCB you're importing.
3. `File -> Import -> KiCad...`
4. Select the `.kicad_sch` file from the corresponding folder.
5. EasyEDA imports symbols + nets. Component placement will be on a
   grid (visually unpolished — rearrange for readability).
6. Verify symbols mapped correctly. EasyEDA may ask you to manually
   match a few less-common symbols against its library. The LCSC#
   custom field on each component helps here.
7. Convert to PCB and lay out per the floorplan in the corresponding
   `SCHEMATIC_*.md`.

## Caveat

These KiCad files contain **schematic only** — no PCB layout. You do
PCB placement, routing, pours, and DRC interactively in EasyEDA.

## What gets imported

Per file:
- All components with their reference designators (J_in, U1, etc.)
- All net connections (wires + labels)
- LCSC numbers in custom fields
- Component values + footprint hints
- No PCB layout, no Gerbers

## What does NOT get imported

- Schematic visual placement quality (you'll rearrange)
- Symbol graphical look (EasyEDA uses its own symbol library, may
  look different from KiCad's)
- Anything PCB-related

## Verification

After import, cross-check:
- Component count matches the corresponding `*_BOM.csv`
- Net labels match the `SCHEMATIC_*.md` connection tables
- No floating pins (use EasyEDA's ERC tool)
