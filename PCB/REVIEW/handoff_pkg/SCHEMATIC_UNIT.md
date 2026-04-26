# Unit PCB — Schematic + Layout Specification

**Revision:** 2026-04-26

EasyEDA-ready spec for the unit PCB. **64 of these per system; one
PCB design.**

## Component list (with LCSC starting points)

| Ref | Value | Footprint | LCSC# (CHECK) | Notes |
|---|---|---|---|---|
| U1 | STM32G030K6T6 | LQFP-32 | C2040675 | MCU; required for silicon UID |
| U2 | TPL7407L | **SOIC-16 (narrow, 150 mil body)** | C383290 | Stepper driver primary; ULN2003A drop-in alt: C2358. **Both parts ship in standard narrow SOIC-16 (150 mil body / 3.9 mm). Do NOT use SOIC-16W (300 mil / 7.5 mm) — pads will not bridge the IC's leads.** |
| U3 | LDL1117S33TR (or LM2937IMP-3.3) | SOT-223 | C434348 (CHECK) | **12V→3.3V LDO**, 40 V max VIN, 1.2 A. **Replaces HT7833** — HT7833 is rated 6.5 V max VIN and would be destroyed at 12 V. Dissipation at 55 mA load = 0.48 W; SOT-223 with copper pour heatsink handles this comfortably (θJA ~50 °C/W with 1 cm² pad). Alternatives: LM2937IMP-3.3 (TI, 26 V max, SOT-223) or AP1117-33 (40 V max). **Do NOT substitute HT7833 / AMS1117 / MCP1825 — wrong VIN ratings.** |
| U4 | SN65HVD75DR | SOIC-8 | C57928 | RS-485 transceiver |
| J3 | Hall connector | JST-XH 3-pin male, vertical THT (B3B-XH-A) | C145756 | 3-pin header for external hall sensor module on flying lead. **Pinout LOCKED: pin 1 = +3V3, pin 2 = GND, pin 3 = HALL_OUT** — matches KY-003 module native order so v1's existing flying-lead cable plugs straight in. Different sensor modules adapt at the cable end. |
| Q1 | AO3401A | SOT-23 | C15127 | P-FET reverse-block. **VGS rated only ±12 V** — at nominal 12 V brick the gate-source margin is zero. Add Z1 12 V Zener clamp gate→source to absorb brick tolerance + transients; see Power section. |
| Z1 | BZT52C12 (or MMSZ5242B) | SOD-123 | C8061 (CHECK) | 12 V Zener clamp on Q1 VGS — cathode → source, anode → gate. Required to keep AO3401A within VGS ratings under brick tolerance and inrush transients. |
| D1 | LED blue | 0805 | C2293 | HEARTBEAT |
| D2 | LED red | 0805 | C2286 | FAULT |
| D3 | LED yellow | 0805 | C2298 | IDENTIFY |
| D4 | SMAJ15A | DO-214AC SMA | C167238 | 12V TVS unidirectional. **Polarity: cathode (banded end) → +12V (post-pogo, before Q1), anode → GND.** Wrong-way orientation forward-biases the diode and shorts the rail. |
| D5 | SM712-02HTG | SOT-23 | C172881 | RS-485 ESD |
| SW1 | Tact switch 6×6 | SMD 6x6x5 | C318884 | IDENTIFY button |
| PG1-PG4 | Pogo pin spring contact | THT (drill 1.83 mm, pad 2.45 mm) | n/a (DigiKey) | **Primary: Mill-Max 0906-2-15-20-75-14-11-0** (5.00 mm free, 1.0–1.4 mm travel, 1.07 mm Au tip, 2 A continuous, 1 M cycles). **Backup: Mill-Max 0906-1-15-20-75-14-11-0** (same footprint, less travel margin). LCSC carries Mill-Max as a courtesy listing — order from DigiKey and ship to JLC for assembly, or hand-solder. **LCSC-only fallback: Xinyangze YZ02015095R-01 (LCSC C5157439)** — derate to 1 A (parallel two pins on the 12 V rail), 10k mate cycles is fine for hobby. **NOTE: PG_KEY 5th polarization pogo pin REMOVED** — pad spacing collides with PG1, and a spring pogo on bare FR-4 is not a hard mechanical interlock. Polarization is now enforced by an asymmetric 3D-printed DIN clip (see Mechanical section + DIN clip note). |
| J2 | JST-XH **5-pin** male | THT B5B-XH-A | C158013 | Stepper output to 28BYJ-48: pins 1-4 = coil drives from TPL7407L OUT1-OUT4, **pin 5 = +12 V** (carries the 28BYJ-48's red wire from the unit's internal 12 V rail). Avoids 64× hand-splicing the +12 V wire into the chassis harness. |
| C_in | 22 µF / 25 V X7R | **1206** | C45783 (CHECK: confirm 1206 22 µF/25 V X7R is in JLC stock; 22 µF/25 V/X7R/0805 is effectively unobtainable) | 12V input bulk |
| C_in2 | 100 nF X7R | 0603 | C14663 | 12V decap |
| C_ldo_in | 10 µF / 25 V X7R | 0805 | C19702 | LDO input |
| C_ldo_out | 10 µF / 10 V X7R | 0805 | C15850 | LDO output |
| C_decap (×4) | 100 nF X7R | 0603 | C14663 | MCU + transceiver decoupling |
| C_rst | 100 nF X7R | 0603 | C14663 | NRST filter |
| C_id | 100 nF X7R | 0603 | C14663 | IDENTIFY button debounce |
| R_hall | 10 kΩ 1% | 0603 | C25804 | Hall sensor pull-up |
| R_de | 1 kΩ 1% | 0603 | C21190 | DE driver line series (PA12 → SN65HVD75 DE) |
| R_led (×3) | 1 kΩ 1% | 0603 | C21190 | LED current limit |
| R_id | 10 kΩ 1% | 0603 | C25804 | IDENTIFY button pull-up |
| R_rst | 10 kΩ 1% | 0603 | C25804 | NRST pull-up |

## Net-by-net wiring

### Power section

```
PG1 (top pogo, 12V_IN) ──── Q1 drain (P-FET, high-side reverse-block)
                              │
                              Q1 source ── PCB-12V rail ──┬── D4 SMAJ15A cathode (banded → +12V; anode → GND)
                                                           ├── C_in 22 µF
                                                           ├── C_in2 100 nF
                                                           ├── U2 TPL7407L pin 9 (COM)
                                                           ├── J2 pin 5 (28BYJ-48 +12V common)
                                                           └── U3 LDL1117S33 input (pin 1 VIN)

Q1 AO3401A (P-FET high-side reverse-block, standard topology):
  drain  ← PG1 (incoming 12V)
  source → PCB-12V rail (load side)
  gate   ── R_q1g 100 Ω series ── (gate node) ── R_q1g2 10 kΩ to GND
  Z1     12 V Zener: cathode → source (PCB-12V), anode → gate node
                     (clamps |VGS| at ~12 V to keep AO3401A inside ±12 V rating)

  Same topology + behaviour as the master Q1 — see SCHEMATIC_MASTER.md
  power section for the full rationale. Do NOT pull the gate to +12V;
  that would defeat the reverse-block. The series 100 Ω + Zener clamp
  protects against brick over-tolerance and inrush transients (without
  the Zener, AO3401A operates with zero VGS margin at 12 V nominal).

D4 SMAJ15A polarity (CRITICAL):
   cathode (banded end) → +12V_RAIL
   anode                → GND
  Reversed orientation forward-biases the diode and shorts the rail.
  Mark on silkscreen with a polarity arrow + verify on first article.

U3 LDL1117S33TR (40 V max VIN, 1.2 A SOT-223):
   pin 1 (TAB/GND)     → GND (large copper pour for heatsinking)
   pin 2 (VIN)         ← +12V
   pin 3 (VOUT)        → 3V3 + C_ldo_out 10 µF + per-IC decoupling
  Thermal: 0.48 W dissipation at 55 mA load; with ~1 cm² copper pad
  on the GND tab (θJA ~50 °C/W) the junction sits ~24 °C above
  ambient — well within the 125 °C Tj limit. **NOT HT7833** (HT7833
  is 6.5 V max VIN — would be destroyed at 12 V).

PG4 (bottom pogo, GND) ── PCB-GND plane
```

### MCU (STM32G030K6T6, LQFP-32)

**Pin map locked against ST DS12991 Rev 6 Table 13** ("STM32G030KxT
LQFP32 pin definition"). The K-suffix LQFP-32 package puts **PC6 at
pin 20** between PA9 (pin 19) and PA10 (pin 21) — there is **no
PA11/PA12 → PA9/PA10 SYSCFG remap dependency**; USART1 TX/RX/DE go
directly to PA9/PA10/PA12 at AF1.

**Functional assignment:**

| Function | STM32 signal | Alt. function | Notes |
|---|---|---|---|
| UART_TX | USART1_TX (PA9) | AF1 | Direct, no remap |
| UART_RX | USART1_RX (PA10) | AF1 | Direct, no remap |
| UART_DE | USART1_RTS_DE_CK (PA12) | AF1 | Hardware-toggled DE |
| (no /RE GPIO) | — | — | Tie SN65HVD75 /RE to GND (always-receive); firmware discards TX echo on RX |
| STEPPER_IN1..4 | PA4 / PA5 / PA6 / PA7 | GPIO | TPL7407L inputs |
| HALL_IN | PB0 | GPIO + EXTI | Hall sensor with 10 kΩ pull-up |
| IDENTIFY_BTN | PA8 | GPIO + EXTI | 10 kΩ pull-up + 100 nF debounce |
| LED_HEARTBEAT | PB1 | GPIO out | **sink-driven** (GPIO → cathode) |
| LED_FAULT | PB2 | GPIO out | **sink-driven** (GPIO → cathode) |
| LED_IDENTIFY | PA15 | GPIO out | **sink-driven** (GPIO → cathode) |
| SWDIO | PA13 | SYS | Test pad |
| SWCLK | PA14 | SYS | Test pad |
| NRST | PF2 | reset | 10 kΩ pull-up + 100 nF + SWD pad |

**Pin-by-pin LQFP-32 K-suffix map** (per DS12991 Rev 6 Table 13):

| Pin | Function | Net |
|---|---|---|
| 1 | VDD | 3V3 + 100 nF decap |
| 2 | PC14/OSC32_IN | NC |
| 3 | PC15/OSC32_OUT | NC |
| 4 | PF2/NRST | NRST: 10 kΩ pull-up + 100 nF + SWD pad |
| 5 | PA0 | NC (spare) |
| 6 | PA1 | NC (spare) |
| 7 | PA2 | NC (spare) |
| 8 | PA3 | NC (spare) |
| 9 | PA4 | STEPPER_IN1 → U2 pin 1 |
| 10 | PA5 | STEPPER_IN2 → U2 pin 2 |
| 11 | PA6 | STEPPER_IN3 → U2 pin 3 |
| 12 | PA7 | STEPPER_IN4 → U2 pin 4 |
| 13 | PB0 | HALL_IN ← J3 pin 3 (external hall OUT, 10 kΩ R_hall pull-up to 3V3) |
| 14 | PB1 | LED_HEARTBEAT → **D1 cathode** (sink); D1 anode → R_led 1 kΩ → 3V3 |
| 15 | PB2 | LED_FAULT → **D2 cathode** (sink); D2 anode → R_led 1 kΩ → 3V3 |
| 16 | VSS | GND |
| 17 | VDD | 3V3 + 100 nF decap |
| 18 | PA8 | IDENTIFY_BTN ← SW1 (10 kΩ pull-up + 100 nF debounce) |
| 19 | **PA9** | UART_TX (USART1_TX, AF1) → SN65HVD75 D (pin 4) |
| 20 | **PC6** | NC (spare) — sits between PA9 and PA10 on K-suffix package |
| 21 | **PA10** | UART_RX (USART1_RX, AF1) ← SN65HVD75 R (pin 1) |
| 22 | **PA11** | NC (spare) — leave unconnected unless deliberately used |
| 23 | **PA12** | UART_DE (USART1_RTS_DE_CK, AF1) → SN65HVD75 DE (pin 3) via 1 kΩ R_de |
| 24 | PA13/SWDIO | SWDIO test pad |
| 25 | PA14/SWCLK | SWCLK test pad |
| 26 | PA15 | LED_IDENTIFY → **D3 cathode** (sink); D3 anode → R_led 1 kΩ → 3V3 |
| 27 | PB3 | NC |
| 28 | PB4 | NC |
| 29 | PB5 | NC |
| 30 | PB6 | NC |
| 31 | PB7 | NC |
| 32 | PB8 | NC |

**Important corrections vs. earlier drafts:**
- Earlier drafts placed PA10 at pin 20; **pin 20 is PC6**, not PA10.
- Earlier drafts assumed a PA11/PA12 → PA9/PA10 SYSCFG remap. **No
  remap is needed** — PA9/PA10/PA12 are at their package-pin-labelled
  positions (19/21/23) and route directly to USART1 AF1.
- LED nets terminate at the LED **cathode** (sink-driven). Earlier
  drafts inconsistently said "→ D_n anode". The driving direction is
  fixed: 3V3 → R_led → anode → cathode → MCU GPIO (sinks low to
  illuminate).

`/RE` is **not** wired to a GPIO — SN65HVD75 pin 2 is tied permanently
to GND. The firmware listens to its own TX echo and discards it,
which is standard half-duplex practice and saves a GPIO + a series
resistor.

### TPL7407L (stepper driver, SOIC-16 narrow)

| Pin | Function | Net |
|---|---|---|
| 1 | IN1 | STEPPER_IN1 ← MCU PA4 |
| 2 | IN2 | STEPPER_IN2 ← MCU PA5 |
| 3 | IN3 | STEPPER_IN3 ← MCU PA6 |
| 4 | IN4 | STEPPER_IN4 ← MCU PA7 |
| 5 | IN5 | GND (unused channel) |
| 6 | IN6 | GND (unused channel) |
| 7 | IN7 | GND (unused channel) |
| 8 | GND | GND |
| 9 | COM | 12V (motor common) |
| 10 | OUT7 | NC |
| 11 | OUT6 | NC |
| 12 | OUT5 | NC |
| 13 | OUT4 | J2 pin 4 (motor coil 4) |
| 14 | OUT3 | J2 pin 3 (motor coil 3) |
| 15 | OUT2 | J2 pin 2 (motor coil 2) |
| 16 | OUT1 | J2 pin 1 (motor coil 1) |

J2 (JST-XH **5-pin**) carries the 4 motor coil drive lines plus the
+12 V supply to the 28BYJ-48 stepper:

| J2 pin | Wire colour (28BYJ-48) | Net | Source |
|---|---|---|---|
| 1 | orange | coil 1 | TPL7407L pin 16 (OUT1) |
| 2 | yellow | coil 2 | TPL7407L pin 15 (OUT2) |
| 3 | pink   | coil 3 | TPL7407L pin 14 (OUT3) |
| 4 | blue   | coil 4 | TPL7407L pin 13 (OUT4) |
| 5 | red    | +12 V common | PCB-12V rail (post-Q1) |

Routing pin 5 to the on-board +12 V rail eliminates 64× hand-splicing
the red wire into the chassis harness. The 28BYJ-48 plugs in directly
with its standard 5-pin connector.

(Unused TPL7407L channels IN5-IN7 must be tied to GND, not floating.)

### SN65HVD75 (RS-485 transceiver, SOIC-8)

| Pin | Function | Net |
|---|---|---|
| 1 | R | UART_RX → MCU PA10 (USART1_RX) |
| 2 | /RE | **tied to GND** (always-receive; firmware discards TX echo) |
| 3 | DE | DE ← MCU PA12 (USART1_RTS_DE_CK) via 1 kΩ R_de |
| 4 | D | UART_TX ← MCU PA9 (USART1_TX) |
| 5 | GND | GND |
| 6 | A | RS485_A → SM712 pin 1 + PG2 (upper-middle pogo) |
| 7 | B | RS485_B → SM712 pin 3 + PG3 (lower-middle pogo) |
| 8 | VCC | 3V3 + 100 nF decap |

### SM712-02HTG (RS-485 ESD, SOT-23)

| Pin | Function | Net |
|---|---|---|
| 1 | I/O1 | RS485_A |
| 2 | GND | GND |
| 3 | I/O2 | RS485_B |

### Hall sensor connector (J3) — 3-pin JST-XH male, vertical THT

**Pinout LOCKED to KY-003 module native order** (so v1's existing
flying-lead cable plugs straight in without re-wiring):

| Pin | Function | Net |
|---|---|---|
| 1 | +3V3 (VCC) | 3V3 |
| 2 | GND | GND |
| 3 | HALL_OUT (SIG) | HALL_IN → MCU PB0 (via 10 kΩ R_hall pull-up to 3V3) |

If a different sensor module is used, the cable end adapts (re-crimp
the 3-pin housing or solder a pigtail) — the PCB pinout stays fixed.

**Hall sensor lives off-board** on a 3-conductor flying lead (matches
v1 mechanical alignment path). Choice of sensor module is build-time:

- **KY-003** (open-collector hall module — what v1 uses) — open-drain
  output works directly with the on-board R_hall pull-up.
- Any other 3-pin hall module (Allegro A1101 breakout, etc.) with the
  same VCC/GND/OUT pinout.

The unit PCB does not constrain magnet alignment — that is set by the
chassis bracket holding the hall module relative to the flap drum.
The PCB just provides the cable interface and signal conditioning
(R_hall pull-up, optional debounce capacitor on the OUT line).

### IDENTIFY button (SW1)

```
SW1 pin A ── IDENTIFY_BTN net ──┬── MCU PA8
                                  ├── R_id 10kΩ ── 3V3
                                  └── C_id 100nF ── GND
SW1 pin B ── GND
```

### LEDs (sink-driven — locked across pin table and this section)

```
3V3 ── R_led 1kΩ ── D1 anode ── D1 cathode ── MCU PB1  (LED_HEARTBEAT, MCU sinks low to illuminate)
3V3 ── R_led 1kΩ ── D2 anode ── D2 cathode ── MCU PB2  (LED_FAULT)
3V3 ── R_led 1kΩ ── D3 anode ── D3 cathode ── MCU PA15 (LED_IDENTIFY)
```

LEDs **sink-driven**: MCU GPIO connects to the LED **cathode** and
sinks current to GND when the GPIO is driven low. Anode is tied
through R_led 1 kΩ to 3V3. This matches the pin table above; earlier
drafts that said "GPIO → anode" were wrong.

### Pogo pins (PG1-PG4, on unit underside)

```
PG1 (top, 12V)    ─── 12V net (→ Q1 drain, before reverse-block)
PG2 (upper-mid, A) ─── RS485_A net (to SN65HVD75 pin 6, SM712 pin 1)
PG3 (lower-mid, B) ─── RS485_B net (to SN65HVD75 pin 7, SM712 pin 3)
PG4 (bottom, GND) ─── GND net
```

**No 5th polarization pogo (PG_KEY removed).** The earlier PG_KEY at
y = +14 mm collided with PG1 at y = +12 mm (2 mm centre-to-centre with
2.45 mm pads = physical overlap), and a spring pogo pressing on bare
FR-4 is not a hard mechanical interlock anyway. Polarization is now
enforced by an asymmetric 3D-printed DIN clip — see Mechanical section.

### SWD test pads

| Pad | Net | Notes |
|---|---|---|
| TP_SWDIO | MCU PA13 | |
| TP_SWCLK | MCU PA14 | |
| TP_NRST | MCU NRST | 10kΩ pull-up + 100nF cap on this net |
| TP_GND | GND | |

### General-purpose test pads

| Pad | Net |
|---|---|
| TP1 | GND |
| TP2 | 3V3 |
| TP3 | UART_RX (PA10) |
| TP4 | UART_TX (PA9) |
| TP5 | NRST |

## Floorplan (rough placement) — chassis-compatible with v1

The v2 unit PCB outline is **80 × 40 mm**, identical to v1
(`PCB/v1/Gerber_PCB_Splitflap.zip`), with the same 4-corner mounting
hole pattern. v2 is a chassis drop-in replacement.

**Hard placement constraints (preserve v1 chassis cable runs):**
- **J2 stepper output**: top-left short edge, same XY as v1's
  "28BYJ-48 Stepper" header (now 5-pin to carry +12V — see J2 spec).
- **J3 hall sensor connector**: just below J2, same XY as v1's
  "Magnet Sensor" 3-pin header.
- **Mounting holes (LOCKED, extracted from v1 Gerber_Drill_NPTH.DRL)**:
  4× M3 clearance (3.2 mm) at:
    - (3.0, 3.0)  mm  ← top-left
    - (3.0, 77.0) mm  ← bottom-left
    - (37.0, 77.0) mm ← bottom-right
    - (37.0, 3.0) mm  ← top-right
  (origin = top-left corner of 40 × 80 mm outline; Y axis pointing
  down). 34 × 74 mm hole-to-hole bounding rectangle, centred on the
  board with 3 mm edge clearance. This pattern is what the 3D-printed
  DIN clip mounts to.

**Layer assignments (preserve v1 heritage where compatible):**
- **Front (top)**: connectors (J2, J3), IDENTIFY button (SW1) on a
  visible edge, status LEDs (D1/D2/D3), SWD test pads.
- **Back (bottom)**: stepper driver U2 TPL7407L in **SOIC-16 narrow
  (150 mil)** placed near v1's ULN2003A position on the top-right
  back. **The footprint is NOT identical to v1**: v1's footprint
  (whatever its width) must be replaced with the narrow SOIC-16
  pattern in EasyEDA — verify against the TPL7407L datasheet
  (TI SLOSEH7) and the ULN2003AD (narrow) datasheet. LDO U3
  **LDL1117S33TR (SOT-223, 40 V max VIN)** placed near v1's AMS1117
  position; SOT-223 with the GND tab pulled into a copper pour for
  heatsinking (~1 cm² on bottom layer). The AMS1117 footprint may
  need adjustment — both are SOT-223 but the tab/pin order should be
  cross-checked against the LDL1117 datasheet. MCU U1 STM32G030K
  LQFP-32, RS-485 transceiver U4 SN65HVD75, Q1 AO3401 + Z1 12 V
  Zener clamp, ESD, decaps — placed wherever leaves a clear ~10 mm
  vertical channel through the long-axis centre for the pogo column.

```
   ─────────────────────── 80 mm ───────────────────────
  ┌──○─────────────────────────────────────────────○──┐
  │ J2 28BYJ-48                                       │
  │  ●●●                                              │
  │ J3 hall                            (FRONT side)   │   40 mm
  │  ●●●                                              │
  │                                                   │
  │   SW1 IDENT       (PG column on back layer        │
  │   D1 D2 D3        directly below — see below)     │
  │   HB FT ID                                        │
  │                                                   │
  │   TP_SWD pads                                     │
  ├──○─────────────────────────────────────────────○──┤

   ────────────── BACK side (component layer) ──────────
  ┌──○─────────────────────────────────────────────○──┐
  │                                                   │
  │           [U2 TPL7407L SOIC-16 narrow]            │   40 mm
  │           (placed near v1 ULN2003A spot;          │
  │            footprint = 150 mil narrow, not v1's)  │
  │                                                   │
  │   [U1 STM32G030] ●  PG1 12V    (y=+12)            │
  │   [U4 SN65HVD75] ●  PG2 A      (y= +4)            │
  │                  ●  PG3 B      (y= -4)            │
  │ [U3 LDL1117S33   ●  PG4 GND    (y=-12)            │
  │  SOT-223 + pour] ↑                                │
  │   [Q1, Z1 12V Z, column at long-axis CENTRE       │
  │    D4 TVS, D5    (x = 40 mm)                      │
  │    ESD]                                           │
  │                  (x = 40 mm)                      │
  ├──○─────────────────────────────────────────────○──┤
   ───────────────────── 80 mm ─────────────────────────

  ○ = mounting hole, M3 clearance, position copied from v1
```

Hall sensor connector (J3) position: matches v1's "Magnet Sensor"
header. The 3-conductor flying lead exits the same edge it does on
v1, so existing chassis bracket + cable run is unchanged.

**DIN rail clip on the back** — **3D-printed asymmetric clip from
MakerWorld** (one printed per unit; user supplies STL link in build
notes). Bolts to the unit board through the 4× M3 holes at (3, 3),
(3, 77), (37, 77), (37, 3). The clip is the **only** polarization
mechanism — its asymmetric profile means the unit physically only
seats one way up on the rail. Reversed orientation does not engage
the rail (no PG_KEY pogo backup). Clip carries the unit's long axis
(80 mm) parallel to the rail. The bus PCB sits in the rail channel
directly above the clip; pogo pins on the back at long-axis centre
contact the bus PCB's contact stations.

## Pogo pin geometry

**4** through-hole pogo pins on the unit's **bottom layer**, dead-centre
on the long axis (`x = 40 mm` on an 80 × 40 mm board, or `x = 0` if
origin is the board centre). Single vertical line, no polarization
pogo:

| Pin | X (centred on long axis) | Y (along short axis) | Function |
|---|---|---|---|
| PG1 | 0 | +12 mm | 12V |
| PG2 | 0 |  +4 mm | RS485_A |
| PG3 | 0 |  -4 mm | RS485_B |
| PG4 | 0 | -12 mm | GND |

Spacing matches bus PCB trace centre-to-centre pitch (8 mm). Y-offsets
±12, ±4 mm exactly match the bus PCB trace centres (4 / 12 / 20 /
28 mm from top of the 32 mm board) so there is no mechanical Y
mismatch.

**Polarization is enforced mechanically by the 3D-printed DIN clip on
the back, NOT by a 5th pogo pin.** The clip is asymmetric — only one
mounting orientation engages the rail correctly. A reversed unit
cannot seat. See Mechanical section for the clip MPN/STL.

**Centred for DIN rail mounting**: the rail runs parallel to the
unit's long axis, with the rail clip on the back centreline. The bus
PCB rests in the rail channel directly above the clip; pogo pins
contact the bus PCB's nearest contact station as the unit is clipped
in. The 24 mm pogo column fits inside the 40 mm short axis with
8 mm clearance to each long edge.

## Fab parameters

| Parameter | Value |
|---|---|
| Outline | **80 × 40 mm** (matches v1 outline + 4-corner mounting hole pattern; chassis drop-in compatible) |
| Layer count | 2 |
| Substrate | FR-4 |
| Thickness | 1.6 mm |
| Copper weight | 1 oz |
| Surface finish | HASL (cheaper) — pogo pins on bus PCB side, not unit side, so unit doesn't need ENIG |
| Mounting holes | 4× M3 clearance at corners |

## EasyEDA build steps

1. Create new EasyEDA Pro project: `splitflap-unit-v2`.
2. Schematic editor: place all components from LCSC numbers above.
3. Wire per the net tables. Use net labels for clarity:
   `12V`, `GND`, `3V3`, `RS485_A`, `RS485_B`, `STEPPER_IN1..4`,
   `HALL_IN`, `IDENTIFY_BTN`, `LED_HEARTBEAT`, `LED_FAULT`,
   `LED_IDENTIFY`, `NRST`, `UART_TX`, `UART_RX`, `DE`, `/RE`.
4. Convert to PCB.
5. PCB editor:
   - Set outline **80 × 40 mm** (matches v1).
   - Place 4× M3 mounting holes at the locked v1 coordinates:
     (3, 3), (3, 77), (37, 77), (37, 3) mm — these are the
     v1 corner hole positions extracted from
     `PCB/v1/Gerber_PCB_Splitflap.zip` → `Gerber_Drill_NPTH.DRL`.
   - Place U1 STM32G030K6T6 centrally on the back side (near v1
     ATmega328 footprint area).
   - Place stepper driver U2 (TPL7407L, **SOIC-16 narrow**) near the
     v1 ULN2003A position on the back.
   - Place LDO U3 (LDL1117S33, SOT-223) near v1's AMS1117 position;
     give the GND tab a generous copper pour for heatsinking.
   - Place RS-485 path (U4 SN65HVD75, D5 SM712) near the pogo pin pads.
   - Place hall connector J3 on a clean edge for cable exit toward
     the chassis hall bracket (matches v1 "Magnet Sensor" XY).
   - Place IDENTIFY button SW1 on the edge so it's accessible after
     install.
   - Add 4 pogo pin mounting holes (1.83 mm THT, on bottom side facing
     down) at PG1..PG4 per the geometry above.
6. Route signal traces freely; power traces should be reasonably
   wide (15-20 mil for 12V at low current internal).
7. Add ground pour on both layers.
8. Run DRC.
9. Order: 70 boards (covers 64 + spares).
