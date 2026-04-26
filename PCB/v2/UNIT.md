# Unit PCB

Per-flap board. Mounts to a 35 mm DIN rail and contacts the row's bus
PCB via 4 pogo pins on the unit's underside. Drives one 28BYJ-48 12 V
stepper, homes on an off-board hall sensor (3-pin connector), talks to its row's master
over RS-485. Identifies itself by its STM32 96-bit silicon UID, with a
manual IDENTIFY button as commissioning hardware.

Unit PCB is **identical across all 64 units**. No backplane, no cabling
to the bus, no DIP switches.

## Block diagram

```
4 pogo pins (on unit underside, pressing onto bus PCB traces)
   1=12V  2=A  3=B  4=GND
       |
       +- TVS SMAJ15A (line-to-GND on 12V)
       +- Q1 P-FET reverse-block (AO3401)
       +- Cin 22 uF + 100 nF
       |
       +-> VBUS_12V (direct to motor driver)
       |        |
       |        v
       |    [TPL7407L primary, ULN2003A footprint-compatible fallback]
       |        |
       |        v
       |    4-pin JST-XH -> 28BYJ-48 12 V stepper
       |
       +-> U1 LDO HT7833 12->3.3 V -> VCC_3V3
                                          |
                                          v
   STM32G030K6T6
       |
       +- 4 GPIO -> motor driver inputs
       +- 1 GPIO <- J3 (3-pin JST-XH) <- KY-003 hall module on flying lead (open-drain, 10k pull-up to 3V3 on PCB)
       +- 1 GPIO -> DE, 1 GPIO -> /RE  ->  SN65HVD75 RS-485 PHY
       +- USART_TX -> SN65HVD75 D
       +- USART_RX <- SN65HVD75 R
       +- 1 GPIO <- IDENTIFY tact switch (active-low; internal pull-up)
       +- 1 GPIO -> IDENTIFY LED (firmware pulses during commissioning)
       +- 1 GPIO -> HEARTBEAT LED
       +- 1 GPIO -> FAULT LED
       +- SWD pads (SWDIO, SWCLK, RST, GND)
       |
   [SN65HVD75]
       |
       +- A/B -> pogo pins 2/3
       +- ESD: SM712-02HTG across A/B to GND
```

## Bus contact (pogo pins)

4 through-hole pogo pins mounted on the unit PCB underside, projecting
downward to contact the bus PCB beneath the DIN rail.

| Pogo pin | Net | Position on unit underside |
|---|---|---|
| 1 | 12V | top edge (matches outer-top trace on bus PCB) |
| 2 | RS485_A | upper-middle |
| 3 | RS485_B | lower-middle |
| 4 | GND | bottom edge (matches outer-bottom trace on bus PCB) |

Spacing: ~8 mm between adjacent pin centres, ~24 mm total span.

Pogo pin spec:
- Through-hole mount, ~1 mm tip diameter, 1-1.5 mm spring travel
- Gold-plated tips for reliability against ENIG bus PCB pads
- ~100-250 g spring force per pin
- Examples: Mill-Max 0907 series, generic R-PT (AliExpress for hobby
  cost ~€0.30 each) or Mouser-stocked equivalents

## Power

- Input: 12 V from bus PCB via pogo pins.
- Reverse protection: P-FET (AO3401) in low-side return — protects
  against accidental reversed unit insertion if polarisation fails.
- Bulk cap: 22 uF + 100 nF on incoming 12 V.
- 12 V → 3.3 V: HT7833 LDO. Load ~55 mA worst case. Dissipation ~0.48 W.
- Stepper coils: directly on 12 V via the driver.

## MCU

**STM32G030K6T6 (LQFP-32).** Required for the 96-bit silicon UID at
`UID_BASE` (0x1FFF7590). Hardware DE auto-toggle on USART1. Native
1.7-3.6 V supply. JLC Basic, ~EUR 0.55.

## IDENTIFY button + LED

- Tact switch (6×6 mm SMD) on the unit board edge, accessible after
  the unit is clipped onto the rail.
- One MCU GPIO with internal pull-up; switch shorts to GND when pressed.
- Firmware debounces and reports state on next status frame.
- Dedicated yellow IDENTIFY LED (0805) driven by its own MCU GPIO so
  the master can pulse it during commissioning.

## Motor driver

**TPL7407L** (primary). 7-channel low-side N-MOSFET array, 16-SOIC.
~12 mW dissipation per coil-on at 200 mA.

ULN2003A is footprint-compatible (also 16-SOIC) and stays as a PCBA-time
substitute if TPL7407L availability flips.

## Hall sensor

- **Off-board** module (KY-003 or any 3-pin VCC/GND/OUT hall) on a
  flying lead — same mechanical pattern as v1.
- Unit PCB has J3 (3-pin JST-XH male, vertical THT, B3B-XH-A) for the
  cable + 10 k pull-up resistor on the OUT line to 3V3.
- Sensor sensitive-face alignment to the flap drum magnet is set by
  the chassis bracket, not the PCB.
- Single magnet on the flap drum.

## RS-485 path

- Transceiver: SN65HVD75DR.
- DE and /RE controlled separately by MCU GPIO.
- ESD: SM712-02HTG across A/B to GND.
- No termination on units (only at the master and via terminator plug
  on the far end of each row).

## LEDs

- D1 HEARTBEAT (blue).
- D2 FAULT (red).
- D3 IDENTIFY (yellow). Dedicated GPIO.

## Test pads

- 5 pads on edge: GND, 3V3, UART_TX, UART_RX, RST.
- 4 SWD pads (SWDIO, SWCLK, RST, GND) for STM32G030 in-circuit programming.

## Mechanical

- Outline: ~75 × 35 mm to match v1 envelope. Existing case mechanics
  drive this.
- Mounting holes: same 4 positions as v1 (or a DIN rail clip mount if
  case is being redesigned).
- DIN rail clip on the back: standard 35 mm TS35 clip (off-the-shelf
  injection-moulded plastic clip or 3D-printed). Mechanically asymmetric
  so unit can only mount one way up — provides natural polarization for
  the pogo pin pattern.
- Pogo pins: 4× through-hole on the underside, vertical line, ~8 mm
  spacing, projecting ~3 mm below board.
- Stepper output: 4-pin JST-XH on opposite edge from DIN rail clip.
- IDENTIFY button: edge-mounted, accessible after install.
- Hall connector (J3): on a clean edge for cable exit toward the
  chassis hall bracket. Magnetic alignment is mechanical, not on-PCB.
- Stack-up: 2-layer HASL.

## BOM

See `UNIT_BOM.csv`.
