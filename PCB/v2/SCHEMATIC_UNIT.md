# Unit PCB — Schematic + Layout Specification

EasyEDA-ready spec for the unit PCB. **64 of these per system; one
PCB design.**

## Component list (with LCSC starting points)

| Ref | Value | Footprint | LCSC# (CHECK) | Notes |
|---|---|---|---|---|
| U1 | STM32G030K6T6 | LQFP-32 | C2040675 | MCU; required for silicon UID |
| U2 | TPL7407L | SOIC-16W | C383290 | Stepper driver primary; ULN2003A drop-in alt: C2358 |
| U3 | HT7833 | SOT-89-5 | C70358 | 12V→3.3V LDO 500 mA |
| U4 | SN65HVD75DR | SOIC-8 | C57928 | RS-485 transceiver |
| U5 | A1101ELHL | SOT-23W | C504318 | Hall sensor |
| Q1 | AO3401A | SOT-23 | C15127 | P-FET reverse-block |
| D1 | LED blue | 0805 | C2293 | HEARTBEAT |
| D2 | LED red | 0805 | C2286 | FAULT |
| D3 | LED yellow | 0805 | C2298 | IDENTIFY |
| D4 | SMAJ15A | DO-214AC SMA | C167238 | 12V TVS |
| D5 | SM712-02HTG | SOT-23 | C172881 | RS-485 ESD |
| SW1 | Tact switch 6×6 | SMD 6x6x5 | C318884 | IDENTIFY button |
| PG1-PG4 | Pogo pin spring contact | THT, 1 mm tip dia | n/a | Mill-Max 0907-0-15-20-75-14-11-0 or generic gold-tip ~3 mm length |
| J2 | JST-XH 4-pin male | THT B4B-XH-A | C158012 | Stepper output |
| C_in | 22 µF / 25 V X7R | 0805 | C45783 | 12V input bulk |
| C_in2 | 100 nF X7R | 0603 | C14663 | 12V decap |
| C_ldo_in | 10 µF / 25 V X7R | 0805 | C19702 | LDO input |
| C_ldo_out | 10 µF / 10 V X7R | 0805 | C15850 | LDO output |
| C_decap (×4) | 100 nF X7R | 0603 | C14663 | MCU + transceiver decoupling |
| C_rst | 100 nF X7R | 0603 | C14663 | NRST filter |
| C_id | 100 nF X7R | 0603 | C14663 | IDENTIFY button debounce |
| R_hall | 10 kΩ 1% | 0603 | C25804 | Hall sensor pull-up |
| R_de | 1 kΩ 1% | 0603 | C21190 | DE driver line series |
| R_re | 1 kΩ 1% | 0603 | C21190 | /RE driver line series |
| R_led (×3) | 1 kΩ 1% | 0603 | C21190 | LED current limit |
| R_id | 10 kΩ 1% | 0603 | C25804 | IDENTIFY button pull-up |
| R_rst | 10 kΩ 1% | 0603 | C25804 | NRST pull-up |

## Net-by-net wiring

### Power section

```
PG1 (top pogo, 12V) ──┬── D4 SMAJ15A anode (cathode → GND)
                       │
                       ├── Q1 source (P-FET, reverse-block in low-side return path*)
                       │
                       ├── C_in 22µF
                       ├── C_in2 100nF
                       │
                       ├── U2 TPL7407L pin 9 (COM, common collector/source for motor coils)
                       │
                       └── U3 HT7833 input (pin 1 VIN)

   * Note: AO3401 is P-channel; in this circuit the drain goes to PG4 (GND),
     source goes to PCB-GND, gate to incoming 12V via 10k pull-up.
     Body diode conducts at startup, gate enhances when polarity correct.

U3 HT7833:
   pin 1 (VIN)  ← 12V
   pin 2 (GND)  → GND
   pin 3 (NC)   ← (no connect)
   pin 4 (GND)  → GND
   pin 5 (VOUT) → 3V3 + C_ldo_out 10µF + decoupling caps for downstream chips

PG4 (bottom pogo, GND) ── PCB-GND plane
```

### MCU (STM32G030K6T6, LQFP-32)

| Pin | Net | Notes |
|---|---|---|
| 1 | VDD | 3V3 + 100 nF decap |
| 2 | PC14/OSC32_IN | NC |
| 3 | PC15/OSC32_OUT | NC |
| 4 | PF2/NRST | NRST: 10kΩ pull-up to 3V3, 100nF cap to GND, SWD pad |
| 5 | PA0 | UART_TX (USART2_TX) → SN65HVD75 D pin (via 1kΩ R_de? or direct) |
| 6 | PA1 | UART_RX (USART2_RX) ← SN65HVD75 R pin |
| 7 | PA2 | DE → SN65HVD75 DE pin (via 1kΩ R_de) |
| 8 | PA3 | /RE → SN65HVD75 /RE pin (via 1kΩ R_re) |
| 9 | PA4 | STEPPER_IN1 → U2 pin 1 |
| 10 | PA5 | STEPPER_IN2 → U2 pin 2 |
| 11 | PA6 | STEPPER_IN3 → U2 pin 3 |
| 12 | PA7 | STEPPER_IN4 → U2 pin 4 |
| 13 | PB0 | HALL_IN ← U5 hall output (with 10kΩ R_hall pull-up to 3V3) |
| 14 | PB1 | LED_HEARTBEAT → D1 anode (via 1kΩ R_led) |
| 15 | PB2 | LED_FAULT → D2 anode (via 1kΩ R_led) |
| 16 | VSS | GND |
| 17 | VDD | 3V3 + 100 nF decap |
| 18 | PA8 | LED_IDENTIFY → D3 anode (via 1kΩ R_led) |
| 19 | PA9 | IDENTIFY_BTN ← SW1 (10kΩ pull-up to 3V3, 100nF debounce cap to GND) |
| 20 | PA10 | NC (or expansion/test) |
| 21 | PA11 | NC |
| 22 | PA12 | NC |
| 23 | PA13/SWDIO | SWDIO test pad |
| 24 | PA14/SWCLK | SWCLK test pad |
| 25 | PA15 | NC |
| 26 | PB3 | NC |
| 27 | PB4 | NC |
| 28 | PB5 | NC |
| 29 | PB6 | NC |
| 30 | PB7 | NC |
| 31 | PB8 | NC |
| 32 | VSS | GND |

Note: USART2 chosen because it has the hardware DE auto-toggle on
some STM32G030 packages. **Verify** USART selection against
STM32G030K6T6 datasheet for hardware DE pin availability — alternative
is USART1 on PA9/PA10 if needed.

If hardware DE is on USART1 instead, swap UART signals to PA9/PA10
and free PA0-PA3 for other use; rewire IDENTIFY_BTN to a free pin.

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
| 1 | R | UART_RX → MCU PA1 |
| 2 | RE | /RE ← MCU PA3 (via 1kΩ R_re) |
| 3 | DE | DE ← MCU PA2 (via 1kΩ R_de) |
| 4 | D | UART_TX ← MCU PA0 |
| 5 | GND | GND |
| 6 | A | RS485_A → SM712 pin 1 + PG2 (upper-middle pogo) |
| 7 | B | RS485_B → SM712 pin 3 + PG3 (lower-middle pogo) |
| 8 | VCC | 3V3 + 100nF decap |

### SM712-02HTG (RS-485 ESD, SOT-23)

| Pin | Function | Net |
|---|---|---|
| 1 | I/O1 | RS485_A |
| 2 | GND | GND |
| 3 | I/O2 | RS485_B |

### A1101ELHL (hall sensor, SOT-23W)

| Pin | Function | Net |
|---|---|---|
| 1 | VCC | 3V3 |
| 2 | GND | GND |
| 3 | OUT | HALL_IN → MCU PB0 (via 10kΩ R_hall pull-up to 3V3) |

### IDENTIFY button (SW1)

```
SW1 pin A ── IDENTIFY_BTN net ──┬── MCU PA9
                                  ├── R_id 10kΩ ── 3V3
                                  └── C_id 100nF ── GND
SW1 pin B ── GND
```

### LEDs

```
3V3 ── R_led 1kΩ ── D1 anode ── D1 cathode ── MCU PB1 (LED_HEARTBEAT, sinks)
3V3 ── R_led 1kΩ ── D2 anode ── D2 cathode ── MCU PB2 (LED_FAULT)
3V3 ── R_led 1kΩ ── D3 anode ── D3 cathode ── MCU PA8 (LED_IDENTIFY)
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
| TP3 | UART_RX (PA1) |
| TP4 | UART_TX (PA0) |
| TP5 | NRST |

## Floorplan (rough placement)

```
                    75 mm
   +─────────────────────────────────+
   │                                  │ <-- TOP edge
   │  Pogo pins (underside, view as below)
   │  ●  PG1  12V    (top)            │
   │                                  │
   │  ●  PG2  A      (upper-middle)   │
   │  ●  PG3  B      (lower-middle)   │
   │                                  │
   │  ●  PG4  GND    (bottom)         │
   │                                  │
   +─────────────────────────────────+
                                       
   On unit's component side (top):
   +─────────────────────────────────+
   │ J2 (motor)                       │
   │  ●●●●                            │
   │   |                              │
   │   v                              │ 35 mm
   │  U2 TPL7407L                     │
   │                                  │
   │  ┌──────┐                         │
   │  │ U1   │ STM32G030K6T6           │
   │  │ LQFP │                          │
   │  └──────┘                         │
   │                                  │
   │  U4 SN65HVD75    U5 A1101 hall   │
   │                                  │
   │  D1 D2 D3                        │
   │  HB FT ID                        │
   │                                  │
   │  SW1 IDENTIFY  TP_SWD pads       │
   │                                  │
   +─────────────────────────────────+
   
   M-holes at corners (4× M3 clearance)
```

Hall sensor (U5) position: match the v1 PCB's hall coordinate
relative to the flap drum magnet path. Use v1 Gerber drill file
(`PCB/v1/Gerber_PCB_Splitflap.zip`) as the alignment reference.

## Pogo pin geometry

4 through-hole pogo pins on the unit's **bottom layer** projecting
downward by ~3 mm:

| Pin | X position (relative to board centre) | Y position (vertical line) |
|---|---|---|
| PG1 (12V) | 0 (centred) | +12 mm (top edge) |
| PG2 (A) | 0 | +4 mm |
| PG3 (B) | 0 | -4 mm |
| PG4 (GND) | 0 | -12 mm (bottom edge) |

Spacing matches bus PCB trace centre-to-centre pitch (8 mm). Pogo
pin tips contact the bus PCB ENIG-plated zones when unit is clipped
onto DIN rail.

## Fab parameters

| Parameter | Value |
|---|---|
| Outline | 75 × 35 mm |
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
   - Set outline 75 × 35 mm.
   - Place ESP32-S3 module... wait, this is the *unit* board — place
     U1 STM32G030K6T6 centrally.
   - Place stepper driver U2 near J2 stepper output.
   - Place RS-485 path (U4, D5) near the pogo pin pads.
   - Place hall sensor U5 at the v1-matching position.
   - Place IDENTIFY button SW1 on the edge so it's accessible after
     install.
   - Add 4 pogo pin mounting holes (1 mm THT pads, on bottom side
     facing down) per the geometry above.
6. Route signal traces freely; power traces should be reasonably
   wide (15-20 mil for 12V at low current internal).
7. Add ground pour on both layers.
8. Run DRC.
9. Order: 70 boards (covers 64 + spares).
