# KiCad Hand-off Package

**Revision:** 2026-04-26
**Tool:** KiCad 10.0 desktop app (free, open source — https://kicad.org)
**Replaces:** the prior EasyEDA Pro workflow (issue #87 — user switched to KiCad).

**Status: HAND-OFF READY.** Architecture, MPNs, footprints, pinouts
are FROZEN as of issue #86 (closed by 060633f). Layout is the only
remaining work.

---

## What this doc is

The **tool-specific workflow** for getting from the schematic specs
in `SCHEMATIC_*.md` and the placement guides in `LAYOUT_*.md` into
JLC-ready Gerbers using KiCad 10.

Everything else (nets, MPNs, pin maps, placement zones, copper pour
rules, verification checklists) lives in:

| Doc | Purpose |
|---|---|
| `MASTER.md` / `UNIT.md` / `BUS_PCB.md` | High-level board spec |
| `SCHEMATIC_MASTER.md` / `SCHEMATIC_UNIT.md` / `SCHEMATIC_BUS.md` | Nets, MPNs, pin maps |
| `LAYOUT_MASTER.md` / `LAYOUT_UNIT.md` / `LAYOUT_BUS.md` | Placement + routing rules |
| `MASTER_BOM.csv` / `UNIT_BOM.csv` / `BUS_PCB_BOM.csv` | BOMs with LCSC codes |
| `ARCHITECTURE.md` | System view |
| `OPEN_DECISIONS.md` | Locked decisions + rationale |

The schematic specs are tool-agnostic — the same nets/MPNs/footprints
apply whether you draw them in KiCad, EasyEDA, or by hand.

---

## Locked decisions (no escalation needed)

Recorded so the layout work doesn't waste cycles re-litigating them.
These came out of internal audit + Gemini + ChatGPT external review
(issues #85 + #86).

1. **Row connector — JST-VH 4-pin (B4P-VH-A, LCSC C144392 — CHECK).**
   3.96 mm pitch, ≥5 A per pin. Master J3–J6 + both ends of every
   bus PCB. **Unit J2 stepper output stays JST-XH 5-pin** (B5B-XH-A,
   C158013) since 28BYJ-48 only sees ~250 mA.

2. **Master U2 = K7803-1000R3** (1 A switching buck), NOT 500 mA and
   NOT a linear LDO. SIP-3 footprint, drop-in for L78xx pinout
   (VIN/GND/VOUT). LCSC C113973 (CHECK 1A version).

3. **Unit U3 = LDL1117S33TR** in SOT-223. **18 V op max / 20 V abs
   max** (NOT 40 V — earlier doc claim was wrong). **Pinout LOCKED
   to LM1117 family convention: pin 1 = GND, pin 2 = VOUT (= tab),
   pin 3 = VIN.** Heatsink copper pour goes on **VOUT (pin 2 + tab)**
   — NOT GND (the tab is internally tied to VOUT; pouring tab to
   GND shorts 3V3 to GND). Optional alt: LM2937IMP-3.3
   (LCSC C140265, 26 V max).

4. **Z1 = BZT52C10** (10 V Zener, NOT 12 V). LCSC C8062 (CHECK).
   Both master and unit use the same part for BOM commonality.
   12 V Zener at +10% tolerance (13.2 V) puts AO3401A's ±12 V VGS
   abs-max out of spec.

5. **F_row polyfuses = 2920 SMD** (NOT 1812 — 1812 family doesn't
   support 4 A hold). Bourns MF-LSMF400/16X-2.

6. **SC16IS740 TSSOP-16 pinout LOCKED** to NXP datasheet
   `SC16IS740_750_760.pdf` Figure 5 + Table 4 (see
   `SCHEMATIC_MASTER.md` § SC16IS740). Pin 14 RESET is
   **active-LOW** — pulled HIGH to 3V3 via 10 kΩ for normal
   operation. Pin 8 (mode-sel) tied to GND for SPI mode. Pin 11
   (CTS) tied to GND.

7. **STM32G030 K-suffix LQFP-32 pin map LOCKED**, no SYSCFG remap.
   Pin 19 = PA9, pin 20 = PC6, pin 21 = PA10, pin 22 = PA11,
   pin 23 = PA12. USART1 TX/RX/DE go directly at AF1.

8. **Polarization = asymmetric 3D-printed DIN clip** (MakerWorld
   STL). NO PG_KEY pogo, NO PG_KEY ENIG pad on bus.

9. **Hall connector pinout LOCKED**: J3 pin 1 = +3V3, pin 2 = GND,
   pin 3 = HALL_OUT (KY-003 native).

10. **Firmware HARD requirement**: master must stagger motor steps
    on power-up + global commands (peak input current under ~12 A
    so the 15 A fuse doesn't nuisance-trip).

---

## KiCad 10 workflow

### 1. Install

Download KiCad 10.0 from https://kicad.org/download/ for your OS.
Install with the default symbol/footprint/3D libraries — they cover
most of what we need.

### 2. Project layout (one project per board)

```
PCB/v2/kicad/
├── master/    splitflap-master-v2.kicad_pro + .kicad_sch + .kicad_pcb
├── unit/      splitflap-unit-v2.kicad_pro + .kicad_sch + .kicad_pcb
└── bus/       splitflap-bus-v2.kicad_pro + .kicad_sch + .kicad_pcb
```

The existing `kicad/unit/unit.net` and `kicad/bus_pcb/bus_pcb.net`
files are **STALE** (pre-merge state with the wrong USART pinout
and the HT7833 LDO). Don't import them — start each project fresh
from the SCHEMATIC_*.md ground truth.

### 3. Symbol + footprint libraries

KiCad 10's built-in libraries cover most components. Specifically:

| Component | KiCad library | Notes |
|---|---|---|
| Generic R/C/D/LED | `Device` | Use 0603 for resistors, 0805/1206 for caps per BOM |
| ESP32-S3-WROOM-1-N16R8 | **Custom or Espressif's KiCad lib** | Espressif maintains an official KiCad lib at https://github.com/espressif/kicad-libraries — clone and add as a library path. Or hand-draw. |
| STM32G030K6T6 | `MCU_ST_STM32G0` | LQFP-32 footprint `Package_QFP:LQFP-32_7x7mm_P0.8mm` |
| SC16IS740IPW | **Custom** | NXP doesn't ship a KiCad lib. Hand-draw symbol per datasheet pinout (locked in `SCHEMATIC_MASTER.md`). Footprint: `Package_SO:TSSOP-16_4.4x5mm_P0.65mm` |
| SN65HVD75DR | `Interface_UART` or `Driver_FET` | TI provides a symbol; if absent, hand-draw. Footprint: `Package_SO:SOIC-8_3.9x4.9mm_P1.27mm` |
| TPL7407L | **Custom or generic ULN2003** | TPL7407L pinout is footprint-compatible with ULN2003A — reuse `Driver_Motor:ULN2003A` symbol, but verify the **SOIC-16 NARROW (150 mil)** footprint: `Package_SO:SOIC-16_3.9x9.9mm_P1.27mm`. Do NOT use SOIC-16W (300 mil = `SOIC-16W_7.5x10.3mm_P1.27mm`). |
| LDL1117S33TR | `Regulator_Linear:LM1117-3.3` symbol works (LM1117 family pinout) | Footprint: `Package_TO_SOT_SMD:SOT-223-3_TabPin2`. **Pin 2 = VOUT = tab, pin 1 = GND, pin 3 = VIN.** |
| K7803-1000R3 | **Custom** | SIP-3 module. Footprint: any L78xx SIP-3 (e.g., `Package_TO_SOT_THT:TO-220-3_Vertical`) — verify pin order matches K7803 datasheet (VIN/GND/VOUT). |
| AOD409 P-FET | `Transistor_FET` (P-channel symbol) | Footprint: `Package_TO_SOT_SMD:TO-252-2` (DPAK) |
| AO3401A P-FET | `Transistor_FET:AO3401A` if present, else generic P-channel symbol | Footprint: `Package_TO_SOT_SMD:SOT-23` |
| BZT52C10 Zener | `Diode:BZT52C10` if present, else generic Zener | Footprint: `Diode_SMD:D_SOD-123` |
| SMAJ15A TVS | `Diode_TVS:SMAJ15A-13-F` or generic | Footprint: `Diode_SMD:D_SMA` (DO-214AC) |
| SM712-02HTG | **Custom** | Hand-draw 3-pin symbol. Footprint: `Package_TO_SOT_SMD:SOT-23` |
| USBLC6-2SC6 | `Power_Protection:USBLC6-2SC6` | Footprint: `Package_TO_SOT_SMD:SOT-23-6` |
| Crystal 14.7456 MHz | `Device:Crystal` | Footprint: `Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm` |
| USB-C receptacle | `Connector_USB:USB_C_Receptacle_USB2.0_16P` | KiCad 10 has it. Footprint matches GCT/Amphenol generics. |
| JST-VH 4-pin | `Connector_JST:JST_VH_B4P-VH-A` | Footprint same name. |
| JST-XH 5-pin | `Connector_JST:JST_XH_B5B-XH-A` | Footprint same name. |
| JST-XH 3-pin | `Connector_JST:JST_XH_B3B-XH-A` | Footprint same name. |
| Mill-Max pogo 0906-2 | **Custom** | Hand-draw single-hole footprint: 1.83 mm drill, 2.45 mm pad, no mask opening. THT (through-hole). |
| 2-pin screw terminal KF7.62 | `Connector_Phoenix_GMSTB:PhoenixContact_GMSTB_*_02_*` or generic | 7.62 mm pitch THT |

### 4. LCSC field for JLC assembly

JLC's PCBA service needs MPN → LCSC mapping. In KiCad 10 add a
custom symbol field per part:

1. Open Schematic Editor → select part → Properties.
2. Under "Custom Fields", add:
   - **Field name**: `LCSC` (must match exactly)
   - **Value**: the LCSC part number from the BOM CSV (e.g., `C113973`).
3. Apply to all parts. Use **"Edit Symbol Fields Table"** (Tools menu)
   to bulk-edit — paste the LCSC column from the BOM CSV directly.

When exporting BOM for JLC assembly, include this `LCSC` field.

### 5. Per-board build steps

For each of the 3 projects (`master`, `unit`, `bus`):

1. **New project** in KiCad 10 → name per the project layout above.
2. **Schematic capture** following `SCHEMATIC_<board>.md`:
   - Place symbols (use the libraries above).
   - Wire nets — use net labels for clarity (`12V`, `GND`, `3V3`,
     `RS485_A0`, `UART0_TX`, etc.).
   - Add the `LCSC` field per part.
   - Run **ERC** (Inspect → Electrical Rules Checker). Fix all
     errors. Warnings can usually stay if they're "no_connect"
     intentional.
3. **Footprint association** (Tools → Edit Symbol Fields Table OR
   Tools → Footprint Assignment Tool / CvPcb-style):
   - Verify every symbol has a footprint.
   - **Triple-check the SOT-223 LDO** (LM1117-style: pin 1 GND,
     pin 2 VOUT = tab, pin 3 VIN — the `SOT-223-3_TabPin2`
     footprint matches).
   - **Triple-check the TPL7407L** is SOIC-16 narrow (150 mil),
     not SOIC-16W (300 mil).
4. **Update PCB from Schematic** (or click "Open PCB" in the
   project manager).
5. **Set board outline** per `LAYOUT_<board>.md` (rectangle of the
   right dimensions, drawn on the `Edge.Cuts` layer).
6. **Place mounting holes** per the LAYOUT spec (KiCad 10 has
   `MountingHole_3.2mm_M3` footprints for NPTH).
7. **Layout per `LAYOUT_<board>.md`**: placement zones, routing
   widths, copper pours.
8. **DRC** (Inspect → Design Rules Checker). Fix all errors.
9. **3D viewer** sanity check (View → 3D Viewer) — catches
   placement collisions visually.
10. **Plot Gerbers** for JLC (see "Gerber export" below).

### 6. Custom footprints to draw

These aren't in KiCad's stock libraries:

#### Mill-Max pogo pin (PG1–PG4 on unit, contact zones on bus)

- **Drill**: 1.83 mm
- **Pad**: 2.45 mm round
- **Layer**: F.Cu (top) + B.Cu (bottom) since THT
- **Solder mask**: opening on bottom (the contact side) for
  ENIG/HASL plating
- Single-hole footprint, refdes = `PG`

Save as `splitflap-custom:Pogo_MillMax_0906-2_THT_1.83mm`.

#### Bus PCB contact zone (per station, ×4 nets per station)

- **12V / GND zones**: 5 × 5 mm rectangle, top layer, mask off.
- **A / B zones**: 3 × 5 mm rectangle, top layer, mask off.
- Drawn as Solid Polygon copper on `F.Cu`, with corresponding
  rectangle on `F.Mask` (mask-clear layer) of the same shape.

These don't need to be footprints — draw them as **graphic
polygons** directly in the PCB editor connected to the appropriate
net via "Edit → Edit Properties → Net".

KiCad 10 makes this easier than KiCad 8 with the improved zone
manager.

### 7. Differential pair routing

KiCad 10 has native differential pair support (Route → Differential
Pair, hotkey `6`).

- **USB D+/D-**: declare as a differential pair in the Net Classes
  panel. Set differential pair gap to ~0.15 mm, width 0.25 mm
  (10 mil). KiCad will auto-route them as a coupled pair.
- **RS-485 A/B**: set as a loose differential pair (gap 0.2 mm,
  width 0.25 mm). 250 kbaud is slow — controlled impedance not
  required.

### 8. Copper zones (poured copper for power)

For the master 15 A power path (per `LAYOUT_MASTER.md`):

1. Right-click on layer → "Add Filled Zone".
2. Choose layer (F.Cu and B.Cu both — do two zones).
3. Assign to net `PCB-12V_RAIL` (post-Q1) for the trunk; or to
   `+12V_INPUT` for the pre-Q1 segment.
4. Draw outline covering the area between J1 and the per-row
   branch point.
5. Set thermal relief properties: hatched, gap 0.5 mm.
6. Repeat for GND on both layers.
7. **Press `B` to fill all zones** — KiCad doesn't auto-fill on
   change. Always re-fill before exporting Gerbers.

For the unit and bus PCBs, GND is also a zone (full board minus
1 mm edge clearance) on the bottom layer.

### 9. DRC settings

In **File → Board Setup → Design Rules → Constraints**:

| Rule | Value |
|---|---|
| Minimum copper clearance | 0.127 mm (5 mil) |
| Minimum track width | 0.127 mm (5 mil) |
| Minimum hole | 0.3 mm |
| Minimum annular ring | 0.13 mm |
| Edge clearance | 0.5 mm to copper, 0.3 mm to silk |

These are JLC's standard rules — set the same on all 3 projects.

### 10. Gerber export for JLC

KiCad 10 has a **JLCPCB preset** under File → Plot:

1. **File → Plot** (Ctrl+P).
2. Layers to plot:
   - F.Cu, B.Cu
   - F.Mask, B.Mask
   - F.Silkscreen, B.Silkscreen
   - F.Paste, B.Paste (only needed if assembling at JLC)
   - Edge.Cuts (board outline)
3. **Use Protel filename extensions** (JLC parses these natively).
4. Output directory: `gerbers/`.
5. Generate Drill Files: PTH and NPTH separately, **Excellon**
   format, decimal format with 2:4 precision.
6. Zip the output directory and upload to JLCPCB.

### 11. BOM + CPL export for JLC assembly

If using JLC PCBA:

1. **BOM**: Tools → Generate BOM. Use the "JLCPCB" template if
   available, or export with columns: `Comment` (Value),
   `Designator` (Reference), `Footprint`, `LCSC` (custom field).
2. **CPL** (Centroid / Pick-and-Place): File → Fabrication
   Outputs → Component Placement (.pos) Files. Use **mm** units,
   **Use auxiliary axis as origin** if you set the aux origin at
   the board's bottom-left corner. Format: ASCII, comma-separated,
   columns Designator/Mid X/Mid Y/Layer/Rotation.

Or just use a plugin (next section).

### 12. JLC integration plugin (optional but recommended)

The community-maintained **Fabrication Toolkit** plugin
(https://github.com/bennymeg/Fabrication-Toolkit) automates
BOM/CPL formatting for JLC. Install via KiCad's **Plugin and
Content Manager** (PCM). One-click export of Gerbers + Drill +
BOM + CPL in JLC's expected format.

Alternative: **JLCPCB-Plugin-for-KiCad**
(https://github.com/wokwi/kicad-jlcpcb-bom-plugin) — older but
also works.

### 13. Order quantities

| PCB | Qty | Notes |
|---|---|---|
| Master | 5 | extras for risk |
| Unit | 70 | covers 64 + spares |
| Bus | 10 | covers 8 + spares |

ENIG plating required for the **bus PCB only** (pogo contacts
need gold). Master + unit are HASL.

---

## Verification checklist before ordering

(Same content as the old EASYEDA_HANDOFF.md verification list —
this is tool-agnostic, just kept here for completeness.)

- [ ] All LCSC part numbers verified in LCSC catalogue.
- [ ] DRC passes with no errors.
- [ ] All component footprints have correct land patterns (compare
      against datasheets — KiCad's library is good but not perfect,
      especially for less common parts).
- [ ] **Strap pins (ESP32-S3 IO0/IO3/IO45/IO46, SC16IS740 RESET)**
      have correct boot-state pulls. **IO46 must have an explicit
      external 10 kΩ pull-down** (do not rely on internal). RESET
      pulled HIGH to 3V3 (RESET is active-LOW).
- [ ] Test pads accessible (not under components).
- [ ] Mounting holes don't interfere with traces or components.
- [ ] Silk screen labels readable (component refs, polarity, pin 1
      markers, IDENTIFY button label, RESET button label, "TOP" /
      "BOTTOM" markers).
- [ ] **Polarization = asymmetric 3D-printed DIN clip on unit**;
      NO PG_KEY pogo and NO PG_KEY ENIG pad anywhere.
- [ ] **Master Q1 + Unit Q1**: P-FET drain to incoming 12 V, source
      to load rail, gate to GND via 10 kΩ. Reverse-block topology.

## Bring-up acceptance tests (post-fab)

- [ ] J1 input current measurement after Q1 (verify reverse-block:
      flip polarity, no current; flip back, current flows normally).
- [ ] Pogo-pin contact resistance per pin: < 100 mΩ at 4 A.
- [ ] RS-485 idle bias at master end of each bus: A-B should sit at
      a positive differential of ~+100 mV.
- [ ] Master 3V3 rail under load (boot ESP32-S3 + WiFi + all 4 PHYs
      idle): U2 buck module surface temp below 50 °C.
- [ ] Unit 3V3 rail under load (stepper running, RS-485 active):
      U3 LDL1117S33 surface temp below 60 °C with the **VOUT-tab**
      pour heatsinking (tab on 3V3 net, NOT GND).
- [ ] **Master J3-J6 row output connectors are JST-VH 4-pin**
      (B4P-VH-A, C144392) — NOT JST-XH (3 A undersized).
- [ ] **Bus PCB J_in / J_out are JST-VH 4-pin** (B4P-VH-A, C144392).
- [ ] **Unit J2 is JST-XH 5-pin** (B5B-XH-A, C158013) with pin 5
      wired to PCB-12V rail (post-Q1).
- [ ] **Unit J3 hall is JST-XH 3-pin** (B3B-XH-A, C145756), pin 1 =
      +3V3, pin 2 = GND, pin 3 = HALL_OUT.
- [ ] **Unit U2 (TPL7407L) footprint is SOIC-16 narrow (150 mil)**,
      NOT SOIC-16W.
- [ ] **Bus PCB outline is 300 × 32 mm**. **No PG_KEY ENIG pad.**
- [ ] **Master U2 is K7803-1000R3** switching buck (1 A version),
      NOT 500 mA and NOT a linear LDO.
- [ ] **Unit U3 is LDL1117S33TR** (or LM2937IMP-3.3 alt), SOT-223,
      18 V op max. **Pinout: pin 1 = GND, pin 2 = VOUT (= tab),
      pin 3 = VIN.** **VOUT-tab pour heatsinking** (NOT GND).
- [ ] **Q1 (master + unit) has Z1 BZT52C10 (10 V Zener)** clamp
      gate→source plus R_q1g 100 Ω series and R_q1g2 10 kΩ
      pull-down. **10 V (not 12 V)** to keep AO3401A inside ±12 V
      VGS abs-max under Zener tolerance.
- [ ] **D8 / D4 (SMAJ15A) cathode (banded end) → +12V**, anode →
      GND. **Master D8 placed AFTER F1 fuse**, before Q1.
      **Unit D4 placement: post-Q1 (load-side PCB-12V rail)**.
- [ ] **F_row polyfuses are 2920 package** (Bourns
      MF-LSMF400/16X-2), NOT 1812.
- [ ] **Master power input is polygon zone** (poured copper) on
      both layers, NOT narrow tracks.
- [ ] **Bus PCB has 4 mounting holes** (centreline, x = 8 / 100 /
      200 / 292 mm at y = 16 mm), NOT 2.
- [ ] **Bus terminator plug**: 120 Ω across pin 2 (A) and pin 3 (B)
      only; pins 1 (12V) and 4 (GND) NC.
- [ ] **Unit LEDs are sink-driven**: 3V3 → R_led → anode → cathode
      → MCU GPIO.
- [ ] **Unit mounting holes** at v1 corner positions (3, 3),
      (3, 77), (37, 77), (37, 3) mm.
- [ ] **STM32G030 pin map**: pin 19 PA9, pin 20 PC6 (NC),
      pin 21 PA10, pin 22 PA11 (NC), pin 23 PA12. **No SYSCFG
      remap.**
- [ ] **SC16IS740 pin 8 (mode-sel) tied to GND** for SPI mode.
- [ ] **SC16IS740 pin 11 (CTS) tied to GND** — input pin.
- [ ] **SC16IS740 pin 14 (RESET) pulled HIGH to 3V3 via 10 kΩ.**
      RESET is **active-LOW** per NXP datasheet — do NOT tie to
      GND.
- [ ] **Master firmware staggers motor steps** so peak input
      current stays under ~12 A.
