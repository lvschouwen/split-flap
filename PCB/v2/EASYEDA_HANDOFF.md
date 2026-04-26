# EasyEDA Hand-off Package

**Revision:** 2026-04-26 (post Gemini + ChatGPT review merge)

**Status: HAND-OFF READY**. Changes since the previous revision:

**From Gemini review (kept):**
- Master 3V3 rail switched from linear LDO to switching buck (thermal fix).
- Master power trace widths spec'd as polygon pour (15 A on 1 oz copper requires it).
- TPL7407L footprint corrected to SOIC-16 narrow (was SOIC-16W).
- Unit J2 expanded to 5-pin JST-XH carrying +12 V (no 64× hand-splices).
- Bus PCB widened from 30 → 32 mm so trace Y-offsets exactly match unit pogo geometry.
- C_in cap upsized from 0805 → 1206 (X7R availability).

**From ChatGPT review (corrects/extends Gemini):**
- **HT7833 dropped from BOTH boards** — 6.5 V max VIN, would be destroyed at 12 V.
  Master = switching buck (already locked). **Unit = LDL1117S33 SOT-223** (**18 V op max /
  20 V abs max**, 1.2 A — earlier draft claimed 40 V; corrected per ChatGPT review of
  ST datasheet 2026-04-26). Optional: LM2937IMP-3.3 (26 V max) for more TVS-clamp margin.
- **SC16IS740 TSSOP-16 pinout rewritten** to correct NXP datasheet map (Gemini's was wrong;
  earlier draft was missing the I²C/SPI mode-select pin entirely).
- **STM32G030 K-32 pin map rewritten** — pin 20 is PC6 (not PA10); USART1 PA9/PA10/PA12
  go directly at pins 19/21/23 with no SYSCFG remap.
- **PG_KEY 5th polarization pogo + bus pad DELETED** — pad collision with PG1 + spring on
  bare FR-4 isn't a hard interlock. **Polarization moved to asymmetric 3D-printed DIN clip**.
- **Row connector locked to JST-VH 4-pin** (not JST-XH — XH is 3 A, row is fused at 4 A).
- **SMAJ15A polarity** explicit in spec (cathode → +12V); **master TVS placed AFTER F1 fuse**.
- **Z1 10 V Zener (BZT52C10)** added across Q1 VGS on both boards — 10 V (not 12 V) to keep
  AO3401A safely inside ±12 V VGS abs-max under Zener tolerance. Cross-validated by Gemini
  + ChatGPT external review 2026-04-26.
- **F_row polyfuse upsized 1812 → 2920** (1812 family doesn't support 4 A hold).
- **Bus mounting holes locked to 4** (not 2); bus terminator prose fixed to match table
  (120 Ω across pins 2-3, not 3-4).
- **Unit LED polarity** locked sink-driven everywhere (was contradictory).
- **ARCHITECTURE.md row-brick claim** corrected (one brick → master → 4 fused row outputs).
- **Hall connector pinout locked** to KY-003 native (pin 1 = +3V3, 2 = GND, 3 = HALL_OUT).
- **v1 mounting hole coordinates extracted** from Gerber (4× M3 at the four corners,
  3 mm in from each edge of the 80 × 40 mm board).

Per-PCB schematic + layout specifications ready for execution in
EasyEDA Pro (browser-based, integrates with JLCPCB fab).

## Locked decisions (no escalation needed; freelancer can act on these directly)

Recorded so the freelancer does not waste cycles re-litigating them.

1. **Row connector family — LOCKED to JST-VH 4-pin (B4P-VH-A,
   LCSC C144392 — CHECK).** 3.96 mm pitch, ≥5 A per pin (matches
   4 A row fuse with margin), keyed crimp housing. Use on master
   J3-J6 and both ends of every bus PCB. Earlier JST-XH lock was
   reversed after ChatGPT review (XH is 3 A — undersized for 4 A
   row). **Unit J2 stepper output stays JST-XH 5-pin** (B5B-XH-A,
   C158013) — 28BYJ-48 only sees ~250 mA. Replace any leftover
   `C124378` references in MASTER_BOM.csv before Gerbers.

2. **SC16IS740 TSSOP-16 pinout — LOCKED to ChatGPT's NXP map** (see
   `SCHEMATIC_MASTER.md` § SC16IS740). Authoritative pin list:

   | Pin | Signal | Pin | Signal |
   |---|---|---|---|
   | 1 | VDD | 9 | VSS |
   | 2 | /CS | 10 | RTS |
   | 3 | SI (MOSI) | 11 | CTS |
   | 4 | SO (MISO) | 12 | TX |
   | 5 | SCLK | 13 | RX |
   | 6 | VSS | 14 | RESET |
   | 7 | IRQ | 15 | XTAL1 |
   | 8 | I²C/SPI mode-sel (tie GND for SPI) | 16 | XTAL2 |

   **Verify on first article against NXP datasheet
   `SC16IS740_750_760.pdf` Figure 5 + Table 4** before fab — Gemini
   and an earlier draft both produced wrong maps for this part.

3. **STM32G030 K-suffix LQFP-32 pin map — LOCKED, no SYSCFG remap.**
   Per ST DS12991 Rev 6 Table 13: pin 19 = PA9, **pin 20 = PC6**,
   pin 21 = PA10, pin 22 = PA11, pin 23 = PA12. USART1 TX/RX/DE go
   directly to PA9/PA10/PA12 at AF1 with no remap dependency. PC6
   and PA11 stay NC. (Earlier draft assumed pin 20 = PA10 with a
   PA11/PA12 → PA9/PA10 remap; both wrong.)

4. **Unit DIN rail clip — LOCKED to 3D-printed asymmetric clip from
   MakerWorld** (one printed per unit; STL link in build notes).
   Bolts to unit board via 4× M3 holes at the v1 corner positions
   (3, 3), (3, 77), (37, 77), (37, 3) mm. The clip is the **only**
   polarization mechanism — PG_KEY pogo approach withdrawn.

5. **Hall sensor connector pinout — LOCKED**: J3 pin 1 = +3V3,
   pin 2 = GND, pin 3 = HALL_OUT. Matches KY-003 native order so
   v1's existing flying-lead cable plugs straight in. Different
   sensor modules adapt at the cable end.

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
- [ ] RS-485 idle bias at master end of each bus: A-B should sit at
      a **positive** differential of ~+100 mV (not +200 mV — the
      1 k/1 k bias network across two 120 Ω terminators in parallel
      gives a smaller divider than earlier drafts implied). The
      SN65HVD75 also has internal failsafe, so any positive idle
      reading > +50 mV is acceptable.
- [ ] Master 3V3 rail under load (boot ESP32-S3 + WiFi + all 4 PHYs
      idle): U2 buck module surface temp should stay below 50 °C.
- [ ] Unit 3V3 rail under load (stepper running, RS-485 active):
      U3 LDL1117S33 surface temp below 60 °C with the **VOUT-tab pour
      heatsinking** (tab is on 3V3 net, NOT GND — pouring tab to GND
      shorts 3V3 to GND through the internally-tied tab/pin-2 node).
      **LDL1117S33 pinout LOCKED: pin 1 = GND, pin 2 = VOUT (= tab),
      pin 3 = VIN.** **Verify HT7833 is NOT used** anywhere (max VIN
      6.5 V, would die at 12 V).
- [ ] **Master J3-J6 row output connectors are JST-VH 4-pin
      (B4P-VH-A, C144392)** — NOT JST-XH (3 A undersized) and NOT
      C124378 (1×40 single-row strip — wrong).
- [ ] **Bus PCB J_in / J_out are JST-VH 4-pin (B4P-VH-A, C144392).**
- [ ] **Unit J2 is JST-XH 5-pin (B5B-XH-A, C158013)** with pin 5 wired
      to PCB-12V rail (post-Q1) — 28BYJ-48 plugs in directly.
- [ ] **Unit J3 hall is JST-XH 3-pin (B3B-XH-A, C145756)**, pin 1
      = +3V3, pin 2 = GND, pin 3 = HALL_OUT.
- [ ] **Unit U2 (TPL7407L) footprint is SOIC-16 narrow (150 mil), NOT
      SOIC-16W (300 mil).**
- [ ] **Bus PCB outline is 300 × 32 mm**. **No PG_KEY ENIG pad** —
      polarization is enforced by the unit's 3D-printed DIN clip.
- [ ] **Master U2 is a switching buck module (K7803-1000R3 / R-78E3.3-1.0
      / V7803-1000, 1 A version), NOT a linear LDO and NOT the 500 mA
      version** (500 mA leaves zero headroom at ESP32-S3 WiFi TX peaks).
- [ ] **Unit U3 is a 12 V-capable LDO in SOT-223 (LDL1117S33TR or
      LM2937-3.3 or AP1117-33), NOT HT7833.**
- [ ] **Q1 (master + unit) has Z1 10 V Zener clamp (BZT52C10) gate→source**
      plus R_q1g 100 Ω series. **10 V (not 12 V)** to keep AO3401A safely
      inside ±12 V VGS abs-max under Zener tolerance. Required on unit Q1
      (AO3401A is only ±12 V VGS rated); recommended on master Q1 for
      symmetry / BOM commonality.
- [ ] **D8 / D4 (SMAJ15A) cathode (banded end) → +12V**, anode → GND.
      **Master D8 placement: AFTER F1 fuse**, before Q1.
      **Unit D4 placement: post-Q1 (load-side PCB-12V rail)** — earlier
      draft inconsistently said "post-pogo, before Q1"; locked here.
- [ ] **F_row polyfuses are 2920 package (Bourns MF-LSMF400/16X-2)**,
      NOT 1812 (1812 family does not support 4 A hold).
- [ ] **Master power input traces are polygon pours**, not narrow
      tracks (15 A on 1 oz copper requires a pour; 60 mil is
      insufficient).
- [ ] **Bus PCB has 4 mounting holes** (centreline, x = 8, 100, 200,
      292 mm at y = 16 mm), NOT 2 — prevents mid-span sag.
- [ ] **Bus terminator plug**: 120 Ω across **pin 2 (A) and pin 3 (B)
      only**; pins 1 (12V) and 4 (GND) NC.
- [ ] **Unit LEDs are sink-driven**: 3V3 → R_led → anode → cathode →
      MCU GPIO (MCU pulls low to illuminate). Pin table and LED
      section both reflect this; earlier drafts were inconsistent.
- [ ] **Unit mounting holes** at v1 corner positions: (3, 3), (3, 77),
      (37, 77), (37, 3) mm — extracted from `Gerber_Drill_NPTH.DRL`.
- [ ] **STM32G030 pin map** uses pin 19 PA9, **pin 20 PC6 (NC)**,
      pin 21 PA10, pin 22 PA11 (NC), pin 23 PA12 — **no SYSCFG remap**.
- [ ] **SC16IS740 pin 8 (I²C/SPI mode select) tied to GND** for SPI
      mode. Floating gives undefined startup behaviour.
- [ ] **SC16IS740 pin 11 (CTS) tied to GND** — input pin, must have a
      defined level (auto-flow disabled in firmware so polarity is
      don't-care, but it cannot float).
- [ ] **SC16IS740 pin 14 (RESET) pulled HIGH to 3V3 via 10 kΩ.**
      RESET is **active-LOW** per NXP datasheet — DO NOT tie to GND
      (that holds the chip in permanent reset). Gemini external review
      mistakenly claimed active-HIGH; confirmed wrong against NXP
      `SC16IS740_750_760.pdf` 2026-04-26.
- [ ] **Master firmware staggers motor steps** on power-up + global
      commands so peak input current stays under ~12 A. Without
      staggering, 64 units × 240 mA = 15.36 A trips the 15 A fuse.
