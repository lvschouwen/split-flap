# KiCad Project Directory

This directory holds the KiCad 10 projects for the three v2 boards.
The subfolders (`master/`, `unit/`, `bus/`) are created as you work
through the `KICAD_HOWTO_*.md` walkthroughs — one project per board.

## Netlists removed 2026-07-04 (#102)

The hand-written `unit/unit.net` and `bus_pcb/bus_pcb.net` netlist
files were **deleted** in the #102 doc-correction pass. They predated
the 2026-04-26 review corrections and encoded the rejected pre-review
design — they contradicted essentially every locked decision.
Examples of what they still contained:

- **HT7833 LDO** on the unit — 6.5 V max VIN, destroyed at 12 V
  (replaced by LM2937IMP-3.3, LDL1117S33 alternate).
- **Q1 miswired and not in series** with the 12 V input — no
  reverse-block function at all.
- **On-board A1101 hall sensor** — the hall sensor lives on the
  KY-003-style lead (3.3 V-rated DRV5023/AH3366Q), not on the PCB.
- **2.54 mm C124378 bus headers** — the row/bus connector is JST-VH
  3.96 mm (B4P-VH-A).
- **TestPoint pogo footprints with 0.8 mm drills** — a Mill-Max 0906
  needs a 1.83 mm drill; the footprint physically cannot fit the part.

Because `.net` files are machine-readable, they were the most
dangerous stale artifact in the repo: an importer would silently
reproduce the rejected design. Hence deletion rather than a warning
banner.

## Authoritative capture sources

Schematic capture happens **natively in KiCad 10**, using **official
KiCad library symbols** (plus the few custom footprints described in
the HOWTOs). The ground truth to capture from:

| Source | Role |
|---|---|
| `SCHEMATIC_MASTER.md` / `SCHEMATIC_UNIT.md` / `SCHEMATIC_BUS.md` | Nets, MPNs, pin maps |
| `MASTER_BOM.csv` / `UNIT_BOM.csv` / `BUS_PCB_BOM.csv` | Parts with LCSC codes |
| `KICAD_HOWTO_MASTER.md` / `KICAD_HOWTO_UNIT.md` / `KICAD_HOWTO_BUS.md` | Click-by-click capture + layout workflow |

No master-board netlist was ever generated — the master spec was
always capture-from-markdown only, and that is now the rule for all
three boards.
