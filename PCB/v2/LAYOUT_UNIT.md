# Layout Guide — Unit PCB

**Revision:** 2026-04-26
**Companion to:** `SCHEMATIC_UNIT.md` (nets, MPNs, pin maps).
**Read this when:** placing components and routing in KiCad 10. See
`KICAD_HANDOFF.md` for the tool-specific workflow (project setup,
libraries, plot/Gerber, JLC integration).

64 of these per system. **Make every decision once, get every unit
right.** A bug here is replicated 64 times.

---

## Board outline

| Spec | Value |
|---|---|
| Outline | **80 × 40 mm** rectangle (matches v1 chassis exactly — drop-in) |
| Layers | 2 |
| Substrate | FR-4 |
| Thickness | 1.6 mm |
| Copper | 1 oz both sides |
| Surface finish | HASL — pogo pins are on the bus PCB side, not the unit side, so unit doesn't need ENIG |
| Solder mask | Green |
| Silk screen | White |

**Origin: top-left corner of the 80 × 40 mm outline.** Y axis points down.

## Mounting holes (LOCKED, 4 holes)

Extracted from `PCB/v1/Gerber_PCB_Splitflap.zip` →
`Gerber_Drill_NPTH.DRL`. **Do not change** — these define the
chassis interface AND the bolt pattern that the 3D-printed DIN clip
mounts to.

| Hole | Position (mm) |
|---|---|
| Top-left | (3, 3) |
| Top-right | (37, 3) |
| Bottom-left | (3, 77) |
| Bottom-right | (37, 77) |

Wait — verify the orientation: v1 board is **80 mm long × 40 mm wide**.
The hole pattern from the Gerber drill file is referenced to the v1
80 × 40 outline; v2 keeps the same outline. Let the KiCad outline
be 80 (long axis = X) × 40 (short axis = Y).

→ Holes are at (3, 3), (3, 37), (77, 3), (77, 37) if X is long axis.

**Pick one convention and stick with it.** The schematic uses the
"long axis = 80 mm = X axis" convention with hole (3, 3) at one
corner and (77, 37) at the diagonal opposite. Match that.

Drill 3.2 mm (M3 clearance). NPTH.

## Pogo pins (4 contacts on bottom layer, NO 5th pin)

Through-hole, projecting downward to contact the bus PCB. Pad/drill:

| Drill | 1.83 mm |
|---|---|
| Pad | 2.45 mm |

Position dead-centre on the **long axis** (x = 40 mm) on the bottom
layer, vertical line along the short axis:

| Pin | (x, y) | Net |
|---|---|---|
| PG1 | (40, 8)  | 12V |
| PG2 | (40, 16) | RS485_A |
| PG3 | (40, 24) | RS485_B |
| PG4 | (40, 32) | GND |

8 mm spacing matches bus PCB trace pitch.

**Do NOT add a 5th pin (PG_KEY) — withdrawn 2026-04-24.** Polarization
is enforced by the asymmetric DIN clip on the back, NOT by a pogo pin.

## DIN clip mount (on back, mechanical only)

3D-printed asymmetric clip (MakerWorld STL, user-supplied). Bolts
through the 4× M3 corner holes from the back side. Asymmetric profile
means the unit physically only seats one orientation on the rail.

The clip is the **only** polarization mechanism. There is no
electronic polarization fallback.

## Component placement zones

```
                          80 mm long axis (X)
   ─────────────────────────────────────────────────────────
  ┌──○────────────────────────────────────────────────○──┐
  │                                                       │
  │ ┌─────┐                                               │
  │ │ J2  │     LDO U3 (LDL1117S33)                       │
  │ │ 5p  │     [VOUT-tab pour ~1cm²]                     │
  │ │ XH  │                            FRONT (top) layer  │  40 mm
  │ └─────┘                                               │  short
  │ ┌─────┐                                               │  axis
  │ │ J3  │     STM32G030K6T6 LQFP-32                     │  (Y)
  │ │ 3p  │                                               │
  │ │ XH  │                                               │
  │ └─────┘                                               │
  │                                                       │
  │ SW1                                  TP_SWD pads      │
  │ D1 D2 D3                            (SWDIO/SWCLK/RST/GND) │
  ├──○────────────────────────────────────────────────○──┤

         ─────────  BACK (bottom) layer  ─────────
  ┌──○────────────────────────────────────────────────○──┐
  │                                                       │
  │   U2 TPL7407L SOIC-16 narrow                          │
  │                                                       │
  │   U4 SN65HVD75              ●  PG1 (40,  8)   12V     │
  │   D5 SM712-02HTG ESD        ●  PG2 (40, 16)   A       │
  │                              ●  PG3 (40, 24)   B       │
  │   Q1 AO3401A                ●  PG4 (40, 32)   GND     │
  │   Z1 BZT52C10               ↑                          │
  │   D4 SMAJ15A           pogo column at x = 40 mm        │
  │   C_in 22µF, etc.        (long-axis centreline)        │
  │                                                       │
  ├──○────────────────────────────────────────────────○──┤

  ○ = M3 mounting hole (4 corners). DIN clip bolts through these.
```

### Front (top) layer — connectors + user controls

- **J2** stepper output (5-pin JST-XH, B5B-XH-A): top-left short edge.
  Match v1 "28BYJ-48 Stepper" header XY so the chassis cable run
  doesn't change. Pins 1–4 = motor coils, **pin 5 = +12V common**.
- **J3** hall sensor (3-pin JST-XH, B3B-XH-A): just below J2, same
  XY as v1 "Magnet Sensor" header. Pins 1=+3V3 / 2=GND / 3=HALL_OUT.
- **SW1** IDENTIFY tact switch (6×6 SMD): on the long-edge of the
  board so it's accessible after install.
- **D1 / D2 / D3** status LEDs (0805): visible after install.
- **TP_SWD** SWD test pads (SWDIO, SWCLK, NRST, GND) and **TP1–TP5**
  (GND, 3V3, RX, TX, NRST): along an edge for probe access.

### Back (bottom) layer — power + ICs + pogo pins

- **PG1–PG4** pogo pins on the **long-axis centreline (x = 40 mm)**.
  Vertical line, 8 mm spacing.
- **U1** STM32G030K6T6 LQFP-32: roughly central, near the SN65HVD75.
- **U4** SN65HVD75 SOIC-8: near the pogo pin column to keep RS-485
  A/B traces short.
- **D5** SM712 ESD (SOT-23): adjacent to U4, on the A/B nets.
- **U2** TPL7407L SOIC-16 **narrow (150 mil body)**: near v1's
  ULN2003A position on the back. Verify the footprint is **narrow**
  (3.9 mm body) NOT wide (7.5 mm body) — pads will not bridge wide
  IC leads.
- **U3** LDL1117S33 SOT-223: near v1's AMS1117 position. **VOUT tab**
  (pin 2 + tab, internally tied) gets a ~1 cm² copper pour on the
  bottom layer for heatsinking. **The tab is on the 3V3 net, NOT
  GND** — pouring tab to GND shorts 3V3 to GND through the
  internally-tied tab/pin-2 node.
- **Q1** AO3401A (SOT-23) + **Z1** BZT52C10 (SOD-123) + **R_q1g**
  100 Ω + **R_q1g2** 10 kΩ: group these together near the LDO input
  on the post-Q1 PCB-12V rail.
- **D4** SMAJ15A (DO-214AC SMA): **post-Q1, on the load-side
  PCB-12V rail**. Cathode (banded) to PCB-12V, anode to GND. Mark
  polarity with silk arrow.
- **C_in** 22 µF / 25 V (1206 X7R) + **C_in2** 100 nF (0603):
  post-Q1 source, near the LDO input. **1206 (NOT 0805)** because
  22 µF/25 V/X7R/0805 is unobtainable on JLC.
- **C_ldo_in** 10 µF + **C_ldo_out** 10 µF: within 3 mm of LDO
  pins 3 (VIN) and 2 (VOUT) respectively.
- **C_decap (×4)** 100 nF (0603): one per VDD pin (MCU has 2 — pin 1
  and pin 17), one for SN65HVD75 pin 8, one spare. Place within
  3 mm of each VDD/Vcc pin.
- **C_rst** 100 nF + 10 kΩ pull-up on NRST near the MCU pin 4.
- **C_id** 100 nF + 10 kΩ pull-up on IDENTIFY button.

## Component placement order (do this first)

1. Place 4 corner mounting holes — they fix the outline.
2. Place 4 pogo pins on bottom layer at long-axis centreline. They
   anchor the bottom-layer floorplan.
3. Place J2 (top-left long edge) and J3 (just below J2) on top
   layer — these have hard mechanical constraints from v1 chassis.
4. Place SW1 IDENTIFY button on accessible edge.
5. Place U1 STM32G030 centrally on the back, near the pogo column.
6. Place U4 SN65HVD75 near U1 and near the pogo pin column.
7. Place U2 TPL7407L on the back, near v1's ULN2003 spot.
8. Place U3 LDL1117S33 on the back, give it a ~1 cm² VOUT pour.
9. Group Q1/Z1/R_q1g/R_q1g2/D4/C_in/C_in2 around the LDO input.
10. Place SWD test pads on the front edge.
11. Place LEDs on the front edge near silk labels.
12. Distribute decoupling caps within 3 mm of every VDD/Vcc pin.

## Power routing

### 12V net (PCB-12V rail = post-Q1 source)

- Trace width: **15–20 mil (0.4–0.5 mm)** internal — peak motor coil
  current is ~250 mA, average much lower. Comfortable margin.
- From PG1 → Q1 drain through a wide pour or trace.
- From Q1 source → load: branch to D4 cathode, C_in, C_in2,
  TPL7407L pin 9 (COM), J2 pin 5, U3 LDO pin 3 (VIN).
- Don't run the 12V net under the SN65HVD75 transceiver or the
  RS-485 A/B traces.

### 3V3 net

- Trace width: **15 mil (0.4 mm)** — peak ~55 mA load.
- From U3 LDO pin 2 (VOUT) + tab → distribute to U1 VDD pins (1, 17),
  U4 SN65HVD75 pin 8, R_hall pull-up, R_id pull-up, R_rst pull-up,
  LED anode side.
- The VOUT pour serves as both heatsink AND distribution — keep it
  wide and use it as the 3V3 reference.

### GND net

- Solid GND pour both layers, stitched with vias every ~10 mm.
- Single GND star-point at the LDO pin 1 / Q1 source bypass area to
  minimize ground bounce on the MCU.

## Signal routing

### RS-485 A/B (PG2 ↔ U4 pin 6, PG3 ↔ U4 pin 7)

- **Differential pair**, ~8 mil traces, 8 mil spacing (loose pair
  fine for 250 kbaud).
- Keep U4 close to the pogo column so A/B traces are < 30 mm.
- Insert SM712-02HTG (D5) on the A/B nets between U4 pins 6/7 and
  the pogo pins. SM712 absorbs ESD before it reaches the transceiver.

### USART1 (MCU ↔ U4)

- PA9 (TX) → U4 pin 4 (D), PA10 (RX) ← U4 pin 1 (R), PA12 (DE) →
  U4 pin 3 (DE) via 1 kΩ R_de. **U4 pin 2 (/RE) tied to GND**
  (always-receive; firmware discards TX echo).
- 6 mil signal traces, no special routing needed.

### Stepper drive (MCU PA4–PA7 → TPL7407L IN1–IN4 → J2 pins 1–4)

- 6 mil signal traces from MCU to U2 inputs.
- **15 mil (0.4 mm) traces from U2 outputs to J2** — coil current is
  the highest signal current on the board (~250 mA peak per coil).
- TPL7407L OUT1–OUT4 are pins 16, 15, 14, 13 — match J2 pin 1, 2, 3, 4.

### Hall sensor (J3 pin 3 → MCU PB0)

- 6 mil trace; 10 kΩ R_hall pull-up to 3V3 close to the MCU pin.

### IDENTIFY button (SW1 → MCU PA8)

- 6 mil trace; 10 kΩ R_id pull-up to 3V3 + 100 nF C_id debounce
  cap, both close to the MCU pin.

### NRST (PF2)

- 10 kΩ pull-up to 3V3 + 100 nF cap to GND on the NRST net, close
  to the MCU.
- Expose NRST on a SWD test pad.

## Layer assignments summary

| Layer | What goes here |
|---|---|
| Top (front) | Connectors (J2/J3), IDENTIFY button (SW1), status LEDs, SWD test pads, general-purpose test pads, silk labels |
| Bottom (back) | All ICs (U1–U4), Q1+Z1+gate-network, D4 TVS, D5 ESD, all caps and resistors, pogo pins (THT), LDO heatsink VOUT pour, GND pour return |

## Silk screen

- Refdes for every component.
- Polarity arrow on D4 SMAJ15A (cathode = banded end → +12V).
- "PG1 12V", "PG2 A", "PG3 B", "PG4 GND" near pogo holes (back side).
- "IDENTIFY" near SW1.
- LED labels: "HB", "FAULT", "ID".
- Test-pad labels: "GND", "3V3", "RX", "TX", "NRST", "SWDIO", "SWCLK".
- "v2" + revision date + STM32G030 unit board identifier.
- "TOP" / "BOTTOM" markers near a corner so the unit's orientation
  is unambiguous when held in hand.

## DRC settings

| Rule | Value |
|---|---|
| Min trace width | 5 mil (0.127 mm) — JLC default |
| Min clearance | 5 mil |
| Min drill | 0.3 mm |
| Min annular ring | 0.13 mm |
| Edge clearance | 0.5 mm to copper |

## Verification before Gerber

- [ ] Outline exactly 80 × 40 mm.
- [ ] 4 mounting holes at v1 corner positions, 3.2 mm drill, NPTH.
- [ ] 4 pogo pins on **bottom layer** at long-axis centreline,
      8 mm pitch, **NO 5th PG_KEY pin**.
- [ ] J2 is **5-pin** JST-XH (B5B-XH-A, C158013), pin 5 wired to
      PCB-12V rail (post-Q1).
- [ ] J3 is 3-pin JST-XH (B3B-XH-A, C145756), pin 1 = +3V3, pin 2 =
      GND, pin 3 = HALL_OUT (KY-003 native).
- [ ] U2 TPL7407L footprint is SOIC-16 **narrow (150 mil)**, NOT
      SOIC-16W (300 mil).
- [ ] U3 LDL1117S33 SOT-223 **VOUT (pin 2 + tab) is on the 3V3 net**,
      with a ~1 cm² copper pour. Tab is NOT GND.
- [ ] Pin 1 of U3 = GND, pin 2 = VOUT (= tab), pin 3 = VIN.
      LM1117 family convention.
- [ ] Q1 AO3401A: drain at PG1/12V_input, source at PCB-12V_RAIL,
      gate via R_q1g 100 Ω + R_q1g2 10 kΩ pull-down.
- [ ] **Z1 is BZT52C10 (10 V), NOT BZT52C12 (12 V)**.
      Cathode → source (PCB-12V), anode → gate node.
- [ ] D4 SMAJ15A: **post-Q1 (load-side PCB-12V)**, cathode banded →
      +12V, anode → GND.
- [ ] C_in is **1206** package (NOT 0805; 0805 22 µF/25 V/X7R is
      unobtainable on JLC).
- [ ] All 4 SN65HVD75 / MCU / TPL7407L VDD pins have a 100 nF decap
      within 3 mm.
- [ ] NRST (PF2) has 10 kΩ pull-up + 100 nF cap.
- [ ] SN65HVD75 /RE (pin 2) tied permanently to GND.
- [ ] LEDs are sink-driven: 3V3 → R_led 1 kΩ → anode → cathode →
      MCU GPIO. (Not source-driven.)
- [ ] DIN clip mounting holes don't interfere with pogo pin
      placement on the back.
- [ ] DRC passes with no errors.

## Fab order

| Spec | Value |
|---|---|
| Quantity | 70 (covers 64 units + spares) |
| Surface finish | HASL |
| Estimated cost | ~EUR 2–3 per board at qty 70 |
