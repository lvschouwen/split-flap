# Master PCB — Schematic + Layout Specification

EasyEDA-ready spec for the master PCB. **1 of these per system.** Most
complex board — ESP32-S3 module, 4× RS-485 paths, SC16IS740 UART
expander, 15 A power input.

## Component list (with LCSC starting points)

| Ref | Value | Footprint | LCSC# (CHECK) | Notes |
|---|---|---|---|---|
| U1 | ESP32-S3-WROOM-1-N16R8 | SMD module 18×25.5 mm | C2913203 | 16 MB flash, 8 MB PSRAM, native USB |
| U2 | HT7833 | SOT-89-5 | C70358 | 12V→3.3V LDO 500 mA, master logic only |
| U3 | SC16IS740IPW | TSSOP-16 | C9099 | Single-channel UART-to-SPI bridge |
| U4 | SN65HVD75DR | SOIC-8 | C57928 | Bus 0 RS-485 PHY |
| U5 | SN65HVD75DR | SOIC-8 | C57928 | Bus 1 RS-485 PHY |
| U6 | SN65HVD75DR | SOIC-8 | C57928 | Bus 2 RS-485 PHY |
| U7 | SN65HVD75DR | SOIC-8 | C57928 | Bus 3 RS-485 PHY (driven by SC16IS740) |
| U8 | USBLC6-2SC6 | SOT-23-6 | C7519 | USB-C ESD |
| Y1 | 14.7456 MHz crystal | SMD 3.2×2.5 | C12674 | SC16IS740 reference clock |
| Q1 | AOD409 (or NTD2955) | DPAK / TO-252 | C160005 (AOD409) | P-FET reverse-block, 15 A continuous |
| F1 | 15 A fuse 5×20 mm + holder | THT | n/a (off-shelf) | Master input fuse, slow-blow |
| F_row1 | Polyfuse 4 A hold | 1812 SMD | C162294 | Bourns MF-MSMF400-2 row 0 |
| F_row2 | same | 1812 | C162294 | row 1 |
| F_row3 | same | 1812 | C162294 | row 2 |
| F_row4 | same | 1812 | C162294 | row 3 |
| D1 | LED green | 0805 | C72043 | PWR indicator |
| D2 | LED blue | 0805 | C2293 | HEARTBEAT |
| D3 | LED red | 0805 | C2286 | FAULT |
| D4-D7 | LED green | 0805 | C72043 | ROW0..ROW3 power indicators |
| D8 | SMAJ15A | DO-214AC SMA | C167238 | Input TVS |
| D9-D12 | SM712-02HTG | SOT-23 | C172881 | RS-485 ESD per bus |
| J1 | Phoenix MKDS 1.5/2 | 5 mm pitch THT | C18643 | 12V/15A input screw terminal |
| J2 | USB-C 16-pin SMD | TYPE-C-31-M-12 | C165948 | USB receptacle, shielded |
| J3-J6 | 4-pin shrouded box header | 1×4 2.54 mm | C124378 | Row outputs |
| J7 | 2×4 pin header | 2.54 mm | C124378 (or similar) | UART/SPI debug breakout |
| SW1 | Tact switch 6×6 | SMD | C318884 | BOOT (IO0) |
| SW2 | Tact switch 6×6 | SMD | C318884 | RST (EN) |
| C_bulk | 470 µF / 25 V | Radial THT 8×11 mm | C107821 | Input bulk |
| C_in | 22 µF / 25 V X7R | 0805 | C45783 | LDO input |
| C_out | 10 µF / 10 V X7R | 0805 (×2) | C15850 | LDO output |
| C_decap_esp | 100 nF X7R | 0603 (×8) | C14663 | ESP32-S3 module decap |
| C_decap_uart | 100 nF X7R | 0603 (×2) | C14663 | SC16IS740 decap |
| C_xtal | 18 pF NP0 | 0603 (×2) | C36760 | Crystal load caps |
| C_usb | 1 µF / 10 V X7R | 0603 (×2) | C15849 | USB rail decap |
| C_row | 10 µF / 25 V X7R | 0805 (×4) | C19702 | Per-row output bulk (post-polyfuse) |
| R_term | 120 Ω 1% | 0805 (×4) | C22787 | RS-485 termination per bus |
| R_bias_a | 1 kΩ 1% | 0805 (×4) | C17513 | A bias to 3V3 per bus |
| R_bias_b | 1 kΩ 1% | 0805 (×4) | C17513 | B bias to GND per bus |
| R_led | 1 kΩ 1% | 0603 (×3) | C21190 | PWR/HB/FAULT LED current limit |
| R_led_row | 4.7 kΩ 1% | 0603 (×4) | C25879 | Row LED current limit (12V) |
| R_cc | 5.1 kΩ 1% | 0603 (×2) | C25905 | USB-C CC pull-downs |
| R_strap | 10 kΩ 1% | 0603 (×4) | C25804 | ESP32-S3 EN pull-up + strap pulls |
| R_uart_strap | 10 kΩ 1% | 0603 (×4) | C25804 | SC16IS740 IRQ + reset pulls |

## High-level schematic blocks

```
                 +─────────+
   12V input ── │ Power   │── 3V3 ── ESP32-S3 ── UART0 ── PHY3 ── J5 (Bus 2)
                │  in     │── 12V   (USB-C   ── UART1 ── PHY1 ── J3 (Bus 0)
                │ Q1, F1, │   row    debug,  ── UART2 ── PHY2 ── J4 (Bus 1)
                │ TVS,    │   poly-  IO map) ── SPI    ── SC16  ── PHY4 ── J6 (Bus 3)
                │ Cbulk   │   fuses)            (4th UART expander)
                │ HT7833  │
                +─────────+
```

## Power section

```
J1 pin 1 (+) ──┬── F1 (15 A slow-blow fuse holder + fuse)
                │
                └── D8 SMAJ15A anode (cathode to GND)
                │
                └── Q1 source (AOD409 P-FET reverse-block)
                    
Q1:
  source ← incoming 12V (post-fuse, post-TVS)
  drain  → PCB-12V_RAIL
  gate   ← GND via 10 kΩ pull-down + body diode pulls source through

C_bulk 470 µF (post-Q1 drain) ─── PCB-12V_RAIL
   |
   ├── HT7833 input (U2 pin 1 + C_in 22 µF)
   ├── F_row1 polyfuse → ROW0_12V → C_row 10 µF → J3 pin 1 + D4 row PWR LED
   ├── F_row2 polyfuse → ROW1_12V → C_row 10 µF → J4 pin 1 + D5 row PWR LED
   ├── F_row3 polyfuse → ROW2_12V → C_row 10 µF → J5 pin 1 + D6 row PWR LED
   └── F_row4 polyfuse → ROW3_12V → C_row 10 µF → J6 pin 1 + D7 row PWR LED

J1 pin 2 (-) ── PCB-GND plane

HT7833:
  pin 1 VIN ← PCB-12V_RAIL
  pin 2 GND → GND
  pin 3 NC
  pin 4 GND → GND
  pin 5 VOUT → 3V3 + C_out 10 µF + decoupling

Per-row PWR LED (×4):
   ROW_n_12V ── R_led_row 4.7 kΩ ── D_n anode ── D_n cathode ── GND
```

## ESP32-S3 module (U1)

Wire per Espressif's WROOM-1 reference design. Key connections:

| Module pin | Net | Notes |
|---|---|---|
| 1, 16, 25, 26, 39, 42 (and pad) | GND | All GND pins tied to plane |
| 2 | 3V3 | + 100 nF + 22 µF decap close to module |
| 3 | EN | 10 kΩ pull-up to 3V3 + SW2 (RST button to GND) + 1 nF cap to GND |
| 4 (IO4) | UART2_/RE | → U6 SN65HVD75 pin 2 (/RE) via 1 kΩ |
| 5 (IO5) | UART2_TX | → U6 SN65HVD75 pin 4 (D) |
| 6 (IO6) | UART2_RX | ← U6 SN65HVD75 pin 1 (R) |
| 7 (IO7) | UART2_DE | → U6 SN65HVD75 pin 3 (DE) via 1 kΩ |
| 8 (IO15) | UART1_/RE | → U5 SN65HVD75 pin 2 |
| 9 (IO16) | UART1_DE | → U5 SN65HVD75 pin 3 |
| 10 (IO17) | UART1_TX | → U5 SN65HVD75 pin 4 |
| 11 (IO18) | UART1_RX | ← U5 SN65HVD75 pin 1 |
| 13 (IO19) | USB_DM | → U8 USBLC6 + USB-C D- |
| 14 (IO20) | USB_DP | → U8 USBLC6 + USB-C D+ |
| 17 (IO46) | strap | leave default (pull-down to 0 for normal boot) |
| 18 (IO9) | SC16IS740_IRQ | ← U3 IRQ pin (open-drain, 10 kΩ pull-up to 3V3) |
| 19 (IO10) | SC16IS740_CS | → U3 CS pin |
| 20 (IO11) | SC16IS740_MOSI | → U3 SI pin |
| 21 (IO12) | SC16IS740_SCK | → U3 SCK pin |
| 22 (IO13) | SC16IS740_MISO | ← U3 SO pin |
| 24 (IO21) | (spare/test) | NC |
| 37 (IO47) | LED_FAULT | → D3 cathode (anode → 3V3 via R_led 1 kΩ) |
| 38 (IO48) | LED_HEARTBEAT | → D2 cathode (anode → 3V3 via R_led 1 kΩ) |
| 40 (IO45) | strap | leave default per WROOM-1 datasheet (sets VDD_SPI level — module-internal) |
| 41 (IO0) | BOOT | 10 kΩ pull-up to 3V3 + SW1 (BOOT button to GND) |

UART0 (free pins on the ESP32-S3 GPIO matrix — IO43, IO44 by default
for boot UART but can be remapped):

| Net | Default GPIO |
|---|---|
| UART0_TX | IO43 |
| UART0_RX | IO44 |
| UART0_DE | IO42 |
| UART0_/RE | IO41 |

UART0 connects to U4 SN65HVD75 (Bus 0). If IO43/44 used for boot
console, can be remapped to other GPIOs at boot via firmware.

## USB-C (J2 + U8 USBLC6-2SC6)

```
J2 (USB-C receptacle):
  VBUS (pins A4, A9, B4, B9) ── connected to a 5V test pad (not used
                                  for power; ESP32-S3 native USB does
                                  not draw bus power)
  GND  (pins A1, A12, B1, B12) ── GND plane
  D+   (pin A6, B6) ── USB_DP (paralleled with B6 if Type-C)
  D-   (pin A7, B7) ── USB_DM
  CC1  (pin A5)     ── R_cc 5.1 kΩ ── GND   (device-side pull-down)
  CC2  (pin B5)     ── R_cc 5.1 kΩ ── GND
  Shield ── chassis GND

U8 USBLC6-2SC6 (ESD across D+/D-):
  pin 1 (I/O1)   ── USB_DM
  pin 2 (GND)    ── GND
  pin 3 (I/O2)   ── USB_DP
  pin 4 (I/O2)   ── USB_DP (yes, repeated, 2 pins for differential)
  pin 5 (VBUS)   ── 5V (or 3V3, see datasheet)
  pin 6 (I/O1)   ── USB_DM

ESP32-S3 IO19/IO20 ──┬─ U8 ── USB-C D+/D-
                       └─── (no series R needed for native USB)

Add 1 µF decap on the VBUS rail at U8.
```

## SC16IS740 (U3) UART expander

```
U3 SC16IS740IPW (TSSOP-16):
  pin 1 (Vsupply)  ── 3V3 + 100 nF
  pin 2 (NC)       ── NC
  pin 3 (XTAL1)    ── Y1 + 18 pF C_xtal to GND
  pin 4 (XTAL2)    ── Y1 + 18 pF C_xtal to GND
  pin 5 (RESET)    ── 10 kΩ R_uart_strap pull-up to 3V3
  pin 6 (CS)       ← ESP32-S3 IO10
  pin 7 (SI/MOSI)  ← ESP32-S3 IO11
  pin 8 (SO/MISO)  → ESP32-S3 IO13
  pin 9 (SCLK)     ← ESP32-S3 IO12
  pin 10 (IRQ)     → ESP32-S3 IO9 (open-drain, 10 kΩ pull-up)
  pin 11 (RX)      ← U7 SN65HVD75 pin 1 (R)
  pin 12 (TX)      → U7 SN65HVD75 pin 4 (D)
  pin 13 (CTS)     ── 10 kΩ R_uart_strap pull-up (unused, tie inactive)
  pin 14 (RTS)     ── (unused, leave NC or use as DE for U7)
  pin 15 (GND)     ── GND
  pin 16 (Vdd_io)  ── 3V3 + 100 nF
```

For the 4th RS-485 path (Bus 3) DE control: SC16IS740's RTS pin can
be configured as auto-RS-485 direction (firmware-set bit). Wire RTS
(pin 14) to U7 SN65HVD75 DE+/RE pins:

```
U7 (SN65HVD75 Bus 3):
  pin 1 (R)  → SC16IS740 RX (pin 11)
  pin 2 (/RE) ── SC16IS740 RTS (pin 14)  -- inverted for /RE
  pin 3 (DE)  ── SC16IS740 RTS (pin 14)
  pin 4 (D)  ← SC16IS740 TX (pin 12)
```

(If split DE/RE control is preferred, use a separate ESP32-S3 GPIO
for /RE and tie DE to RTS only. Or invert RTS via a tiny logic gate
or transistor.)

## RS-485 paths (×4 identical sub-circuits)

Per bus (replace `n` with 0, 1, 2, 3):

```
Master MCU/UART_n ── SN65HVD75_Un (transceiver):
  pin 1 (R)   → UART_n RX
  pin 2 (/RE) ← UART_n /RE (via 1 kΩ R_re)
  pin 3 (DE)  ← UART_n DE  (via 1 kΩ R_de)
  pin 4 (D)   ← UART_n TX
  pin 5 (GND) ── GND
  pin 6 (A)   → RS485_An ── R_term 120 Ω across A/B + R_bias_a 1 kΩ to 3V3 + SM712 + J3/4/5/6 pin 3
  pin 7 (B)   → RS485_Bn ── R_term 120 Ω across A/B + R_bias_b 1 kΩ to GND + SM712 + J3/4/5/6 pin 4
  pin 8 (Vcc) ── 3V3 + 100 nF decap

Output connector J3/4/5/6 (Row n):
  pin 1: ROW_n_12V (post-polyfuse F_rown)
  pin 2: GND
  pin 3: RS485_An
  pin 4: RS485_Bn
```

SM712-02HTG (4× ESD arrays D9-D12), one per bus:

```
SM712 pin 1 ── RS485_An
SM712 pin 2 ── GND
SM712 pin 3 ── RS485_Bn
```

## LEDs (3 master-status, hardwired-12V row indicators)

```
3V3 ── R_led 1 kΩ ── D1 anode ── D1 cathode ── GND  (PWR, hardwired-on when 3V3 alive)
3V3 ── R_led 1 kΩ ── D2 anode ── D2 cathode ── ESP32-S3 IO48  (HEARTBEAT, sinks)
3V3 ── R_led 1 kΩ ── D3 anode ── D3 cathode ── ESP32-S3 IO47  (FAULT, sinks)

ROW_n_12V ── R_led_row 4.7 kΩ ── D_n+3 anode ── D_n+3 cathode ── GND  (row power-OK, hardwired)
```

## Boot/Reset buttons

```
SW1 (BOOT button):
  pin A ── ESP32-S3 IO0 (with 10 kΩ pull-up to 3V3)
  pin B ── GND

SW2 (RST button):
  pin A ── ESP32-S3 EN (with 10 kΩ pull-up to 3V3 + 1 nF to GND)
  pin B ── GND
```

## Floorplan (rough placement)

```
                     ~100 mm
   +─────────────────────────────────────────────+
   │ J1 screw    USB-C J2     SW1     SW2        │ <-- TOP edge
   │ terminal              BOOT    RST        ... │
   │ 12V/15A                                      │
   │                                              │
   │  D8 TVS                                      │
   │  Q1 P-FET   F1 fuse                          │
   │  C_bulk                                      │
   │  HT7833 U2     U8 USBLC                      │
   │                                              │
   │  ┌─────────────────────────┐                  │ ~80 mm
   │  │ U1 ESP32-S3-WROOM-1     │                  │
   │  │   antenna keep-out →     │                  │
   │  │                          │                  │
   │  └─────────────────────────┘                  │
   │                                                │
   │  U3 SC16IS740   Y1 crystal                    │
   │                                                │
   │  U4 PHY0  U5 PHY1  U6 PHY2  U7 PHY3            │
   │  D9       D10      D11      D12  (ESDs)         │
   │  R_term0  R_term1  R_term2  R_term3            │
   │  bias0    bias1    bias2    bias3              │
   │  F_row0   F_row1   F_row2   F_row3 (polyfuses) │
   │  D4       D5       D6       D7   (row LEDs)    │
   │                                                │
   │  J3       J4       J5       J6                 │ <-- BOTTOM edge
   │  Bus 0    Bus 1    Bus 2    Bus 3              │ (4-pin shrouded
   │  out      out      out      out                │  row outputs)
   │                                                │
   +─────────────────────────────────────────────+
   M-holes at 4 corners
```

Layout principles:
- Power input (J1, F1, Q1, TVS, bulk) along the top edge.
- ESP32-S3 module roughly centred. Antenna keep-out per WROOM-1
  datasheet (no copper, components, or traces under the antenna area).
- 4 RS-485 PHYs in a row, each near its corresponding output
  connector at the bottom edge.
- Per-row polyfuses near the master power section, distributing 12V
  through wide traces to each output connector.
- USB-C and BOOT/RST buttons on the top edge for user access.
- HT7833 LDO Kelvin-tap from the input rail (separate trace before
  per-row branches) to keep master logic 3V3 stable during row inrush.

## Power trace widths

| Net | Width | Reason |
|---|---|---|
| J1 → Q1 → C_bulk (12V input) | 60 mil (1.5 mm) or wider pour | 15 A capacity |
| C_bulk → polyfuses | 40 mil (1 mm) or pour | ~5 A per row trunk |
| polyfuse → row output | 30 mil (0.75 mm) | 4 A per row |
| 3V3 to ESP32-S3 / SC16IS740 / PHYs | 15 mil (0.4 mm) | <500 mA |
| Signal traces | 6 mil (0.15 mm) standard | low current |

Use **GND pour on both layers** to give a solid return plane and
crosstalk shielding.

## Fab parameters

| Parameter | Value |
|---|---|
| Outline | ~100 × 80 mm |
| Layer count | 2 |
| Substrate | FR-4 |
| Thickness | 1.6 mm |
| Copper weight | 1 oz |
| Surface finish | HASL (cheaper) |
| Mounting holes | 4× M3 clearance at corners |

## EasyEDA build steps

1. Create new EasyEDA Pro project: `splitflap-master-v2`.
2. Schematic editor:
   - Place all components from LCSC numbers.
   - Wire per the section-by-section nets above. Use net labels
     liberally — `12V`, `GND`, `3V3`, `ROW0_12V`...`ROW3_12V`,
     `RS485_A0`...`RS485_A3`, `RS485_B0`...`RS485_B3`, `UART0_TX`,
     etc.
   - Group decoupling caps near their ICs.
3. ERC: fix any unconnected pins.
4. Convert to PCB.
5. PCB editor:
   - Set outline 100 × 80 mm.
   - Place per the floorplan.
   - Route 12V power per the trace-width table.
   - Add solid GND pour on both layers.
   - Place WROOM-1 antenna keep-out (rectangle on top layer +
     forbidden region under it).
6. Run DRC.
7. Order: 5 boards from JLC.

## Known design risks (verify in EasyEDA)

1. **WROOM-1 footprint orientation**: EasyEDA's WROOM-1 footprint has
   a specific orientation; verify pin numbering matches the wiring
   table.
2. **SC16IS740 RTS-as-DE-control**: Some firmware drivers handle
   auto-DE on RTS; verify in the SC16IS740 datasheet which mode you
   intend. Default behaviour is GPIO; auto-RS485 is a register bit.
3. **USB-C VBUS not connected to 5V buck**: Master is powered from
   J1, not USB. Don't accidentally wire VBUS to 5V_RAIL.
4. **AOD409 P-FET orientation**: Source goes to incoming 12V (post-
   fuse), drain to PCB-12V_RAIL. Body diode conducts at startup, gate
   pulls down through the existing path. Verify in EasyEDA's symbol.

## Verification checklist

- [ ] All LCSC parts verified in catalogue.
- [ ] WROOM-1 antenna keep-out present (no copper or components
      within the keep-out rectangle).
- [ ] EN pin (chip enable) has 10 kΩ pull-up + 1 nF cap (per
      Espressif reference).
- [ ] BOOT button pulls IO0 to GND when pressed.
- [ ] USB-C CC1 and CC2 each have 5.1 kΩ to GND.
- [ ] All 4 polyfuses sized 4 A hold (Bourns MF-MSMF400 or equivalent).
- [ ] Row output connectors all 1×4 shrouded box headers, indexed.
- [ ] SC16IS740 crystal load caps 18 pF NP0.
- [ ] ESD arrays present on all 4 bus pairs.
- [ ] Termination + bias resistors all present at master end of every bus.
- [ ] DRC passes.
