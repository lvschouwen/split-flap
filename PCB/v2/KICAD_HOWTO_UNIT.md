# KiCad 10 Howto — Unit PCB (Beginner)

**Revision:** 2026-04-27
**Audience:** You've finished `KICAD_HOWTO_BUS.md`. You know how to
place symbols, wire net labels, run ERC, place footprints, route
traces, draw zones, run DRC, and plot Gerbers.
**Time:** 6-10 hours start to Gerbers (largest jump in complexity
vs. Bus — many components, many nets, two-sided placement).
**Companion to:** `KICAD_GETTING_STARTED.md`, `SCHEMATIC_UNIT.md`
(the spec — every net, every part), `LAYOUT_UNIT.md` (placement +
routing rules).

The Unit board is **80 × 40 mm**, **64 of these per system** — every
mistake here is multiplied 64 times. Take your time on the schematic;
the PCB is mostly mechanical execution after that.

---

## What this board has

| Block | Components | Notes |
|---|---|---|
| MCU | U1 STM32G030K6T6 LQFP-32 | Pin map LOCKED — see SCHEMATIC_UNIT § MCU |
| Stepper driver | U2 TPL7407L SOIC-16 **narrow** | 4 channels drive 28BYJ-48 coils |
| LDO | U3 LDL1117S33TR SOT-223 | 12 V → 3.3 V; **VOUT is on the tab**, not GND |
| RS-485 PHY | U4 SN65HVD75 SOIC-8 | half-duplex with hardware DE |
| Reverse-block FET | Q1 AO3401A SOT-23 + Z1 BZT52C10 + R_q1g 100 Ω + R_q1g2 10 kΩ | P-FET high-side, **10 V Zener** clamp |
| Input TVS | D4 SMAJ15A SMA | post-Q1, cathode → +12V |
| RS-485 ESD | D5 SM712-02HTG SOT-23 | between U4 and pogo pins |
| LEDs | D1 (HB blue), D2 (FAULT red), D3 (ID yellow) — 0805 | **sink-driven** |
| Identify button | SW1 6×6 SMD tact | active-low with debounce cap |
| Pogo pins | PG1-PG4 THT, 1.83 mm drill, 2.45 mm pad | bottom layer, **no 5th PG_KEY** |
| Stepper output | J2 JST-XH 5-pin THT | **5-pin** (carries +12V to motor) |
| Hall input | J3 JST-XH 3-pin THT | KY-003 pinout: VCC/GND/SIG |
| Decoupling, pull-ups, etc. | C_in 22µF/1206, decaps 100nF/0603, R_hall/R_id/R_rst/R_led 1kΩ-10kΩ/0603 | see SCHEMATIC_UNIT § Component list |

Read **SCHEMATIC_UNIT.md** end-to-end **before** starting in KiCad.
Don't skim. The pin map and the LDO pinout have both been wrong in
prior drafts and would be silent killers if mis-built.

---

## Step 0 — Create the project

Same procedure as Bus. Save as `splitflap-unit-v2` under
`PCB/v2/kicad/unit/`.

Don't import the existing `kicad/unit/unit.net` — it's stale (wrong
USART pinout, wrong LDO). Start fresh.

---

## Step 1 — Schematic capture, by block

The cleanest way to draw this schematic is to lay it out in 5 blocks
on the page, then connect them with **net labels** rather than long
wires. This keeps the page readable and makes ERC errors easier to
locate.

Suggested page layout (A4 landscape):

```
+---------------+---------------+---------------+
|               |               |               |
|   POWER       |   MCU         |   RS-485      |
|   (Q1, Z1,    |   (U1         |   (U4 PHY,    |
|    LDO U3,    |    STM32G030, |    D5 ESD)    |
|    D4 TVS,    |    decaps,    |               |
|    Caps,      |    NRST net)  |               |
|    Pogos      |               |               |
|    PG1-PG4)   |               |               |
+---------------+---------------+---------------+
|               |               |               |
|   STEPPER     |   USER I/O    |   TEST PADS   |
|   (U2         |   (SW1, D1-3, |   (SWD pads,  |
|    TPL7407L,  |    LEDs +     |    TP1-5)     |
|    J2)        |    R_id/R_led)|               |
+---------------+---------------+---------------+
                                |               |
                                |   HALL J3     |
                                |               |
                                +---------------+
```

You don't have to follow this exactly — it's a hint to keep the
schematic legible. Eeschema's Hierarchical Sheets feature lets you
split this across multiple pages, but for a single board this size
one flat page is fine.

### Place all symbols (Step 1a)

For each component, press **`A`**, search the symbol, place it
roughly in its block area. Don't worry about precise layout —
schematic position has zero effect on the PCB.

Use these symbols (from `KICAD_HANDOFF.md` § 3 / `KICAD_GETTING_STARTED.md` § 4):

| Designator | Symbol library | Search |
|---|---|---|
| U1 (STM32G030) | `MCU_ST_STM32G0` | `STM32G030K6T` |
| U2 (TPL7407L) | `Driver_Motor` | `ULN2003A` (footprint-compatible — we'll change footprint later) |
| U3 (LDL1117S33) | `Regulator_Linear` | `LM1117-3.3` |
| U4 (SN65HVD75) | `Interface_UART` | `SN65HVD75` (or generic 8-pin RS-485) |
| Q1 (AO3401A) | `Transistor_FET` | `AO3401` |
| Z1 (BZT52C10) | `Diode` | `BZT52C` |
| D4 (SMAJ15A TVS) | `Diode_TVS` or `Device:D_TVS` | `SMAJ15A` |
| D5 (SM712 ESD) | (custom — draw 3-pin) | n/a; see step 1c |
| D1-D3 (LEDs) | `Device:LED` | `LED` |
| R_hall, R_id, R_rst (10k) | `Device:R` | `R` |
| R_led (×3, 1k) | `Device:R` | `R` |
| R_de (1k) | `Device:R` | `R` |
| R_q1g (100Ω) | `Device:R` | `R` |
| R_q1g2 (10k) | `Device:R` | `R` |
| C_in (22 µF) | `Device:C_Polarized` (or `C`) | `C` |
| C_in2, C_decap, C_rst, C_id (100nF) | `Device:C` | `C` |
| C_ldo_in, C_ldo_out (10 µF) | `Device:C` | `C` |
| SW1 (tact) | `Switch:SW_Push` | `SW_Push` |
| J2 (JST-XH 5p) | `Connector_JST:JST_XH_B5B-XH-A` | `JST_XH_B5B` |
| J3 (JST-XH 3p) | `Connector_JST:JST_XH_B3B-XH-A` | `JST_XH_B3B` |
| PG1-PG4 (pogo) | use `Connector_Generic:Conn_01x01` for the schematic — we'll attach a custom footprint in PCB | `Conn_01x01` |

### Set values + LCSC fields (Step 1b)

For every placed symbol:
1. Click → `E` → set the **Value** to match the BOM (e.g. `22u/25V`,
   `STM32G030K6T6`, `BZT52C10`, etc.).
2. Add a custom field **`LCSC`** with the LCSC code from
   `UNIT_BOM.csv`.

Faster bulk method: **Tools → Edit Symbol Fields Table**. Open the
table, paste-fill the `LCSC` column from `UNIT_BOM.csv`. Saves a lot
of clicks.

### Custom symbol for SM712-02HTG (D5) (Step 1c)

The SM712 is a 3-pin RS-485 ESD diode and isn't in KiCad's stock
libraries.

1. **File → Symbol Editor**.
2. **File → New Library** → name it `splitflap-custom`. Save in
   project (per-project) scope.
3. **File → New Symbol** → name `SM712-02HTG`.
4. Draw a rectangle for the body (e.g. 200 × 100 mil).
5. Add 3 pins:
   - Pin 1: name `IO1`, number `1`, electrical type `Bidirectional`.
   - Pin 2: name `GND`, number `2`, electrical type `Power input`.
   - Pin 3: name `IO2`, number `3`, electrical type `Bidirectional`.
6. Set **Reference**: `D`.
7. Set **Default footprint**: `Package_TO_SOT_SMD:SOT-23`.
8. Save.
9. Close Symbol Editor.

Back in Eeschema, press `A`, search `SM712`, your custom symbol
appears under `splitflap-custom`. Place it.

### Wire it up (Step 1d)

Use **net labels (`L`)** liberally. Names that match
`SCHEMATIC_UNIT.md` exactly:
- Power: `+12V_PG1` (incoming, pre-Q1), `PCB-12V` (post-Q1 load-side
  rail), `+3V3`, `GND`.
- USART: `UART_TX`, `UART_RX`, `DE`, `RS485_A`, `RS485_B`.
- Stepper: `STEPPER_IN1`, `STEPPER_IN2`, `STEPPER_IN3`, `STEPPER_IN4`.
- I/O: `HALL_IN`, `IDENTIFY_BTN`, `LED_HEARTBEAT`, `LED_FAULT`,
  `LED_IDENTIFY`, `NRST`.
- Misc: `Q1_GATE` (the node between R_q1g and R_q1g2 / Z1 anode).

Use **power symbols (`P`)** for `+3V3` and `GND` (KiCad has these
built-in — type `+3V3` and `GND` in the power-symbol picker). Don't
use a power symbol for `+12V` — there are 2 different 12V nets
(pre-Q1 and post-Q1). Treat them as labeled signal nets so you don't
accidentally short them via a generic `+12V` power symbol.

### Critical wiring details (don't get any of these wrong)

- **U1 (STM32G030) pin map**: PA9 (pin 19) = USART1_TX, **PC6 (pin
  20) = NC** (PA10 is NOT here — common mistake), PA10 (pin 21) =
  USART1_RX, PA11 (pin 22) = NC, PA12 (pin 23) = USART1_RTS_DE_CK.
  No SYSCFG remap.
- **U3 LDO pinout**: pin 1 = GND, pin 2 = VOUT (= tab), pin 3 = VIN.
  LM1117 family convention. **The tab (pin 2) is on the 3V3 net,
  NOT GND.**
- **Q1 reverse-block topology**: drain ← +12V_PG1 (incoming), source
  → PCB-12V (load), gate via R_q1g 100 Ω in series to the gate node;
  R_q1g2 10 kΩ from gate node to GND; Z1 cathode to source, anode to
  gate node. Don't pull the gate to +12V — that defeats the
  reverse-block.
- **D4 TVS (SMAJ15A)**: cathode (banded end) → PCB-12V (post-Q1
  load-side), anode → GND. Wrong polarity = forward-biased = dead
  short.
- **U4 SN65HVD75**: pin 2 (/RE) **tied to GND** (always-receive;
  firmware discards TX echo).
- **LEDs are sink-driven**: 3V3 → R_led 1 kΩ → LED anode → LED
  cathode → MCU GPIO. The MCU sinks current to illuminate. Do NOT
  wire the GPIO to the anode.
- **TPL7407L unused inputs (IN5, IN6, IN7)**: tie pins 5/6/7 to
  GND. Floating CMOS inputs are bad.

### Add PWR_FLAGs for ERC (Step 1e)

Add **PWR_FLAG** symbols on these nets so ERC doesn't complain about
"power input not driven":
- `+3V3` (driven by U3 LDO)
- `GND`
- `+12V_PG1` (driven by external bus via PG1)

### Run ERC

`Inspect → Electrical Rules Checker`. Fix all errors. Common ones:
- "Pin not connected" on MCU NC pins (PA0-PA3, PB3-PB8, PA11, PC6,
  PC14, PC15) → place **No-Connect flags** (`Q`) on each.
- "Power input pin not driven" on U1 pin 1 (VDD) and pin 17 (VDD)
  → make sure both are wired to the +3V3 net (not just one).
- "Pin not connected" on TPL7407L unused channels → wire IN5-IN7 to
  GND, OUT5-OUT7 should also be NC-flagged.

When ERC is clean, save and move to the PCB.

---

## Step 2 — Footprint assignment

`Tools → Footprint Assignment Tool` in Eeschema. For every symbol,
verify the footprint matches `SCHEMATIC_UNIT.md` and the relevant
table in `KICAD_GETTING_STARTED.md` § 4.

**Triple-check these three because they have wrong-default traps:**

1. **U2 TPL7407L** → `Package_SO:SOIC-16_3.9x9.9mm_P1.27mm` (NARROW
   150 mil). The default in KiCad's `Driver_Motor:ULN2003A` symbol
   may be the WIDE 7.5 mm body — change it. Wide pads will not bridge
   narrow IC leads, and you'll have 16 cold joints.
2. **U3 LDL1117S33** → `Package_TO_SOT_SMD:SOT-223-3_TabPin2`. The
   `_TabPin2` suffix means the heatsink tab is electrically pin 2
   (VOUT). KiCad has a `_TabPin3` variant for some LDOs — wrong for
   this part.
3. **C_in 22 µF** → `Capacitor_SMD:C_1206_3216Metric` (NOT 0805).
   22 µF / 25 V / X7R / 0805 doesn't exist in JLC stock. 1206 is the
   smallest viable package.

Save, close Footprint Assignment Tool, save Eeschema.

---

## Step 3 — Open Pcbnew, set Board Setup

Same procedure as Bus. Use the values in
`KICAD_GETTING_STARTED.md` § 8.

---

## Step 4 — Pull components in (Update PCB from Schematic)

`F8` → Update PCB. Components dump in a cluster. Don't try to place
them yet; first draw the outline and mounting holes.

---

## Step 5 — Board outline + mounting holes

### Outline (80 × 40 mm)

1. Layer: `Edge.Cuts`. Grid: 1 mm.
2. Use `Place → Add Rectangle`. Click corner `(0, 0)` → corner
   `(80, 40)`. Done.

### Mounting holes (4× M3 NPTH at v1 corner positions)

`Place → Add Footprint`. Search `MountingHole_3.2mm_M3`. Place at:
- (3, 3)
- (77, 3)
- (3, 37)
- (77, 37)

These match the v1 chassis bolt pattern AND are where the 3D-printed
DIN clip bolts onto the back. **Do not change.**

---

## Step 6 — Custom pogo-pin footprint

Same as you did for the Bus board (if you walked through the optional
custom-footprint creation). If you haven't yet:

1. **File → Footprint Editor**.
2. **File → New Library** → `splitflap-custom`. Project scope.
3. **File → New Footprint** → `Pogo_MillMax_0906-2_THT`.
4. **Place → Add Pad**:
   - Pad number: `1`
   - Pad shape: Circular
   - Pad type: Through-hole
   - Drill: 1.83 mm
   - Pad size: 2.45 mm × 2.45 mm
   - Layers: F.Cu, B.Cu, F.Mask, B.Mask
5. Place the pad at the footprint origin.
6. Save.

Back in Pcbnew, you'll need to assign this custom footprint to PG1-
PG4 via Footprint Assignment Tool (Eeschema side) or by directly
editing the PCB footprint properties.

---

## Step 7 — Place pogo pins (the floorplan anchor)

The 4 pogo pins are the **mechanical reference** for the back
floorplan. Place them first.

All on the **bottom layer (B.Cu)**. They're THT but we want the
"contact side" to be the bottom (so they project downward toward the
bus PCB).

1. Select PG1, press `M`, position to **(40, 8)** mm. (Long-axis
   center = x = 40.)
2. Press `F` to flip to bottom layer (the footprint becomes mirrored
   on B.Cu).
3. Repeat for PG2 at **(40, 16)**, PG3 at **(40, 24)**, PG4 at
   **(40, 32)**.

Vertical pogo column at x = 40, y spacing 8 mm. Matches the bus PCB
trace pitch exactly.

---

## Step 8 — Place top-layer (front) components

Per `LAYOUT_UNIT.md` § Front (top) layer:

- **J2** (5-pin JST-XH stepper output) — top-left short edge, around
  (5, 8) — match v1's "28BYJ-48 Stepper" XY.
- **J3** (3-pin JST-XH hall) — just below J2, around (5, 18) — match
  v1's "Magnet Sensor" XY.
- **SW1** IDENTIFY button — somewhere on the long edge, accessible
  after install. (5, 30) is fine.
- **D1, D2, D3** status LEDs — visible after install, near SW1.
- **TP_SWD pads + TP1-5 pads** — along an edge for probe access.

Use `M` to move, `R` to rotate. After placement, switch to
**View → 3D Viewer** to verify J2/J3 face the right direction for
their cables.

---

## Step 9 — Place bottom-layer (back) components

Per `LAYOUT_UNIT.md` § Back (bottom) layer. Press `F` after each
placement to flip the component onto B.Cu.

Place these in roughly this order so wiring is clean:

1. **U1** STM32G030 LQFP-32 — central on the back, near the pogo
   column. Try (50, 20).
2. **U4** SN65HVD75 SOIC-8 — between U1 and the pogo column, so the
   RS-485 A/B traces from PG2/PG3 stay short (< 30 mm).
3. **D5** SM712 SOT-23 — adjacent to U4, on the A/B nets (between
   U4 and the pogos).
4. **U2** TPL7407L SOIC-16 narrow — near v1's ULN2003A position
   (top-right area of the back, around (60, 12)).
5. **U3** LDL1117S33 SOT-223 — near v1's AMS1117 position (around
   (15, 30)). Leave room for a ~1 cm² VOUT pour next to it.
6. **Q1, Z1, R_q1g, R_q1g2** — group near U3's input (the post-Q1
   PCB-12V rail).
7. **D4** SMAJ15A SMA — on the PCB-12V rail (post-Q1), near the
   junction where Q1 source feeds C_in / C_in2 / U3 / J2 pin 5.
   **Mark the cathode side with a silk arrow** so the assembler
   doesn't fit it backwards.
8. **C_in** (22 µF / 1206) and **C_in2** (100 nF / 0603) — on the
   PCB-12V rail, near U3 input.
9. **C_ldo_in** and **C_ldo_out** — within 3 mm of U3 pins 3 (VIN)
   and 2 (VOUT) respectively.
10. **C_decap** (×4 100 nF) — one within 3 mm of each VDD/Vcc pin
    (U1 pin 1, U1 pin 17, U4 pin 8, plus one spare you can place
    near the MCU).
11. **C_rst, R_rst** on the NRST net near U1 pin 4.
12. **C_id, R_id** on the IDENTIFY_BTN net near U1 pin 18 (and SW1).
13. **R_hall** on the HALL_IN net near U1 pin 13 (and J3 pin 3).
14. **R_de** on the DE net between U1 pin 23 and U4 pin 3.
15. **R_led (×3)** between 3V3 and each LED anode.

Don't aim for perfection on first pass. Place, route, see what's
ugly, move things.

---

## Step 10 — Routing

Routing rules summary (full details in `LAYOUT_UNIT.md` § Power
routing / § Signal routing):

| Net group | Width | Notes |
|---|---|---|
| 12V (PCB-12V rail post-Q1) | 15-20 mil (0.4-0.5 mm) | branches to D4, C_in, C_in2, U2 pin 9, J2 pin 5, U3 pin 3 |
| 12V (pre-Q1, PG1 → Q1 drain) | 15-20 mil | very short — stay on bottom layer |
| 3V3 | 15 mil (0.4 mm) | from U3 pin 2 + tab to U1 VDD pins, U4 pin 8, all 3V3 pull-ups |
| GND | poured zone both layers | stitched ~10 mm |
| Stepper coil drives (U2 OUT → J2) | 15 mil | peak ~250 mA per coil |
| MCU stepper inputs (PA4-7 → U2 IN) | 6 mil | signal-level |
| RS-485 A/B (PG2/3 ↔ U4 ↔ D5) | 8 mil, loose differential pair | KiCad 10: hotkey `6` for diff-pair routing |
| USART1 (MCU ↔ U4 R/D/DE) | 6 mil | normal signal |
| Hall, identify, NRST, LEDs | 6 mil | normal signal |

### Important routing constraints
- **Don't run 12V or stepper coil traces under U4 SN65HVD75** —
  switching noise from coil drives can couple into the receiver.
- **Don't run anything under the MCU's crystal area** if you decide
  to add an external crystal later. (Current design uses the
  internal HSI oscillator — no crystal — so this is moot. Note for
  future revs.)
- **Z1 Zener and R_q1g network** must be physically close to Q1's
  gate pin to keep the gate node low-impedance against transients.

### Order of routing operations

1. Route all power: 12V then 3V3. Use thick traces; pour copper
   liberally on the bottom layer for the 3V3 distribution if you
   like.
2. Route stepper coil drives U2 OUT → J2 pins 1-4.
3. Route MCU USART1 → U4 R/D/DE.
4. Route MCU GPIO → U2 IN1-4.
5. Route RS-485 A/B as a coupled pair.
6. Route Hall, IDENTIFY, NRST, LED nets.
7. **Add filled zones** for GND on both layers.
8. **Add filled zone** for 3V3 on the bottom layer near U3, attached
   to U3 pin 2 + tab. Aim for ~1 cm² for heatsinking.
9. Press `B` to fill all zones.

---

## Step 11 — Add silkscreen labels

Per `LAYOUT_UNIT.md` § Silk screen:

- Designator for every component (KiCad places these automatically;
  move them to readable positions).
- Polarity arrow on D4 SMAJ15A (cathode = banded → +12V).
- Pin labels near pogo holes (back side): "PG1 12V", "PG2 A",
  "PG3 B", "PG4 GND".
- "IDENTIFY" near SW1.
- LED labels: "HB", "FAULT", "ID".
- Test pad labels: "GND", "3V3", "RX", "TX", "NRST", "SWDIO", "SWCLK".
- "v2 / 2026-04-27 / SplitFlap Unit" somewhere — bottom edge corner.
- "TOP" / "BOTTOM" markers near a corner so orientation is
  unambiguous.

---

## Step 12 — DRC + 3D viewer

1. Press `B` (fill zones).
2. **Inspect → Design Rules Checker**. Fix all errors.
3. **View → 3D Viewer**. Look for:
   - All connectors on correct edges, facing correct direction for
     cable exits.
   - Pogo pins as small cylinders projecting from the bottom.
   - LDO U3 with its tab on the 3V3 pour (verify the tab geometry —
     SOT-223's middle pin 2 should look continuous with the tab).
   - Q1, D4, D5 oriented correctly.
4. Save.

---

## Step 13 — Plot Gerbers

`KICAD_GETTING_STARTED.md` § 9. **Include F.Paste and B.Paste** if
you're doing JLC PCBA (the unit board is mostly SMT — JLC-assembled
makes sense for 70 boards).

---

## Step 14 — JLC order

| Spec | Value |
|---|---|
| Quantity | 70 (covers 64 + 6 spares) |
| Layers | 2 |
| Material | FR-4 |
| Thickness | 1.6 mm |
| Surface finish | **HASL** is fine — pogo pins are on the **bus PCB** (which is ENIG), not the unit board |
| Copper weight | 1 oz both sides |
| Estimated cost | ~EUR 2-3 per board at qty 70 (~EUR 150-200 total) |

Plus PCBA cost if you have JLC populate parts (~EUR 4-8 per board for
this complexity at qty 70).

---

## Verification before clicking "Place Order"

Open `LAYOUT_UNIT.md` § "Verification before Gerber" — run through
every checkbox. The killers (multiplied 64×):

- [ ] **U2 TPL7407L is SOIC-16 narrow (150 mil)**, NOT SOIC-16W.
- [ ] **U3 LDL1117S33 tab is on 3V3 net**, NOT GND. Verify by
      clicking the tab in Pcbnew → properties → net should be `+3V3`.
- [ ] **U3 pinout: pin 1 = GND, pin 2 = VOUT (= tab), pin 3 = VIN.**
- [ ] **Q1 gate-source clamp is BZT52C10 (10V)**, NOT BZT52C12.
- [ ] **D4 SMAJ15A** is post-Q1 (load-side) with cathode → +12V.
- [ ] **C_in is 1206 package**, NOT 0805.
- [ ] **No 5th PG_KEY pogo pin anywhere.**
- [ ] **U1 pin map**: pin 19 = PA9 (TX), pin 20 = PC6 (NC),
      pin 21 = PA10 (RX), pin 22 = PA11 (NC), pin 23 = PA12 (DE).
- [ ] **U4 pin 2 (/RE) tied to GND.**
- [ ] **All LEDs sink-driven** (GPIO → cathode, anode → R_led →
      3V3).
- [ ] **J2 is JST-XH 5-pin**, pin 5 wired to PCB-12V (post-Q1).
- [ ] **J3 is JST-XH 3-pin**, pin 1 = +3V3, pin 2 = GND, pin 3 =
      HALL_OUT.
- [ ] All 4 corner mounting holes at v1 positions (3,3) (77,3)
      (3,37) (77,37) — chassis drop-in.
- [ ] DIN clip mounting bolt heads on the back don't interfere
      with pogo pins or any back-layer components (visual check
      in 3D viewer; the clip is bolted from the back through M3
      holes).
- [ ] DRC clean.

---

## What's harder than the Bus board

- **More nets** (~30 vs. 4) — schematic is busier.
- **Two-sided placement** — most components on bottom, connectors
  and user I/O on top. You'll be flipping (`F`) often.
- **A custom pogo footprint and a custom SM712 symbol** — you've now
  drawn at least one custom symbol and one custom footprint, so you
  know how.
- **The LDO tab gotcha** — the most common silent failure on this
  board. Verify multiple times: the tab IS pin 2, pin 2 IS VOUT, and
  the **3V3 net** is what touches the tab.

If you got through this, the Master is structurally similar — just
more components and a much heftier power input. Open
`KICAD_HOWTO_MASTER.md`.
