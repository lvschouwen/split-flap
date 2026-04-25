# Unit PCB

Per-flap board. Plugs into a row harness via a single 4-pin shrouded
header. Drives one 28BYJ-48 12 V stepper, homes on an on-board hall
sensor, talks to its row's master over RS-485. Identifies itself by its
STM32 96-bit silicon UID, with a manual IDENTIFY button as commissioning
hardware.

Unit PCB is **identical across all 64 units**. No backplane, no slot
wiring, no DIP switches.

## Block diagram

```
Harness connector (4-pin shrouded, 2x2, indexed)
   1=12V  2=GND  3=A  4=B
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
       +- 1 GPIO <- A1101ELHL hall sensor (open-drain, 10k pull-up to 3V3)
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
       +- A/B -> harness pins 3/4
       +- ESD: SM712-02HTG across A/B to GND
```

## MCU

**STM32G030K6T6 (LQFP-32).**

Hardware-merit reasons:
- 96-bit unique device ID readable from the `UID_BASE` register
  (0x1FFF7590), three 32-bit words. Used as the unit's stable network
  identity. Ports like ATmega328P do not have a manufacturer-baked UID.
- Hardware DE auto-toggle on USART1 — TX direction control in the UART
  peripheral, deterministic timing.
- Native 1.7-3.6 V supply, matches 3.3 V rail directly.
- 32 KB flash / 8 KB SRAM is comfortable for stepper firmware + RS-485
  protocol + UID handling.
- JLC Basic, ~EUR 0.55 per unit.

This is settled. The previous "STM32 vs ATmega" decision is closed because
ATmega328P does not have a hardware UID and the IDENTIFY-by-UID flow is
the addressing mechanism.

## IDENTIFY button + LED

- Tact switch (6x6 mm SMD, 50 mA) on the unit board edge, accessible
  through the case opening.
- One MCU GPIO with internal pull-up; switch shorts to GND when pressed.
- Firmware debounces and reports state on next status frame.
- IDENTIFY LED (e.g., yellow 0805) driven by another MCU GPIO so master
  can pulse it during commissioning ("press the unit whose LED is
  flashing").
- If board area is tight, the IDENTIFY LED can share with FAULT (different
  pulse pattern); decision in firmware design.

## Power

- Input: 12 V from harness. No second rail. No on-board buck.
- Reverse protection: P-FET (AO3401) in low-side return.
- Bulk cap: 22 uF + 100 nF on incoming 12 V.
- 12 V -> 3.3 V: HT7833 LDO. Load ~55 mA worst case. Dissipation ~0.48 W.
- Stepper coils: directly on 12 V via the driver.

## Motor driver

| Aspect | TPL7407L (primary) | ULN2003A (fallback) |
|---|---|---|
| Tech | 7-channel low-side N-MOSFET array | 7-channel Darlington array |
| V_low at 200 mA | ~0.06 V | ~0.9 V |
| Heat per coil-on | ~12 mW | ~180 mW |
| Pinout | identical 16-SOIC | identical 16-SOIC |
| JLC Basic | typically yes | yes |

PCB footprint takes both. BOM swap if availability flips.

## Hall sensor

- A1101ELHL (Allegro), unipolar latching, 100 G op / 45 G release.
- SOT-23W, output is open-drain, 10 k pull-up to 3.3 V.
- Single magnet on the flap drum.
- Position selected to match v1 PCB's hall coordinate.

## RS-485 path

- Transceiver: SN65HVD75DR.
- DE and /RE controlled separately by MCU GPIO.
- ESD: SM712-02HTG across A/B to GND.
- No termination resistor on units (only master + far end of harness).
- No bias on units (only master).

## LEDs

- D1 HEARTBEAT (blue).
- D2 FAULT (red).
- D3 IDENTIFY (yellow). Optional — can be merged with FAULT if board
  area requires.

## Test pads

- 5 pads on edge: GND, 3V3, UART_TX, UART_RX, RST.
- 4 SWD pads (SWDIO, SWCLK, RST, GND) for STM32G030 in-circuit programming
  + debug.

## Mechanical

- Outline: ~75 x 35 mm to match v1 envelope. Existing case mechanics drive
  this — non-negotiable.
- Mounting holes: same 4 positions as v1.
- Harness connector: 4-pin shrouded box header on back edge.
- Stepper output: 4-pin JST-XH on opposite edge.
- IDENTIFY button: edge-mounted, accessible after case assembly.
- Hall sensor: positioned to match v1 magnet path.
- Stack-up: 2-layer HASL.

## BOM

See `UNIT_BOM.csv`.
