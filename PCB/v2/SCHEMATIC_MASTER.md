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

Wire per Espressif's WROOM-1 datasheet (Table 3-1 "Pin Definitions" of
the ESP32-S3-WROOM-1 datasheet v1.6). Module is 41-pin + EPAD.

**Pin map (verified against datasheet, 2026-04-25):**

| Module pin | Signal | Net | Notes |
|---|---|---|---|
| 1, 40, 41, EPAD | GND | GND | All GND pads tied to plane (only these 4 — earlier doc revisions over-counted) |
| 2 | 3V3 | 3V3 | + 100 nF + 22 µF decap close to module |
| 3 | EN | EN | 10 kΩ pull-up to 3V3 + SW2 (RST button to GND) + 1 nF cap to GND |
| 4 | IO4 | UART2_/RE | → U6 SN65HVD75 pin 2 (/RE) via 1 kΩ |
| 5 | IO5 | UART2_TX | → U6 SN65HVD75 pin 4 (D) |
| 6 | IO6 | UART2_RX | ← U6 SN65HVD75 pin 1 (R) |
| 7 | IO7 | UART2_DE | → U6 SN65HVD75 pin 3 (DE) via 1 kΩ |
| 8 | IO15 | UART1_/RE | → U5 SN65HVD75 pin 2 |
| 9 | IO16 | UART1_DE | → U5 SN65HVD75 pin 3 |
| 10 | IO17 | UART1_TX | → U5 SN65HVD75 pin 4 |
| 11 | IO18 | UART1_RX | ← U5 SN65HVD75 pin 1 |
| 12 | IO8 | (spare) | NC |
| 13 | IO19 | USB_DM | → U8 USBLC6 + USB-C D- |
| 14 | IO20 | USB_DP | → U8 USBLC6 + USB-C D+ |
| 15 | IO3 | strap | **Strapping pin** — add 10 kΩ pull-up to 3V3 to keep USB-JTAG functional by default |
| 16 | IO46 | strap | **Strapping pin — must be LOW at boot.** Add explicit external 10 kΩ pull-down to GND on this pin. Internal pull-down is not always reliable across resets. |
| 17 | IO9 | SC16IS740_IRQ | ← U3 IRQ pin (open-drain, 10 kΩ pull-up to 3V3) |
| 18 | IO10 | SC16IS740_CS | → U3 CS pin |
| 19 | IO11 | SC16IS740_MOSI | → U3 SI pin |
| 20 | IO12 | SC16IS740_SCK | → U3 SCK pin |
| 21 | IO13 | SC16IS740_MISO | ← U3 SO pin |
| 22 | IO14 | (spare) | NC |
| 23 | IO21 | (spare/test) | NC |
| 24 | IO47 | LED_FAULT | → D3 cathode (anode → 3V3 via R_led 1 kΩ) |
| 25 | IO48 | LED_HEARTBEAT | → D2 cathode (anode → 3V3 via R_led 1 kΩ) |
| 26 | IO45 | strap | Leave default per WROOM-1 datasheet (sets VDD_SPI level — module-internal). For N16R8 the module-internal circuitry pulls IO45 to a defined state. |
| 27 | IO0 | BOOT | **Strapping pin — must be HIGH at boot.** 10 kΩ pull-up to 3V3 + SW1 (BOOT button to GND). |
| 28-35 | IO35-IO42 | (spare) | NC (IO35-IO37 are used internally for SPI flash/PSRAM on N16R8 — do not connect externally) |
| 36 | RXD0 (IO44) | UART0_RX | → U4 SN65HVD75 pin 1 (R) |
| 37 | TXD0 (IO43) | UART0_TX | → U4 SN65HVD75 pin 4 (D) |
| 38 | IO2 | UART0_/RE | → U4 SN65HVD75 pin 2 (/RE) via 1 kΩ |
| 39 | IO1 | UART0_DE | → U4 SN65HVD75 pin 3 (DE) via 1 kΩ |

**UART0 path (Bus 0)**: TXD0/RXD0 share the boot console UART. After
boot the firmware can either keep them as the application UART (and
either disable boot console output or accept its presence on the
RS-485 bus during boot) or remap UART0 to other GPIOs via the GPIO
matrix. Recommended layout: route IO43/IO44 to U4 transceiver and
provide test pads so a USB-serial dongle can also tap UART0 during
bring-up.

**Strap pin summary** (per WROOM-1 datasheet Table 2-4):

| Pin | Required boot state | Implementation |
|---|---|---|
| IO0 | HIGH (SPI boot) | 10 kΩ pull-up + BOOT button to GND |
| IO3 | HIGH (USB-JTAG enabled) | 10 kΩ pull-up to 3V3 |
| IO45 | module-internal | leave default (no external pull) |
| IO46 | LOW | 10 kΩ pull-down to GND (explicit, do not rely on internal) |

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

**⚠️ Verify pin numbering directly against NXP datasheet Figure 5 +
Table 4** (`SC16IS740_750_760.pdf`). The TSSOP-16 IPW pinout was not
extractable as plain text during spec authoring. Confirmed facts (do
not deviate):

- **Pin 1 = VDD (Vsupply, 3V3 main supply)**
- **Pin 5 = XTAL1, Pin 6 = XTAL2** (NOT pins 3/4 as some early drafts had)
- **Pin 15 = VSS (GND)**
- **Pin 16 = VDD_io (3V3 logic-level supply)**
- The four SPI signals (CS, SI/MOSI, SO/MISO, SCLK) are 4 consecutive
  pins; IRQ is one pin; RX/TX/CTS/RTS are 4 consecutive pins; RESET
  is near the XTAL pins. Pull these from Figure 5.

**Connections (by signal, pin numbers per datasheet Table 4):**

| Signal | Direction | Net |
|---|---|---|
| VDD (pin 1) | in | 3V3 + 100 nF decap |
| XTAL1 (pin 5) | in | Y1 14.7456 MHz + 18 pF C_xtal to GND |
| XTAL2 (pin 6) | out | Y1 14.7456 MHz + 18 pF C_xtal to GND |
| VSS (pin 15) | gnd | GND |
| VDD_io (pin 16) | in | 3V3 + 100 nF decap |
| RESET (active-low) | in | 10 kΩ R_uart_strap pull-up to 3V3 (never pulled low after boot) |
| CS | in | ← ESP32-S3 IO10 |
| SI (MOSI) | in | ← ESP32-S3 IO11 |
| SO (MISO) | out | → ESP32-S3 IO13 |
| SCLK | in | ← ESP32-S3 IO12 |
| IRQ (open-drain, active-low) | out | → ESP32-S3 IO9 with 10 kΩ pull-up to 3V3 |
| RX | in | ← U7 SN65HVD75 pin 1 (R) |
| TX | out | → U7 SN65HVD75 pin 4 (D) |
| CTS | in | tie to GND (unused — auto-flow disabled in firmware so polarity is don't-care, GND keeps it inactive on both polarities) |
| RTS | out | → U7 DE pin (pin 3); see auto-DE wiring below |

### Bus 3 transceiver (U7) DE/RE wiring — design-correct

Earlier drafts wired RTS to **both** DE and /RE simultaneously. This
is **incorrect**: DE is active-high and /RE is active-low — one signal
cannot drive both correctly without an inverter. Corrected wiring:

```
U7 (SN65HVD75, Bus 3):
  pin 1 (R)   → SC16IS740 RX (auto-routed via firmware)
  pin 2 (/RE) ── tie permanently to GND (always-receive mode)
  pin 3 (DE)  ← SC16IS740 RTS (auto-driven)
  pin 4 (D)   ← SC16IS740 TX
  pin 5 (GND), 6 (A), 7 (B), 8 (Vcc) — see RS-485 path section
```

`/RE` tied low means U7's receiver is **always enabled**. While master
is transmitting on Bus 3, the master's own UART will hear its own TX
echo on the RX line — the firmware MUST discard echoed bytes during
TX windows. This is standard half-duplex RS-485 practice and avoids
the inverter / extra GPIO that the original wiring would have needed.

### Firmware setup for auto-DE on RTS

The SC16IS740 implements auto-RS-485 direction control via the
**EFCR (Extra Features Control Register)**:
- **EFCR bit 4 = 1** → enable auto-RS-485 RTS control. Transmitter
  asserts RTS while the TX FIFO has data and de-asserts after the
  last stop bit shifts out.
- **EFCR bit 5 = 1** → invert RTS polarity so DE goes active-high
  during TX (which matches the SN65HVD75 DE polarity).
- **EFR bits 6:7 = 0** → disable hardware flow control (otherwise
  RTS is repurposed and auto-DE doesn't work).

These are register-level firmware settings; no hardware change. They
are listed here so the schematic + firmware stay coherent.

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
  datasheet section 7.2: **18.6 mm × 5 mm rectangle** under the
  antenna area extending past the module's PCB edge. No copper,
  components, traces, or vias inside this rectangle on either layer.
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
- [ ] WROOM-1 antenna keep-out present: **18.6 × 5 mm rectangle**
      extending past the module edge with no copper, components,
      traces, or vias on either layer.
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
