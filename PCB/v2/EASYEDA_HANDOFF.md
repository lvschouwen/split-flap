# EasyEDA Hand-off Package

**Revision:** 2026-04-26 (post-datasheet-sweep + independent review)

**Status: HAND-OFF READY** — pin maps verified, BOMs reconciled,
reverse-block topology corrected, polarization mandated, BOM gaps
filled. The 5 escalation items below are decisions the freelancer
must confirm with the user before ordering fab; none of them block
schematic capture or layout work.

Per-PCB schematic + layout specifications ready for execution in
EasyEDA Pro (browser-based, integrates with JLCPCB fab).

## ⚠️ Items the freelancer must escalate before fab order

Before sending Gerbers to JLC, confirm with the user:

1. **Row connector family / LCSC code** — `OPEN_DECISIONS.md §4`. The
   BOMs reference `C124378` for the 4-pin row connector but that's a
   plain 1×40 pin-header strip with no shroud or key. User has
   deferred the choice; recommended **JST-XH 4-pin (B4B-XH-A,
   C158012)** for BOM consolidation with the unit-board stepper
   header. **Do not order with C124378 as-listed.**
2. **SC16IS740 pin-by-pin map** — `SCHEMATIC_MASTER.md` lists power
   pins (1, 5/6 XTAL, 15, 16) by number; remaining 9 signal pins are
   listed by signal only. Pull NXP datasheet `SC16IS740_750_760.pdf`
   Figure 5 + Table 4 and lock the pin numbers in EasyEDA before
   starting layout.
3. **STM32G030 USART1 pin remap** — `SCHEMATIC_UNIT.md` uses PA9/PA10
   for TX/RX, which on K-32 are remapped from PA11/PA12 by SYSCFG bits.
   Schematic net "PA9" lands on the package pin labelled PA11 in
   pre-remap state. Firmware sets `SYSCFG_CFGR1.PA11_RMP` at boot.
   Verify against DS12991 Rev 6 Table 13 before placing the MCU.
4. **DIN rail clip MPN** — `UNIT_BOM.csv` lists "generic or 3D-printed".
   The unit PCB's mounting hole pattern depends on the clip. Pick a
   specific clip (Phoenix CLIPFIX 35 or a committed 3D-print STL)
   before locking the unit board outline.
5. **Hall sensor module + chassis bracket** — `SCHEMATIC_UNIT.md`
   spec'd as off-board KY-003 on flying lead via J3 (3-pin JST-XH).
   Confirm with user that the chassis bracket holds the hall module
   in alignment with the flap-drum magnet path.

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
- [ ] Strap pins (ESP32-S3 IO0/IO3/IO45/IO46, SC16IS740 RESET) have
      correct boot-state pulls — IO46 must have an **explicit
      external 10 kΩ pull-down** (not just internal).
- [ ] Test pads accessible (not under components).
- [ ] Mounting holes don't interfere with traces or components.
- [ ] Silk screen labels readable (component refs, polarity, pin 1
      markers, IDENTIFY button label, RESET button label, "TOP" /
      "BOTTOM" markers on unit + bus PCBs for polarization).
- [ ] Polarization key on unit-to-bus interface (5th pogo pin OR
      hard-keyed DIN clip, see BUS_PCB.md §Polarization).
- [ ] **Master Q1 + Unit Q1**: P-FET drain to incoming 12 V, source to
      load rail, gate to GND via 10 kΩ. Reverse-block topology.

## Bring-up acceptance tests (post-fab)

- [ ] J1 input current measurement after Q1 (verify reverse-block:
      flip polarity, no current should flow; flip back, current
      flows normally).
- [ ] Pogo-pin contact resistance per pin: < 100 mΩ at 4 A.
- [ ] RS-485 idle bias measurement at master end of each bus: A-B
      should sit at ~+200 mV with no traffic.
- [ ] Master 3V3 rail under load (boot ESP32-S3 + WiFi + all 4 PHYs
      idle): HT7833 surface temp should stay below 70 °C.
