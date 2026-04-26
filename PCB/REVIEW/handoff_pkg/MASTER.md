# Master PCB

**Revision:** 2026-04-26

ESP32-S3 controller. Drives **4 RS-485 buses** (one per row). 64 units
total. USB-C native programming. **Sources row power**: takes one 12 V /
15 A input and outputs 12V + GND + A + B per row on a single 4-pin
combined connector.

## Block diagram

```
12 V / 15 A DC input (screw terminal)
   |
   +- F1 fuse 15 A slow-blow (5x20 mm)
   +- TVS SMAJ15A
   +- Q1 P-FET reverse-block (AOD409 or NTD2955, DPAK)
   +- Z1 BZT52C10 10 V Zener gate→source clamp on Q1 (+ R_q1g 100 Ω, R_q1g2 10 kΩ)
   +- Cbulk 470 uF / 25 V + 100 nF
   |
   +-> VBUS_12V_RAIL (wide pour, 5+ mm trace)
   |        |
   |        +-> Per-row polyfuses (×4, 4 A hold, 2920 SMD)
   |        |        |
   |        |        +-> 12V_ROW0, 12V_ROW1, 12V_ROW2, 12V_ROW3
   |        |        +-> Each combined into the row's 4-pin output (12V + GND + A + B)
   |        |        +-> Per-row PWR LED (post-polyfuse, indicates row 12V present)
   |        |
   +- U2 buck module K7803-1000R3 12->3.3 V -> VCC_3V3 (master logic; switching, ~85-90% efficient — replaces LDO due to 12V→3.3V dissipation hazard)
                                    |
                                    v
   ESP32-S3-WROOM-1-N16R8
       |
       +- USB-C (USBLC6 ESD, native CDC)
       +- BOOT button (IO0 -> GND)
       +- RESET button (EN -> GND)
       +- LEDs: PWR, HEARTBEAT, FAULT, ROW0..ROW3 (7 total)
       |
       +- UART1 -> SN65HVD75 #1 -> Bus 0 (row 0; nets RS485_A0 / RS485_B0)
       +- UART2 -> SN65HVD75 #2 -> Bus 1 (row 1; nets RS485_A1 / RS485_B1)
       +- UART0 -> SN65HVD75 #3 -> Bus 2 (row 2; nets RS485_A2 / RS485_B2)
       +- SPI -> SC16IS740 -> SN65HVD75 #4 -> Bus 3 (row 3; nets RS485_A3 / RS485_B3)
                |
                +- 14.7456 MHz crystal
                +- IRQ -> ESP32-S3 GPIO

Per-row 4-pin output (combined power + signal):
  Pin 1: 12V (post-polyfuse)
  Pin 2: RS485_A
  Pin 3: RS485_B
  Pin 4: GND

Each transceiver block:
   - DE/RE GPIOs (ESP32-S3 native; SC16IS740 GPIO for Bus 3)
   - 120 ohm termination at master end (across A/B)
   - 1k bias to 3V3 (A) + 1k bias to GND (B)
   - SM712 ESD across A/B
   - A/B routed to the 4-pin row output connector
```

## Power

- **Input: 12 V / 15 A from a single SMPS brick.** Sized for 64-unit
  worst-case peak (~16 A briefly during simultaneous flap transitions);
  steady-state draw is ~3-5 A.
- Input connector: **2-pin Phoenix-style screw terminal** (5 mm pitch).
  Robust, accepts bare-wire or terminated cable from any 12 V brick.
- Reverse-polarity: P-FET (AOD409 or NTD2955 in DPAK) handles 15 A
  continuous comfortably. **Z1 BZT52C10 10 V Zener clamp on Q1
  gate→source** (cathode → source/+12V_RAIL, anode → gate). AOD409 is
  ±20 V VGS rated so the clamp is not strictly required, but is
  included for symmetry with the unit Q1 fix (unit Q1 AO3401A needs
  10 V to stay safely inside ±12 V VGS abs-max under Zener
  tolerance — a 12 V Zener +10% = 13.2 V puts the FET out of spec).
  R_q1g 100 Ω gate series + R_q1g2 10 kΩ gate pull-down to GND
  complete the topology.
- Input fuse: 15 A slow-blow, 5×20 mm holder.
- TVS: SMAJ15A across input.
- Bulk: 470 uF / 25 V + 100 nF for inrush smoothing.
- Per-row protection: 4× polyfuses (4 A hold / 8 A trip, **2920
  package** — Bourns MF-LSMF400/16X-2 or equivalent; **NOT 1812**,
  which does not support 4 A hold), one per row 12 V output.
- 12 V → 3.3 V for master logic: **switching buck module
  (K7803-1000R3 / R-78E3.3-0.5 / V7803-500, SIP-3 drop-in for L78xx)**,
  fed Kelvin-style from the input rail (before per-row branches) so
  master logic stays stable during row inrush. **Not** a linear LDO —
  12 V→3.3 V at the master's ~150-250 mA load would dissipate ~1.7 W
  in a SOT-89 LDO (rated ~0.5 W), which would thermal-shutdown.

Master is the only point of power injection in the system. No row
bricks. No power distribution board.

**Firmware staggering — HARD REQUIREMENT.** 64 units × 240 mA peak =
15.36 A simultaneous-coil-energise current, which would nuisance-trip
the 15 A input fuse on a full-system reset. Master firmware MUST
stagger motor steps on power-up and during global commands so that
peak input current stays below 12 A (~50 % units active at once).
Flagged by Gemini external review 2026-04-26. Don't uprate the fuse —
the 4 A row polyfuses give per-row fault isolation that uprating the
input would defeat.

## ESP32-S3 module

- ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB PSRAM).
- USB: native CDC -> USB-C through USBLC6-2SC6 ESD.
- Reset/boot buttons.

## UART expander (4th UART)

- **SC16IS740IPW** single-channel UART-to-SPI bridge.
- TSSOP-16 package, ~EUR 2.
- Driven by ESP32-S3 SPI (e.g., SPI2_HOST).
- Crystal: 14.7456 MHz with two 18 pF load caps.
- IRQ to an ESP32-S3 GPIO.
- Drives Bus 3 (row 3) RS-485 transceiver.

## RS-485 paths (×4)

Four identical sub-circuits, one per bus. Each terminates into the same
4-pin output connector that carries 12 V + GND for that row.

Per bus:
- Transceiver: SN65HVD75DR (3.3 V, half-duplex).
- DE/RE controlled separately by GPIO (ESP32-S3 native or SC16IS740 GPIO).
- TX/RX from the corresponding UART.
- Termination: 120 ohm across A/B at master end.
- Failsafe bias: 1 k from A to 3V3, 1 k from B to GND.
- ESD: SM712-02HTG across A/B to GND.
- Output connector: **4-pin shrouded box header** (1×4, 2.54 mm, indexed).

## Output connector pinout (per row)

| Pin | Net | Notes |
|---|---|---|
| 1 | 12V | post-polyfuse |
| 2 | RS485_A | from local transceiver |
| 3 | RS485_B | from local transceiver |
| 4 | GND | |

Pin order is **12V / A / B / GND** (outer-inner-inner-outer) so 12 V
and GND are on opposite ends of the connector — adjacent-pin shorts
cannot directly bridge the supply rails. Same pinout on every 4-pin
row connector in the system (master rows, bus PCB ends, terminator
plug, daisy-chain cable).

4-pin shrouded box header at 2.54 mm pitch. The 12V pin carries up to
~3 A typical / 4 A peak per row — within standard pin rating since real-
world peaks are brief and steady-state is ~1-2 A.

## UART pin assignment (suggested)

| Function | Pin/IO |
|---|---|
| UART0 TX/RX/DE/RE | IO43 / IO44 / IO42 / IO41 |
| UART1 TX/RX/DE/RE | IO17 / IO18 / IO16 / IO15 |
| UART2 TX/RX/DE/RE | IO5 / IO6 / IO7 / IO4 |
| SPI MOSI/MISO/SCK/CS to SC16IS740 | IO11 / IO13 / IO12 / IO10 |
| SC16IS740 IRQ | IO9 |
| HEARTBEAT LED | IO48 |
| FAULT LED | IO47 |
| ROW0..ROW3 PWR LEDs | hardwired to post-polyfuse 12V via current-limited path |

ESP32-S3 GPIO matrix lets these be remapped.

## LEDs

- D1 PWR (green, hardwired to 3V3).
- D2 HEARTBEAT (blue, GPIO).
- D3 FAULT (red, GPIO).
- D4-D7 ROW0..ROW3 (4× green, hardwired to per-row post-polyfuse 12V).

## Test pads

- VIN, 3V3, GND.
- Each UART TX/RX/DE on a small breakout header.
- SPI bus to SC16IS740 broken out.

## PCB stack-up

- 2-layer HASL.
- Recommended outline: ~100 × 80 mm. Power section needs wide copper
  pours and the 4× 4-pin output connectors take edge real estate.
- Mounting: 4× M3 corners.

## Layout notes

- USB-C and 4× 4-pin row output connectors along the same long edge for
  cable routing.
- Screw terminal input on the opposite edge (or on the back) so the
  power cable doesn't fight the row cables.
- Wide 12 V rail copper from the input through the polyfuses to each
  row connector. Aim for >5 mm trace width or full-fill pour.
- Polyfuses placed close to their respective row connectors.
- TVS at the input within ~10 mm of the screw terminal.
- Master 3.3 V LDO input tapped Kelvin-style from the post-fuse rail
  (separate trace, not the high-current pour).
- Continuous GND pour between transceivers for crosstalk rejection.
- ESP32-S3 antenna keep-out per WROOM-1 datasheet.
- SC16IS740 close to the ESP32-S3 SPI pins; crystal close to SC16IS740.

## BOM

See `MASTER_BOM.csv`.
