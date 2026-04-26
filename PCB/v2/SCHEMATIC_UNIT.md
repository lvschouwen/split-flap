# Unit PCB — Schematic + Layout Specification

**Revision:** 2026-04-26

EasyEDA-ready spec for the unit PCB. **64 of these per system; one
PCB design.**

## Component list (with LCSC starting points)

| Ref | Value | Footprint | LCSC# (CHECK) | Notes |
|---|---|---|---|---|
| U1 | STM32G030K6T6 | LQFP-32 | C2040675 | MCU; required for silicon UID |
| U2 | TPL7407L | SOIC-16W | C383290 | Stepper driver primary; ULN2003A drop-in alt: C2358 |
| U3 | HT7833 | SOT-89-5 | C70358 | 12V→3.3V LDO 500 mA |
| U4 | SN65HVD75DR | SOIC-8 | C57928 | RS-485 transceiver |
| J3 | Hall connector | JST-XH 3-pin male, vertical THT (B3B-XH-A) | C145756 | 3-pin header for external hall sensor module on flying lead (KY-003 or similar). Sensor positioned mechanically by chassis to align with flap drum magnet — hall NOT on-board (matches v1 mechanical alignment path). |
| Q1 | AO3401A | SOT-23 | C15127 | P-FET reverse-block |
| D1 | LED blue | 0805 | C2293 | HEARTBEAT |
| D2 | LED red | 0805 | C2286 | FAULT |
| D3 | LED yellow | 0805 | C2298 | IDENTIFY |
| D4 | SMAJ15A | DO-214AC SMA | C167238 | 12V TVS |
| D5 | SM712-02HTG | SOT-23 | C172881 | RS-485 ESD |
| SW1 | Tact switch 6×6 | SMD 6x6x5 | C318884 | IDENTIFY button |
| PG1-PG4 | Pogo pin spring contact | THT (drill 1.83 mm, pad 2.45 mm) | n/a (DigiKey) | **Primary: Mill-Max 0906-2-15-20-75-14-11-0** (5.00 mm free, 1.0–1.4 mm travel, 1.07 mm Au tip, 2 A continuous, 1 M cycles). **Backup: Mill-Max 0906-1-15-20-75-14-11-0** (same footprint, less travel margin). LCSC carries Mill-Max as a courtesy listing — order from DigiKey and ship to JLC for assembly, or hand-solder. **LCSC-only fallback: Xinyangze YZ02015095R-01 (LCSC C5157439)** — derate to 1 A (parallel two pins on the 12 V rail), 10k mate cycles is fine for hobby. |
| J2 | JST-XH 4-pin male | THT B4B-XH-A | C158012 | Stepper output |
| C_in | 22 µF / 25 V X7R | 0805 | C45783 | 12V input bulk |
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
                              Q1 source ── PCB-12V rail ──┬── D4 SMAJ15A anode (cathode → GND)
                                                           ├── C_in 22 µF
                                                           ├── C_in2 100 nF
                                                           ├── U2 TPL7407L pin 9 (COM)
                                                           └── U3 HT7833 input (pin 1 VIN)

Q1 AO3401A (P-FET high-side reverse-block, standard topology):
  drain  ← PG1 (incoming 12V)
  source → PCB-12V rail (load side)
  gate   ── 10 kΩ to GND

  Same topology + behaviour as the master Q1 — see SCHEMATIC_MASTER.md
  power section for the full rationale. Do NOT pull the gate to +12V;
  that would defeat the reverse-block.

U3 HT7833:
   pin 1 (VIN)  ← 12V
   pin 2 (GND)  → GND
   pin 3 (NC)   ← (no connect)
   pin 4 (GND)  → GND
   pin 5 (VOUT) → 3V3 + C_ldo_out 10µF + decoupling caps for downstream chips

PG4 (bottom pogo, GND) ── PCB-GND plane
```

### MCU (STM32G030K6T6, LQFP-32)

**⚠️ Verify pin map against STMicro DS12991 Rev 6 Table 13** ("STM32G030KxT
LQFP32 pin definition") and Table 14 ("Alternate function") **before
finalizing the schematic.** Two confirmed constraints to design around:

1. **Hardware DE auto-toggle (RS-485 driver-enable)** is available on:
   - **USART1_RTS_DE_CK** on **PA12** (AF1)
   - **USART2_RTS_DE_CK** on **PA1** (AF1)
   - LPUART1 also exposes hardware DE on selected pins.
2. **PA9/PA10 are remapped to PA11/PA12 by default on the K-suffix
   package** — the SYSCFG_CFGR1 PA11_RMP / PA12_RMP bits select which
   die pad is bonded to the package pins. The freelancer must confirm
   the remap state required to expose USART1_TX / USART1_RX on the
   chosen package pins.

**Recommended pin assignment** (preferred — uses hardware DE on PA12
which has no known conflicts on K-32):

| Function | STM32 signal | Alt. function | Notes |
|---|---|---|---|
| UART_TX | USART1_TX (PA9, post-remap) | AF1 | Confirm remap bit in SYSCFG_CFGR1 |
| UART_RX | USART1_RX (PA10, post-remap) | AF1 | Confirm remap bit |
| UART_DE | USART1_RTS_DE_CK (PA12) | AF1 | Hardware-toggled DE |
| (no /RE GPIO) | — | — | Tie SN65HVD75 /RE to GND (always-receive); firmware discards TX echo on RX |
| STEPPER_IN1..4 | PA4 / PA5 / PA6 / PA7 | GPIO | TPL7407L inputs |
| HALL_IN | PB0 | GPIO + EXTI | Hall sensor with 10 kΩ pull-up |
| IDENTIFY_BTN | PA8 | GPIO + EXTI | 10 kΩ pull-up + 100 nF debounce |
| LED_HEARTBEAT | PB1 | GPIO out | sink-driven |
| LED_FAULT | PB2 | GPIO out | sink-driven |
| LED_IDENTIFY | PA15 | GPIO out | sink-driven |
| SWDIO | PA13 | SYS | Test pad |
| SWCLK | PA14 | SYS | Test pad |
| NRST | PF2 | reset | 10 kΩ pull-up + 100 nF + SWD pad |

PA0–PA3 left as NC / spare for now — the freelancer should consult
DS12991 Table 13 to determine whether any of those pads are bonded to
other functional pads on the K-suffix package; if they are unbonded
they can be repurposed for analog test pads or future expansion.

**Why not USART2 on PA0/PA1/PA2?** Earlier drafts of this spec used
PA0/PA1/PA2/PA3 for USART2 TX/RX/DE/RE. That assignment was withdrawn
because (a) hardware DE on PA12 (USART1) gives a cleaner BOM-and-ROM
match for the firmware, and (b) the pad-bonding behavior of PA0–PA3
on K-suffix packages requires datasheet-level verification that has
not been completed at spec time. The conservative choice is USART1
with `/RE` tied to GND.

**Pin-by-pin LQFP-32 map** (verify against DS12991 Table 13):

| Pin | Function | Net |
|---|---|---|
| 1 | VDD | 3V3 + 100 nF decap |
| 2 | PC14/OSC32_IN | NC |
| 3 | PC15/OSC32_OUT | NC |
| 4 | PF2/NRST | NRST: 10 kΩ pull-up + 100 nF + SWD pad |
| 5 | PA0 | NC (spare) — verify pad bonding |
| 6 | PA1 | NC (spare) — verify pad bonding |
| 7 | PA2 | NC (spare) — verify pad bonding |
| 8 | PA3 | NC (spare) — verify pad bonding |
| 9 | PA4 | STEPPER_IN1 → U2 pin 1 |
| 10 | PA5 | STEPPER_IN2 → U2 pin 2 |
| 11 | PA6 | STEPPER_IN3 → U2 pin 3 |
| 12 | PA7 | STEPPER_IN4 → U2 pin 4 |
| 13 | PB0 | HALL_IN ← J3 pin 3 (external hall module OUT, 10 kΩ R_hall pull-up to 3V3) |
| 14 | PB1 | LED_HEARTBEAT → D1 anode (via 1 kΩ R_led) |
| 15 | PB2 | LED_FAULT → D2 anode (via 1 kΩ R_led) |
| 16 | VSS | GND |
| 17 | VDD | 3V3 + 100 nF decap |
| 18 | PA8 | IDENTIFY_BTN ← SW1 (10 kΩ pull-up + 100 nF debounce) |
| 19 | PA9 | UART_TX (USART1_TX, AF1) → SN65HVD75 D (pin 4) |
| 20 | PA10 | UART_RX (USART1_RX, AF1) ← SN65HVD75 R (pin 1) |
| 21 | PA11 | NC — bonded to PA9 unless remapped (see SYSCFG note above) |
| 22 | PA12 | UART_DE (USART1_RTS_DE_CK, AF1) → SN65HVD75 DE (pin 3) via 1 kΩ |
| 23 | PA13/SWDIO | SWDIO test pad |
| 24 | PA14/SWCLK | SWCLK test pad |
| 25 | PA15 | LED_IDENTIFY → D3 anode (via 1 kΩ R_led) |
| 26 | PB3 | NC |
| 27 | PB4 | NC |
| 28 | PB5 | NC |
| 29 | PB6 | NC |
| 30 | PB7 | NC |
| 31 | PB8 | NC |
| 32 | VSS | GND |

`/RE` is **not** wired to a GPIO — SN65HVD75 pin 2 is tied permanently
to GND. The firmware listens to its own TX echo and discards it,
which is standard half-duplex practice and saves a GPIO + a series
resistor.

### TPL7407L (stepper driver, SOIC-16W)

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

J2 (JST-XH 4-pin) carries the 4 motor coil drive lines to the
28BYJ-48 stepper. Note: the 28BYJ-48's red wire (V+) connects to 12V
externally — it's NOT carried by J2. Either jumper red wire to 12V
on the unit board internally or assume external splice.

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

| Pin | Function | Net |
|---|---|---|
| 1 | VCC | 3V3 |
| 2 | GND | GND |
| 3 | OUT | HALL_IN → MCU PB0 (via 10 kΩ R_hall pull-up to 3V3) |

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

### LEDs

```
3V3 ── R_led 1kΩ ── D1 anode ── D1 cathode ── MCU PB1 (LED_HEARTBEAT, sinks)
3V3 ── R_led 1kΩ ── D2 anode ── D2 cathode ── MCU PB2 (LED_FAULT)
3V3 ── R_led 1kΩ ── D3 anode ── D3 cathode ── MCU PA15 (LED_IDENTIFY)
```

LEDs sink-driven (MCU pulls low to illuminate). Cathode → MCU GPIO,
anode → 3V3 via current-limit resistor.

### Pogo pins (PG1-PG4, on unit underside)

```
PG1 (top, 12V)    ─── 12V net
PG2 (upper-mid, A) ─── RS485_A net (to SN65HVD75 pin 6, SM712 pin 1)
PG3 (lower-mid, B) ─── RS485_B net (to SN65HVD75 pin 7, SM712 pin 3)
PG4 (bottom, GND) ─── GND net
```

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
  "28BYJ-48 Stepper 12V" 3-pin header.
- **J3 hall sensor connector**: just below J2, same XY as v1's
  "Magnet Sensor" 3-pin header.
- **Mounting holes**: 4 corners; exact XY extracted from v1 Gerber
  drill file (`Gerber_Drill_PTH.DRL` in the v1 zip).

**Layer assignments (preserve v1 heritage):**
- **Front (top)**: connectors (J2, J3), IDENTIFY button (SW1) on a
  visible edge, status LEDs (D1/D2/D3), SWD test pads.
- **Back (bottom)**: stepper driver U2 TPL7407L (drops directly into
  v1 ULN2003A's SOIC-16W footprint, top-right back). LDO U3 HT7833
  near v1's AMS1117 position (note: SOT-89-5, **not** SOT-223 —
  needs a new pad, AMS1117 footprint will not work). MCU U1
  STM32G030K LQFP-32, RS-485 transceiver U4 SN65HVD75, AO3401, ESD,
  decaps — placed wherever leaves a clear ~10 mm vertical channel
  through the long-axis centre for the pogo column.

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
  │              [U2 TPL7407L SOIC-16W]               │   40 mm
  │              (kept in v1 ULN2003A spot)           │
  │                                                   │
  │   [U1 STM32G030] ●  PG_KEY (y=+14, polarization)  │
  │                  ●  PG1 12V    (y=+12)            │
  │   [U4 SN65HVD75] ●  PG2 A      (y= +4)            │
  │                  ●  PG3 B      (y= -4)            │
  │   [U3 HT7833]    ●  PG4 GND    (y=-12)            │
  │   [Q1, D4 TVS,   ↑                                │
  │    D5 ESD]       column at long-axis CENTRE       │
  │                  (x = 40 mm)                      │
  ├──○─────────────────────────────────────────────○──┤
   ───────────────────── 80 mm ─────────────────────────

  ○ = mounting hole, M3 clearance, position copied from v1
```

Hall sensor connector (J3) position: matches v1's "Magnet Sensor"
header. The 3-conductor flying lead exits the same edge it does on
v1, so existing chassis bracket + cable run is unchanged.

**DIN rail clip on the back** — clips onto the rail with the unit's
long axis (80 mm) parallel to the rail. The bus PCB sits in the
rail channel directly above the clip; pogo pins on the back at
long-axis centre contact the bus PCB's contact stations.

## Pogo pin geometry

5 through-hole pogo pins on the unit's **bottom layer**, dead-centre
on the long axis (`x = 40 mm` on an 80 × 40 mm board, or `x = 0` if
origin is the board centre). 4 main power/signal pogos in a vertical
line + 1 polarization key:

| Pin | X (centred on long axis) | Y (along short axis) | Function |
|---|---|---|---|
| PG1 | 0 | +12 mm | 12V |
| PG2 | 0 |  +4 mm | RS485_A |
| PG3 | 0 |  -4 mm | RS485_B |
| PG4 | 0 | -12 mm | GND |
| PG_KEY | 0 | +14 mm | Polarization (no electrical function) |

Spacing matches bus PCB trace centre-to-centre pitch (8 mm).

**PG_KEY at y=+14 mm** breaks the up/down symmetry: in normal install
it lands on a matching ENIG keying pad on the bus PCB; if the unit is
mounted reversed (180° in-plane rotation), PG_KEY lands at y=-14 mm
on bare FR-4, so the unit physically does not seat — protecting
against the 12 V → GND short that a symmetric 4-pogo install would
allow.

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
   - Set outline **80 × 40 mm** (matches v1 — extract exact corner mounting hole XY from v1 Gerber drill file at `PCB/v1/Gerber_PCB_Splitflap.zip` → `Gerber_Drill_PTH.DRL`).
   - Place ESP32-S3 module... wait, this is the *unit* board — place
     U1 STM32G030K6T6 centrally.
   - Place stepper driver U2 near J2 stepper output.
   - Place RS-485 path (U4, D5) near the pogo pin pads.
   - Place hall connector J3 on a clean edge for cable exit toward
     the chassis hall bracket.
   - Place IDENTIFY button SW1 on the edge so it's accessible after
     install.
   - Add 4 pogo pin mounting holes (1 mm THT pads, on bottom side
     facing down) per the geometry above.
6. Route signal traces freely; power traces should be reasonably
   wide (15-20 mil for 12V at low current internal).
7. Add ground pour on both layers.
8. Run DRC.
9. Order: 70 boards (covers 64 + spares).
