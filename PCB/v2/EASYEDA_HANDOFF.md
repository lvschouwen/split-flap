# EasyEDA Hand-off Package

Per-PCB schematic + layout specifications ready for execution in
EasyEDA Pro (browser-based, integrates with JLCPCB fab).

## Workflow

1. Open EasyEDA Pro: https://pro.easyeda.com (free account, no
   software install).
2. Create a new PCB project per board (3 projects total).
3. For each project, follow the corresponding `SCHEMATIC_*.md`:
   - Add components (LCSC numbers given — paste into EasyEDA's
     parts library search, they auto-populate symbol + footprint).
   - Wire nets per the connection table.
   - Place components per the floorplan sketch.
   - Convert to PCB. Set layer rules per the layer spec.
   - Route, run DRC, fix violations.
   - Export Gerber + BOM + Pick-and-Place CSV.
4. Order from JLCPCB directly (EasyEDA has a one-click "Order at
   JLCPCB" button in the PCB editor).

## Per-PCB documents

| PCB | Schematic spec | BOM with LCSC |
|---|---|---|
| Bus PCB (×8) | `SCHEMATIC_BUS.md` | `BUS_PCB_BOM.csv` |
| Unit PCB (×64) | `SCHEMATIC_UNIT.md` | `UNIT_BOM.csv` |
| Master PCB (×1) | `SCHEMATIC_MASTER.md` | `MASTER_BOM.csv` |

## Order quantities (one full system)

| PCB | Qty for 1 system | JLC minimum order |
|---|---|---|
| Bus PCB | 8 | 5 (order 10 — better unit cost) |
| Unit PCB | 64 | 5 (order 70 — keeps a few spares) |
| Master PCB | 1 | 5 (order 5 — extras for risk) |

## LCSC part-number convention

Each BOM line lists the suggested LCSC part number. **Verify each one
in LCSC's catalogue (https://www.lcsc.com) before ordering.** Some
parts have multiple package variants — confirm the right one matches
the footprint listed.

LCSC numbers marked `CHECK` are starting points that need confirmation
at order time. JLC Basic parts (no setup fee) are preferred; Extended
parts add ~EUR 3 setup fee per unique part per order.

## EasyEDA Pro vs Standard

**Pro** is the newer version with:
- Better hierarchical schematic support
- Improved DRC
- Native JLCPCB integration (one-click order)

**Standard** still works and is simpler for hobbyist use. Either is
fine for these PCBs.

## Custom footprints

The bus PCB uses **custom contact pad shapes** for the pogo pin
landing zones — these need to be drawn manually in EasyEDA's PCB
editor (instructions in `SCHEMATIC_BUS.md`). All other footprints are
standard library parts.

## Layer / fab parameters (all PCBs unless noted)

| Parameter | Value |
|---|---|
| Layer count | 2 |
| Substrate | FR-4 |
| Thickness | 1.6 mm (bus PCB: 1.6 or 2.0) |
| Copper weight | 1 oz |
| Plating | HASL (master + unit); **ENIG required for bus PCB** |
| Solder mask | Green (standard) |
| Silk screen | White |
| Min trace / clearance | 5 mil / 5 mil (JLC standard) |
| Min drill | 0.3 mm |
| Min annular ring | 0.13 mm |

## Verification checklist before ordering

- [ ] All LCSC part numbers verified in LCSC catalogue.
- [ ] DRC passes with no errors.
- [ ] All component footprints have correct land patterns (compare
      against datasheets — EasyEDA's library is good but not perfect).
- [ ] Strap pins (ESP32-S3, SC16IS740) have correct boot-state pulls.
- [ ] Test pads accessible (not under components).
- [ ] Mounting holes don't interfere with traces or components.
- [ ] Silk screen labels readable (component refs, polarity, pin 1
      markers, IDENTIFY button label, RESET button label).
