# Master PCB

ESP32-S3 controller. Drives **4 RS-485 buses** (one per row). 64 units
total. USB-C native programming. Powered by its own small 12 V brick —
does not source power for the rows.

## Block diagram

```
12 V DC jack (small, ~5W)
   |
   +- F1 fuse 1 A
   +- TVS SMAJ15A
   +- Q1 P-FET reverse-block (AO3401)
   +- Cbulk 100 uF + 100 nF
   +- U1 LDO HT7833 12->3.3 V -> VCC_3V3
                                    |
                                    v
   ESP32-S3-WROOM-1-N16R8
       |
       +- USB-C (USBLC6 ESD, native CDC)
       +- BOOT button (IO0 -> GND)
       +- RESET button (EN -> GND)
       +- LEDs: PWR, HEARTBEAT, FAULT
       |
       +- UART1 -> SN65HVD75 #1 -> Bus 0 (row 0)
       +- UART2 -> SN65HVD75 #2 -> Bus 1 (row 1)
       +- UART0 -> SN65HVD75 #3 -> Bus 2 (row 2)
       |
       +- SPI -> SC16IS740 UART expander -> SN65HVD75 #4 -> Bus 3 (row 3)
                |
                +- 14.7456 MHz crystal
                +- IRQ -> ESP32-S3 GPIO

Each transceiver block:
   - DE/RE GPIOs (ESP32-S3 native; SC16IS740 GPIO for Bus 3)
   - 120 ohm termination at master end
   - 1k bias to 3V3 (A) + 1k bias to GND (B)
   - SM712 ESD across A/B
   - 3-pin output connector: A / B / GND
```

## Power

- Input: 12 V DC, 2.1 mm barrel jack, centre-positive.
- Recommended brick: 12 V / 1 A wall wart (master-only).
- **Master does not power the rows.** Each row has its own brick at the
  harness master-end.
- Reverse-polarity: P-FET (AO3401) in low-side return.
- Fuse: 1 A slow-blow.
- TVS: SMAJ15A.

## ESP32-S3 module

- ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB PSRAM).
- USB: native CDC -> USB-C through USBLC6-2SC6 ESD.
- Reset/boot buttons.

## UART expander (4th UART)

ESP32-S3 has 3 native UARTs. The 4th is provided by a **SC16IS740IPW**
single-channel UART-to-SPI bridge:

- TSSOP-16 package, ~EUR 2.
- Driven by ESP32-S3 SPI (e.g., SPI2_HOST on IO11/12/13/14).
- Crystal: 14.7456 MHz (allows clean 250 kbaud / 115200 / 921600
  derivation).
- IRQ to ESP32-S3 GPIO for RX-data interrupts.
- 64-byte FIFO per direction (more headroom than ESP32-S3 native UARTs).
- Drives Bus 3 (row 3) RS-485 transceiver same as the native buses.

## RS-485 paths (×4)

Four identical sub-circuits, one per bus.

Per bus:
- Transceiver: SN65HVD75DR (3.3 V, half-duplex).
- DE/RE controlled separately by GPIO (ESP32-S3 native or SC16IS740 GPIO).
- TX/RX from the corresponding UART.
- Termination: 120 ohm across A/B at master end. Populated.
- Failsafe bias: 1 k from A to 3V3, 1 k from B to GND. Populated.
- ESD: SM712-02HTG across A/B to GND.
- Output connector: 3-pin shrouded box header (2.54 mm, indexed).

Connector pinout per row port:
| Pin | Net |
|---|---|
| 1 | RS485_A |
| 2 | RS485_B |
| 3 | GND |

## UART pin assignment (suggested)

| Function | Pin/IO |
|---|---|
| UART0 TX | IO43 |
| UART0 RX | IO44 |
| UART0 DE/RE | IO42 / IO41 |
| UART1 TX | IO17 |
| UART1 RX | IO18 |
| UART1 DE/RE | IO16 / IO15 |
| UART2 TX | IO5 |
| UART2 RX | IO6 |
| UART2 DE/RE | IO7 / IO4 |
| SPI MOSI/MISO/SCK/CS to SC16IS740 | IO11 / IO13 / IO12 / IO10 |
| SC16IS740 IRQ | IO9 |
| HEARTBEAT LED | IO48 |
| FAULT LED | IO47 |

ESP32-S3 GPIO matrix lets these be remapped.

## LEDs

- D1 PWR (green, hardwired).
- D2 HEARTBEAT (blue, GPIO-driven).
- D3 FAULT (red, GPIO-driven).

## Test pads

- VIN, 3V3, GND.
- Each UART TX/RX/DE on a small breakout header.
- SPI bus to SC16IS740 broken out.

## PCB stack-up

- 2-layer HASL.
- Recommended outline: ~80 x 80 mm (slightly larger than 2-bus version
  due to 4 transceivers + UART expander). Final size pending freelancer
  layout.
- Mounting: 4x M3 corners.

## Layout notes

- USB-C, all 4 row output connectors, and the barrel jack along one
  long edge if possible.
- TVS as close to the barrel jack as possible.
- Each transceiver close to its termination resistor and output connector.
- Continuous GND pour between transceivers to reduce crosstalk.
- ESP32-S3 antenna keep-out per WROOM-1 datasheet.
- SC16IS740 close to the ESP32-S3 SPI pins; crystal close to SC16IS740.

## BOM

See `MASTER_BOM.csv`.
