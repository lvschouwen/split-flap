# Master PCB

ESP32-S3 controller. Drives **4 RS-485 buses** (one per row). 64 units
total. USB-C native programming. **Sources row power**: takes one 12 V /
15 A input and outputs 12V + GND + A + B per row on a single 6-pin
combined connector.

## Block diagram

```
12 V / 15 A DC input (screw terminal)
   |
   +- F1 fuse 15 A slow-blow (5x20 mm)
   +- TVS SMAJ15A
   +- Q1 P-FET reverse-block (AOD409 or NTD2955, DPAK)
   +- Cbulk 470 uF / 25 V + 100 nF
   |
   +-> VBUS_12V_RAIL (wide pour, 5+ mm trace)
   |        |
   |        +-> Per-row polyfuses (×4, 4 A hold, 1812 SMD)
   |        |        |
   |        |        +-> 12V_ROW0, 12V_ROW1, 12V_ROW2, 12V_ROW3
   |        |        +-> Each combined into the row's 6-pin output (pins 1+2 doubled)
   |        |        +-> Per-row PWR LED (post-polyfuse, indicates row 12V present)
   |        |
   +- U1 LDO HT7833 12->3.3 V -> VCC_3V3 (master logic)
                                    |
                                    v
   ESP32-S3-WROOM-1-N16R8
       |
       +- USB-C (USBLC6 ESD, native CDC)
       +- BOOT button (IO0 -> GND)
       +- RESET button (EN -> GND)
       +- LEDs: PWR, HEARTBEAT, FAULT, ROW0..ROW3 (7 total)
       |
       +- UART1 -> SN65HVD75 #1 -> Bus 0 (row 0)
       +- UART2 -> SN65HVD75 #2 -> Bus 1 (row 1)
       +- UART0 -> SN65HVD75 #3 -> Bus 2 (row 2)
       +- SPI -> SC16IS740 -> SN65HVD75 #4 -> Bus 3 (row 3)
                |
                +- 14.7456 MHz crystal
                +- IRQ -> ESP32-S3 GPIO

Per-row 4-pin output (combined power + signal):
  Pin 1: 12V (post-polyfuse)
  Pin 2: GND
  Pin 3: RS485_A
  Pin 4: RS485_B

Each transceiver block:
   - DE/RE GPIOs (ESP32-S3 native; SC16IS740 GPIO for Bus 3)
   - 120 ohm termination at master end (across A/B)
   - 1k bias to 3V3 (A) + 1k bias to GND (B)
   - SM712 ESD across A/B
   - A/B routed to the 6-pin row output connector
```

## Power

- **Input: 12 V / 15 A from a single SMPS brick.** Sized for 64-unit
  worst-case peak (~16 A briefly during simultaneous flap transitions);
  steady-state draw is ~3-5 A.
- Input connector: **2-pin Phoenix-style screw terminal** (5 mm pitch).
  Robust, accepts bare-wire or terminated cable from any 12 V brick.
- Reverse-polarity: P-FET (AOD409 or NTD2955 in DPAK) handles 15 A
  continuous comfortably.
- Input fuse: 15 A slow-blow, 5×20 mm holder.
- TVS: SMAJ15A across input.
- Bulk: 470 uF / 25 V + 100 nF for inrush smoothing.
- Per-row protection: 4× polyfuses (4 A hold / 8 A trip, e.g.,
  Bourns MF-MSMF400-2), one per row 12 V output.
- 12 V → 3.3 V for master logic: HT7833 LDO, fed Kelvin-style from the
  input rail (before per-row branches) so master logic stays stable
  during row inrush.

Master is the only point of power injection in the system. No row
bricks. No power distribution board.

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
6-pin output connector that carries 12 V + GND for that row.

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
| 2 | GND | |
| 3 | RS485_A | from local transceiver |
| 4 | RS485_B | from local transceiver |

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
  pours and the 4× 6-pin output connectors take edge real estate.
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
