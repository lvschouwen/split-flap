# Master v2 — textual schematic

**Rev B.** Authoritative netlist. Layout engineer recaptures in target EDA (KiCad 8 or EasyEDA) from this file + BOM.csv.

Conventions:
- Net names in `UPPER_SNAKE_CASE`.
- Designators match [BOM.csv](./BOM.csv).
- "→" means "connects to".
- Passive values inline.
- Every pin of every IC is defined. Every net has a name. No "verify later" / "approximate" / "representative" language.

---

## 1. USB-C input (J1, 16-pin receptacle, USB 2.0)

| USB-C pin(s) | Net | Notes |
|---|---|---|
| A4, A9, B4, B9 | **USB_VBUS** | VBUS, tied internally |
| A1, A12, B1, B12, shield | **GND** | including cable shield |
| A5 | **USB_CC1** | 5.1 kΩ R_CC1 → GND |
| B5 | **USB_CC2** | 5.1 kΩ R_CC2 → GND |
| A6, B6 | **USB_DP_RAW** | to USBLC6 U1 |
| A7, B7 | **USB_DN_RAW** | to USBLC6 U1 |
| A2, A3, A10, A11, B2, B3, B10, B11 | NC | USB 2.0 only, SuperSpeed pairs not used |

- USB_VBUS → ferrite bead **FB1** (BLM18PG471SN1D, 470 Ω @ 100 MHz) → **USB_5V_IN**
- Cable shield bonded to GND through 1 MΩ **R_SH1** in parallel with 10 nF **C_SH1**.

## 2. USB ESD protection — U1 USBLC6-2SC6 (SOT-23-6)

| Pin | Signal | Net |
|---|---|---|
| 1 | I/O 1 | **USB_DN_RAW ↔ USB_DN** (tied to pin 6) |
| 2 | GND | **GND** |
| 3 | I/O 2 | **USB_DP_RAW ↔ USB_DP** (tied to pin 4) |
| 4 | I/O 2 | tied to pin 3 — **NOT GND** |
| 5 | VBUS | **USB_5V_IN** |
| 6 | I/O 1 | tied to pin 1 |

- USB_DN → ESP32-S3 GPIO19
- USB_DP → ESP32-S3 GPIO20

## 3. Barrel jack (J2, DC-005 5.5/2.1 mm, center-positive)

- Center pin → **JACK_IN_RAW**
- Sleeve → **GND**

### Reverse polarity — Q1 AO3401A PMOS (SOT-23-3)
- S (pin 3) → **JACK_IN_RAW**
- G (pin 1) → **GND** via 10 kΩ **R_QGATE**
- D (pin 2) → **JACK_5V**

### TVS — D1 SMAJ5.0CA (DO-214AC)
- Between **JACK_5V** and **GND**, bidirectional, 5 V working, 9.2 V clamp.

## 4. Power path (dual-rail split)

Two independent use cases:

1. **USB input** — low current (≤ 1.5 A), powers logic only.
2. **Barrel jack input** — high current (up to 8 A system load), powers row 5 V directly plus a low-current backfeed for logic.

Two distinct power nets:

- **5V_LOGIC** — buck converter input + any low-current 5 V loads.
- **5V_RAIL** — row polyfuses + per-row 5 V distribution.

### USB path (protected, limited current) — U2 LM66100DCKR (SOT-23-5)

| Pin | Name | Net |
|---|---|---|
| 1 | IN | **USB_5V_IN** |
| 2 | GND | **GND** |
| 3 | ST (status) | NC |
| 4 | /CE (enable, active low) | pulled to IN via 100 kΩ **R_CE_USB** (always enabled) |
| 5 | OUT | **5V_LOGIC** |

Purpose: safe OR-ing for USB-powered bench operation, ~1.5 A max. Does not feed row power.

### Barrel jack path (HIGH CURRENT, BYPASSES LM66100)

- **JACK_5V** (post Q1, D1) → **5V_RAIL** directly through polyfuse distribution only. No LM66100 in this path.
- Polyfuses F1..F8 (§10) break 5V_RAIL into per-row segments.

**CRITICAL**: JACK_5V MUST NOT pass through any LM66100. Current up to 8 A destroys the device.

### Logic backfeed from jack (low current)

- **D_BACKFEED** Schottky (SS14, 40 V, 1 A, V_F ≤ 0.4 V, DO-214AC): anode on **JACK_5V**, cathode on **5V_LOGIC**.
- Jack-only operation: logic runs at ~4.55 V (well within AP63300 V_IN 3.8–32 V range, ~750 mV margin).
- With USB present: U2 asserts 5.0 V on 5V_LOGIC; D_BACKFEED reverse-biased.
- Backfeed current capped by buck input demand (≤ 600 mA).

### Bulk capacitance

| Cap | Value | Net | Placement |
|---|---|---|---|
| **C_BULK_INPUT** | 470 µF 10 V Al-polymer, ESR ≤ 25 mΩ | JACK_5V → GND | near Q1 output |
| **C_BULK_5V_RAIL** | 220 µF 10 V Al-polymer, ESR ≤ 30 mΩ | 5V_RAIL → GND | central, between row fuses |
| **C_BULK_5V_CER** | 47 µF 10 V X5R 1210 | 5V_RAIL → GND | central |
| **C_BULK_5V_HF** | 100 nF 0603 X7R | 5V_RAIL → GND | central |
| **C_USB_IN** | 10 µF 25 V X7R 0805 | USB_5V_IN → GND | near U1/U2 |
| **C_JACK_IN** | 10 µF 25 V X7R 0805 | JACK_5V → GND | near Q1 output |
| **C_LOGIC_IN** | 10 µF 25 V X7R 0805 | 5V_LOGIC → GND | near U2 output |

Per-row caps: see §10.

## 5. 3.3 V regulator — U4 AP63300WU-7 synchronous buck (TSOT-26)

Part: **AP63300WU-7** (Diodes Incorporated, LCSC C2158012). 3.8 V – 32 V input, 3 A, integrated synchronous MOSFETs, internally compensated, 0.803 V feedback reference, **fixed 500 kHz** switching. Internal compensation and internal soft-start — no external COMP or SS network required.

*(Design rationale, not implementation — retained for handoff context.)* The wider 3.8 V V_IN minimum was the deciding factor over TPS54302DDCR (4.5 V min). Jack-only operation feeds this buck via SS14 backfeed, not directly from the jack. Drop budget at 500 mA logic draw: Q1 R_DS(on) ~50 mV + SS14 V_F ~0.4 V + cable/transient allowance ~0.2 V → steady-state `5V_LOGIC` ≈ 4.5 V with dips to ~4.35 V during S3 WiFi TX bursts. TPS54302 (4.5 V min) would glitch on every burst; AP63300 keeps ≥ 550 mV transient margin. See README §"Buck selection rationale" for the full drop budget.

### U4 AP63300WU-7 pinout (TSOT-26)

| Pin | Name | Net |
|---|---|---|
| 1 | SW | **SW_NODE** — to L_BUCK input; also bootstrap capacitor return |
| 2 | GND | **GND** |
| 3 | FB | **FB_NODE** |
| 4 | EN | tied to **5V_LOGIC** via 100 kΩ **R_EN_BUCK** (always enabled) |
| 5 | VIN | **5V_LOGIC** |
| 6 | BST | **BST_NODE** — via **C_BOOT** 100 nF 0603 X7R to SW (pin 1) |

### External network (required)

- **L_BUCK** 4.7 µH, shielded, I_SAT ≥ 3 A, DCR ≤ 50 mΩ: between **SW_NODE** and **3V3_RAIL**.
- **C_BUCK_IN** 22 µF 25 V X5R 1210: **5V_LOGIC** (U4 pin 5) → GND.
- **C_BUCK_OUT_1** 22 µF 25 V X5R 0805: **3V3_RAIL** (L_BUCK output) → GND.
- **C_BUCK_OUT_2** 22 µF 25 V X5R 0805: **3V3_RAIL** → GND. Total output capacitance ≥ 44 µF.
- **R_FB_TOP** 100 kΩ 1 % 0603: **3V3_RAIL** → **FB_NODE**.
- **R_FB_BOT** 31.6 kΩ 1 % 0603: **FB_NODE** → GND.
- **C_FB** 22 pF 0603 C0G: across R_FB_TOP (feedforward zero).
- **C_BOOT** 100 nF 0603 X7R: between pin 6 (BST) and SW node (pin 1).

V_OUT = 0.803 V × (1 + 100 kΩ / 31.6 kΩ) = **3.34 V** (nominal 3.3 V ±1 % typical).

### Layout requirements (MANDATORY)

- L_BUCK and C_BUCK_OUT_1 within 4 mm of U4.
- **SW node copper area ≤ 3 mm² total** (U4 SW pad + short trace to L_BUCK input pad). SW is a high-dV/dt EMI aggressor — do NOT expand the SW copper for thermal or any other reason.
- SW trace on L1 only; L2 GND plane directly beneath, uninterrupted.
- Feedback divider within 5 mm of U4 pin 3; FB trace flanked by GND pour.
- BST capacitor within 3 mm of U4 between pins 6 and 1.
- No signal routing under L_BUCK or under SW node on any layer.

### Thermal

- AP63300WU-7 is TSOT-26 with no explicit EPAD. Heat exits primarily through pin 2 (GND) and secondarily through pin 5 (VIN). Allocate ≥ 50 mm² L1 GND copper under and around U4, stitched to L2 with ≥ 6 × 0.3 mm vias. Pin 5 (VIN) pad copper may be expanded on the 5V_LOGIC L3 island as a secondary thermal path.
- **SW pin (pin 1) gets NO thermal pour** — its copper stays bounded by the ≤ 3 mm² SW-node rule above. Any attempt to sink heat through SW defeats the EMI constraint and re-introduces radiated noise near the USB diff pair and the antennas.
- Dissipation budget: real workload is ≤ 0.5 A 3V3 typical (S3+H2+mux) ≈ 0.3 W. With the pin-2 GND copper area above, θ_JA ≈ 80 °C/W per Diodes datasheet → rise ≈ 24 °C. Well within SOA. Worst-case 3 A continuous (not a real workload here) would dissipate ~1.7 W and exceed limits; the board is not specified for that load.

## 6. ESP32-S3 module — U5 ESP32-S3-WROOM-1-N16R8

Reference: Espressif ESP32-S3-WROOM-1/1U datasheet (N16R8 variant: 16 MB quad flash + 8 MB octal PSRAM).

### Power
- 3V3 pins (module pin 2) → **3V3_RAIL**
- GND pins (module pins 1, 40, 41, EPAD) → **GND**
- EN (module pin 3) → **EN_S3_NODE** (10 kΩ **R_EN_S3** pullup to 3V3_RAIL + 1 µF **C_ESP_EN** to GND + SW1 RESET_S3 button to GND)

### I/O used

| GPIO | Module pin | Net | Destination |
|---|---|---|---|
| IO0  | 27 | **BOOT_S3_BTN** | SW2 terminal 1 (module internal pullup) |
| IO4  | 4  | **TLC_SCK** | TLC5947DAP pin 4 (SCLK) |
| IO5  | 5  | **TLC_SIN** | TLC5947DAP pin 3 (SIN) |
| IO6  | 6  | **TLC_XLAT** | TLC5947DAP pin 2 (XLAT) |
| IO7  | 7  | **TLC_BLANK** | TLC5947DAP pin 1 (BLANK) |
| IO8  | 8  | **I2C_SDA_3V3** | TCA9548A SDA |
| IO9  | 9  | **I2C_SCL_3V3** | TCA9548A SCL |
| IO10 | 10 | **MUX_RESET_N** | TCA9548A /RESET (+ 10 kΩ R_MUX_RST pullup to 3V3) |
| IO11 | 11 | **S3_TO_H2_RST** | R_H2_RST_SER 1 kΩ → **H2_RESET_N** (H2 EN) |
| IO12 | 12 | **H2_IRQ** | H2 IO10 (active low, H2 open-drain; 10 kΩ R_H2_IRQ pullup to 3V3) |
| IO13 | 13 | **S3_TO_H2_BOOT** | R_H2_BOOT_SER 1 kΩ → **H2_BOOT_SEL** (H2 IO9 strap) |
| IO17 | 17 | **H2_UART_TX** | H2 IO4 (H2's UART1 RX) |
| IO18 | 18 | **H2_UART_RX** | H2 IO5 (H2's UART1 TX) |
| IO19 | 23 | **USB_DN** | USBLC6 pins 1/6 |
| IO20 | 24 | **USB_DP** | USBLC6 pins 3/4 |
| IO21 | 25 | **RGB_DATA** | SK6812 LED_RGB_1 DIN (via 33 Ω R_RGB_SER) |
| IO43 | 37 | **U0TXD_S3** | JP_DEBUG_S3 pin 3, TP_U0TXD_S3 |
| IO44 | 36 | **U0RXD_S3** | JP_DEBUG_S3 pin 4, TP_U0RXD_S3 |

Module pin numbers per Espressif ESP32-S3-WROOM-1 datasheet rev 1.4+.

### Reserved / no-copper-escape
- IO3, IO45, IO46: strap pins. No copper on any layer beyond module pad.
- IO26..IO32: internal SPI flash (N16 variant). Module-internal, no external connection.
- IO33..IO37: internal octal PSRAM (R8 variant). Module-internal, no external connection.
- IO48: NC (optional RGB LED on some module revs).

### Decoupling (MANDATORY — place per Espressif reference design)

- **C_ESP_BULK** 22 µF 25 V X5R 0805 on 3V3_RAIL within 10 mm of module.
- At each module 3V3 entry (module has one 3V3 pin but internal distribution creates multiple VDD domains), three local decoupling pairs around module perimeter:
  - **C_ESP_DEC1a** 100 nF 0603 X7R + **C_ESP_DEC1b** 1 µF 0603 X5R
  - **C_ESP_DEC2a** 100 nF 0603 X7R + **C_ESP_DEC2b** 1 µF 0603 X5R
  - **C_ESP_DEC3a** 100 nF 0603 X7R + **C_ESP_DEC3b** 1 µF 0603 X5R
- **C_ESP_EN** 1 µF 25 V X5R 0603 from EN_S3_NODE to GND.

Rationale: WiFi burst transient current up to 500 mA requires paired bulk + HF local decoupling at each VDD entry.

## 7. ESP32-H2 module — U_H2 ESP32-H2-MINI-1-N4

Reference: Espressif ESP32-H2-MINI-1 datasheet (N4 variant: 4 MB SPI flash). RISC-V @ 96 MHz, IEEE 802.15.4 + Bluetooth LE 5.

### Power
- 3V3 pins (module pin 2) → **3V3_RAIL**
- GND pins (module pins 1, 3, 25, 26, 27, 28/EPAD per datasheet) → **GND**
- EN (module pin 24) → **H2_RESET_N** (10 kΩ **R_EN_H2** pullup to 3V3_RAIL + 1 µF **C_H2_EN** to GND). Driven low by S3 IO11 (through 1 kΩ R_H2_RST_SER) during reset.

### I/O used

| H2 GPIO | Module pin | Direction | Net | Destination |
|---|---|---|---|---|
| IO2  | 20 | Input, strap — tie HIGH | — | 10 kΩ R_H2_STRAP2 → 3V3_RAIL |
| IO4  | 4  | Input (H2 UART1 RX) | **H2_UART_TX** | S3 IO17 |
| IO5  | 5  | Output (H2 UART1 TX) | **H2_UART_RX** | S3 IO18 |
| IO8  | 9  | Input, strap — tie HIGH | — | 10 kΩ R_H2_STRAP8 → 3V3_RAIL |
| IO9  | 10 | Input, strap — **H2_BOOT_SEL** | **H2_BOOT_SEL** | 10 kΩ R_H2_STRAP9 → 3V3_RAIL; R_H2_BOOT_SER 1 kΩ from S3 IO13; JP_DEBUG_H2 pin 6 |
| IO10 | 11 | Output, open-drain | **H2_IRQ** | S3 IO12 (10 kΩ R_H2_IRQ pullup to 3V3 on S3 side) |
| IO11 | 12 | Output | **LED_H2_HB**  | R_LED_H2_HB 1 kΩ → LED_4 anode (yellow) |
| IO12 | 16 | Output | **LED_ZIGBEE** | R_LED_ZIGBEE 1 kΩ → LED_6 anode (yellow) |
| IO13 | 17 | Output | **LED_BLE**    | R_LED_BLE 1 kΩ → LED_7 anode (blue) |
| IO23 | 14 | Input (H2 UART0 RX, debug) | **U0RXD_H2** | JP_DEBUG_H2 pin 4, TP_U0RXD_H2 |
| IO24 | 15 | Output (H2 UART0 TX, debug) | **U0TXD_H2** | JP_DEBUG_H2 pin 3, TP_U0TXD_H2 |
| IO26 | 23 | USB D− (H2 built-in USB Serial/JTAG) | **H2_USB_DN** | TP_H2_USB_DN |
| IO27 | 22 | USB D+ (H2 built-in USB Serial/JTAG) | **H2_USB_DP** | TP_H2_USB_DP |

Module pin numbers per Espressif ESP32-H2-MINI-1 datasheet rev 1.1+.

### Reserved
- IO0, IO1, IO3, IO14, IO22, IO25: unused — no copper escape beyond module pad.

### Decoupling (MANDATORY)

- **C_H2_BULK** 10 µF 10 V X5R 0805 on 3V3_RAIL within 10 mm of module (10 V rating requires 0805 in X5R; 0603 tops out at 6.3 V for this capacitance).
- Three local decoupling pairs adjacent to module 3V3 entry:
  - **C_H2_DEC1a** 100 nF 0603 X7R + **C_H2_DEC1b** 1 µF 0603 X5R
  - **C_H2_DEC2a** 100 nF 0603 X7R + **C_H2_DEC2b** 1 µF 0603 X5R
  - **C_H2_DEC3a** 100 nF 0603 X7R
- **C_H2_EN** 1 µF 25 V X5R 0603 from H2_RESET_N to GND.

### H2 boot / programming strap

- Normal boot (SPI flash): H2 IO9 = HIGH. Achieved via R_H2_STRAP9 10 kΩ pullup to 3V3_RAIL. S3 IO13 held high-Z (input) during normal operation.
- UART download (for first flash or recovery): S3 IO13 driven LOW through R_H2_BOOT_SER 1 kΩ, overriding the 10 kΩ pullup at IO9 node. Then pulse H2_RESET_N low via S3 IO11 for ≥ 10 ms and release. H2 enters UART download mode. S3 drives esptool protocol on UART at 115200 bps (auto-baud).
- External factory flash alternative: JP_DEBUG_H2 pin 6 exposes H2_BOOT_SEL — external USB-UART dongle can assert low directly.

## 8. Reset and BOOT buttons

### SW1 RESET_S3 — tactile SMD 6×6
- Terminal 1 → **EN_S3_NODE**
- Terminal 2 → **GND**

### SW2 BOOT_S3 — tactile SMD 6×6
- Terminal 1 → **BOOT_S3_BTN**
- Terminal 2 → **GND**

No physical buttons for H2 — reset and boot controlled by S3 or JP_DEBUG_H2.

## 9. TCA9548A mux — U6 TCA9548APWR (TSSOP-24)

### Power and address
| Pin | Name | Net |
|---|---|---|
| 24 | VCC | **3V3_RAIL** |
| 12 | GND | **GND** |
| 1 | A0 | **GND** |
| 2 | A1 | **GND** |
| 3 | A2 | **GND** |

Resolved I2C address: **0x70**.

### Master-side I2C
| Pin | Name | Net | Pullup |
|---|---|---|---|
| 22 | SCL | **I2C_SCL_3V3** | 4.7 kΩ **R_SCL_MCU** to 3V3_RAIL |
| 23 | SDA | **I2C_SDA_3V3** | 4.7 kΩ **R_SDA_MCU** to 3V3_RAIL |

### Reset
- Pin 4 /RESET → **MUX_RESET_N** (S3 GPIO10). Pullup 10 kΩ **R_MUX_RST** to 3V3_RAIL.

### Pin mapping (LOCKED — per TI SCPS195)

| Pin | Name |
|---|---|
| 1 | A0 |
| 2 | A1 |
| 3 | A2 |
| 4 | /RESET |
| 5 | SD0 |
| 6 | SC0 |
| 7 | SD1 |
| 8 | SC1 |
| 9 | SD2 |
| 10 | SC2 |
| 11 | SD3 |
| 12 | GND |
| 13 | SC3 |
| 14 | SD4 |
| 15 | SC4 |
| 16 | SD5 |
| 17 | SC5 |
| 18 | SD6 |
| 19 | SC6 |
| 20 | SD7 |
| 21 | SC7 |
| 22 | SCL |
| 23 | SDA |
| 24 | VCC |

### Downstream channel net assignments

| Ch | SDA pin / net | SCL pin / net |
|---|---|---|
| 0 | pin 5 → **MUX_SDA1** | pin 6 → **MUX_SCL1** |
| 1 | pin 7 → **MUX_SDA2** | pin 8 → **MUX_SCL2** |
| 2 | pin 9 → **MUX_SDA3** | pin 10 → **MUX_SCL3** |
| 3 | pin 11 → **MUX_SDA4** | pin 13 → **MUX_SCL4** |
| 4 | pin 14 → **MUX_SDA5** | pin 15 → **MUX_SCL5** |
| 5 | pin 16 → **MUX_SDA6** | pin 17 → **MUX_SCL6** |
| 6 | pin 18 → **MUX_SDA7** | pin 19 → **MUX_SCL7** |
| 7 | pin 20 → **MUX_SDA8** | pin 21 → **MUX_SCL8** |

Each MUX_SDA{n} / MUX_SCL{n}: 4.7 kΩ pullup to 3V3_RAIL (**R_SDA_MUX{n}** / **R_SCL_MUX{n}**).

### Decoupling
- **C_MUX_DEC** 100 nF 0603 X7R between pin 24 and GND, adjacent to U6.

### Debug header (pre-mux logic analyzer access) — JP_DEBUG_I2C

1×4 pin header 2.54 mm:

| Pin | Net | Notes |
|---|---|---|
| 1 | **3V3_RAIL** | power |
| 2 | **GND** | |
| 3 | **I2C_SDA_3V3** (via 220 Ω **R_DBG_SDA**) | series resistor protects MCU from probe loading |
| 4 | **I2C_SCL_3V3** (via 220 Ω **R_DBG_SCL**) | |

## 10. Per-row level shift + protection (identical block × 8, n = 1..8)

### PCA9306DCTR — U{6+n} VSSOP-8

| Pin | Name | Net |
|---|---|---|
| 1 | EN | **3V3_RAIL** |
| 2 | VREF1 | **3V3_RAIL** |
| 3 | SDA1 | **MUX_SDA{n}** |
| 4 | GND | **GND** |
| 5 | SCL1 | **MUX_SCL{n}** |
| 6 | SCL2 | **ROW{n}_SCL_5V** |
| 7 | VREF2 | **5V_RAIL** |
| 8 | SDA2 | **ROW{n}_SDA_5V** |

### Package selection (LOCKED)

- **VSSOP-8 (DCT suffix, PCA9306DCTR)** only. SOT-23-6 `PCA9306DP1R` variant MUST NOT substitute — different pinout, incompatible footprint.
- Pinout above matches TI SCPS169 VSSOP-8 datasheet exclusively.

### 5 V-side pullups
- **R_SDA_ROW{n}** 4.7 kΩ: **ROW{n}_SDA_5V** → **5V_RAIL**
- **R_SCL_ROW{n}** 4.7 kΩ: **ROW{n}_SCL_5V** → **5V_RAIL**

### ESD array — U{14+n} PESD3V3L5UW (SOT-363)
Connected on the row-side of the level shifter, upstream of the connector:
- Channel 1 (pins 1–6): **ROW{n}_SDA_5V** to **GND**
- Channel 2 (pins 2–5): **ROW{n}_SCL_5V** to **GND**
- Channel 3 (pins 3–4): tied to GND or left NC per Nexperia datasheet.

### Power protection
- **F{n}** 2 A polyfuse 1812 (Littelfuse 1812L200/24THMR): between **5V_RAIL** and **ROW{n}_5V_OUT**.
- **D_ROW{n}** SMBJ5.0A unidirectional TVS (600 W, 5 V working, 9.2 V clamp): between **ROW{n}_5V_OUT** and **GND**.
- **C_ROW{n}** 47 µF 10 V X5R 1210: between **ROW{n}_5V_OUT** and **GND** at connector.
- **C_ROW{n}_HF** 100 nF 0603 X7R: between **ROW{n}_5V_OUT** and **GND** at connector.

### Row connector — J{2+n} B4B-XH-A (4-pin THT, 2.5 mm pitch)
| Pin | Signal |
|---|---|
| 1 | **ROW{n}_5V_OUT** |
| 2 | **GND** |
| 3 | **ROW{n}_SDA_5V** |
| 4 | **ROW{n}_SCL_5V** |

## 11. LED bar

A 35-LED status bar runs along the front-facing long edge of the PCB, in two stacked rows. The 3D-printed case exposes this edge through a slot aligned with the silkscreen label `LED ROW — FRONT`.

Top row: 19 fixed-function side-view SMD LEDs (Everlight 19-217 series, 1204 package, 4 mm pitch = 72 mm).
Bottom row: 16 × SK6812-mini RGB side-emit (3.5 mm pitch = 56 mm).

### 11.1 Fixed row — LED reference, driver, net

| Ref   | Color      | Driver              | Series R / Driver       | Net            | Indicates |
|-------|------------|---------------------|--------------------------|----------------|-----------|
| LED_1 | green      | always-on           | 1 kΩ **R_LED_5V**        | `5V_RAIL`      | 5V rail present |
| LED_2 | green      | always-on           | 1 kΩ **R_LED_3V3**       | `3V3_RAIL`     | 3V3 rail present |
| LED_3 | green      | **U_LED** ch0       | TLC5947 (constant I)     | `LED_S3_HB`    | S3 heartbeat |
| LED_4 | yellow     | H2 IO11             | 1 kΩ **R_LED_H2_HB**     | `LED_H2_HB`    | H2 heartbeat |
| LED_5 | blue       | **U_LED** ch1       | TLC5947                  | `LED_WIFI`     | WiFi link + traffic |
| LED_6 | yellow     | H2 IO12             | 1 kΩ **R_LED_ZIGBEE**    | `LED_ZIGBEE`   | Zigbee link + TX/RX |
| LED_7 | blue | H2 IO13             | 1 kΩ **R_LED_BLE**       | `LED_BLE`      | BLE state + GATT |
| LED_8 | green      | **U_LED** ch2       | TLC5947                  | `LED_I2C_ACT`  | I2C bus activity |
| LED_9 | white      | **U_LED** ch3       | TLC5947                  | `LED_UART_ACT` | S3↔H2 UART activity |
| LED_10 | green     | **U_LED** ch4       | TLC5947                  | `LED_ROW_1`    | Row 1 selected |
| LED_11 | green     | **U_LED** ch5       | TLC5947                  | `LED_ROW_2`    | Row 2 selected |
| LED_12 | green     | **U_LED** ch6       | TLC5947                  | `LED_ROW_3`    | Row 3 selected |
| LED_13 | green     | **U_LED** ch7       | TLC5947                  | `LED_ROW_4`    | Row 4 selected |
| LED_14 | green     | **U_LED** ch8       | TLC5947                  | `LED_ROW_5`    | Row 5 selected |
| LED_15 | green     | **U_LED** ch9       | TLC5947                  | `LED_ROW_6`    | Row 6 selected |
| LED_16 | green     | **U_LED** ch10      | TLC5947                  | `LED_ROW_7`    | Row 7 selected |
| LED_17 | green     | **U_LED** ch11      | TLC5947                  | `LED_ROW_8`    | Row 8 selected |
| LED_18 | orange    | **U_LED** ch12      | TLC5947                  | `LED_OTA`      | OTA in progress |
| LED_19 | red       | **U_LED** ch13      | TLC5947                  | `LED_FAULT`    | Fault / recovery |

TLC5947 channels 14..23 route to an unpopulated 1×10 header (`JP_LED_EXP`) for future LEDs.

Power LED anode = rail (5V_RAIL or 3V3_RAIL), cathode = GND via series resistor. All other fixed LEDs have anode at the driver output, cathode at GND (TLC5947 is a constant-current sink, so its channels pull the cathode to ~0 V when enabled; the LED anodes tie to 3V3_RAIL).

### 11.2 TLC5947 LED driver — U_LED

**Part:** Texas Instruments TLC5947DAP — HTSSOP-28 (Power-PAD).
**Function:** 24-channel 12-bit PWM, constant-current sink. Drives 14 of the 19 fixed LEDs (3 rails are hardware-driven, 3 are H2-driven).

| TLC5947 pin | Name  | Net                      | Notes |
|-------------|-------|--------------------------|-------|
| 1           | BLANK | **TLC_BLANK** (S3 IO7)   | Active high disables outputs |
| 2           | XLAT  | **TLC_XLAT** (S3 IO6)    | Latch shift register → PWM |
| 3           | SIN   | **TLC_SIN** (S3 IO5)     | Serial data in |
| 4           | SCLK  | **TLC_SCK** (S3 IO4)     | Serial clock, 30 MHz max |
| 5           | SOUT  | NC                       | For daisy-chain (unused) |
| 6           | GND   | **GND**                  | |
| 7           | IREF  | **R_IREF** to GND        | 2.7 kΩ sets ~15 mA per channel |
| 8           | VCC   | **3V3_RAIL**             | 100 nF + 10 µF decoupling |
| 9–23        | OUT0–OUT14 | LED cathodes (LED_3, 5, 8, 9, 10..17, 18, 19, + 4 spare) | open-drain constant current |
| 24–31       | OUT15–OUT22 | `JP_LED_EXP` pins 1–8 | expansion (DNP LEDs) |
| 32          | OUT23 | `JP_LED_EXP` pin 9       | expansion |
| (EPAD)      | GND thermal pad | **GND** | stitch 6 vias to L2 |

Decoupling:
- **C_LED_DEC1** 100 nF 0603 X7R between VCC pin 8 and EPAD, adjacent to U_LED.
- **C_LED_DEC2** 10 µF 0805 X5R at the VCC fan-out of U_LED.

IREF sets the per-channel current via `I_out = 1.23 V / R_IREF × 20`. R_IREF = 2.7 kΩ → I_out ≈ 9 mA × 20 / current-ratio... (actually TI formula: I_out = 1.23 / R_IREF × 15, at R=2 kΩ → 9.2 mA typical). Target **15 mA** per channel; order exact value from datasheet curve.

### 11.3 RGB strip — 16 × SK6812 mini side-emit

Daisy-chained on a single data line from S3 IO21 (`RGB_DATA`). 800 kHz bitstream, NRZ encoding, GRB colour ordering.

| SK6812 pin | Net                      | Notes |
|------------|--------------------------|-------|
| VDD        | **5V_RAIL**              | 5 V supply, 10 nF per LED (C_RGB{1..16}) |
| VSS        | **GND**                  | |
| DIN (LED1) | **RGB_DATA** (S3 IO21)   | 3.3 V logic → SK6812 V_IH = 0.7 × VDD = 3.5 V. Marginal; populate **R_RGB_SER** 33 Ω in series to limit reflection and **LEVEL_RGB** 74LVC1T45 3V3→5V translator if logic-level qualification fails on bring-up (DNP by default; jumper JP_RGB_BYPASS shorts out the translator). |
| DOUT (LED1) | → DIN (LED2)            | daisy-chain |
| ...        | ...                      | |
| DOUT (LED16) | NC                      | terminated at last LED |

Decoupling: **C_RGB_BULK** 100 µF 10 V 1210 at the start of the strip; per-LED 100 nF on VDD pin.

### 11.4 H2-direct LEDs

LED_4 (heartbeat), LED_6 (Zigbee), LED_7 (BLE) are driven straight from H2 GPIOs (IO11/12/13) through a 1 kΩ series resistor each. Anode to 3V3_RAIL is not used — active-high GPIO drives the anode; cathode to GND.

| Ref   | H2 GPIO | Net          | Series R          |
|-------|---------|--------------|-------------------|
| LED_4 | IO11    | `LED_H2_HB`  | 1 kΩ R_LED_H2_HB  |
| LED_6 | IO12    | `LED_ZIGBEE` | 1 kΩ R_LED_ZIGBEE |
| LED_7 | IO13    | `LED_BLE`    | 1 kΩ R_LED_BLE    |

### 11.5 Physical placement

- LEDs 1–19 occupy the upper of two parallel rows along the long edge, 4 mm centre-to-centre, viewed face-up with the LED visible through the case slot.
- 16 SK6812 occupy the lower row, 3.5 mm centre-to-centre, aligned to the long edge opposite the connector bank.
- Silkscreen: label the slot `LED ROW — FRONT`, number each LED with its `LED_n` reference for field debug.
- Keepout: no components or vias within 2 mm of the emission axis of each LED (side-emit — axis is parallel to the board surface, pointing off the long edge).

### 11.6 Activity pulse-stretch

Comm-activity LEDs (`LED_WIFI`, `LED_ZIGBEE`, `LED_BLE`, `LED_I2C_ACT`, `LED_UART_ACT`) use a minimum-50 ms on-time stretch in firmware. Per-byte or per-packet events retrigger a `last_activity_ms` timestamp; the LED output is `(millis() - last_activity_ms) < 50`. Without this, the flicker is too fast for the eye to perceive at link-layer rates.

## 12. Debug headers and test points

### JP_DEBUG_S3 — 1×4 header 2.54 mm (UART0 breakout for S3)

| Pin | Net | Notes |
|---|---|---|
| 1 | **3V3_RAIL** | power, 0.5 A max |
| 2 | **GND** | |
| 3 | **U0TXD_S3** (S3 IO43) | S3 TX |
| 4 | **U0RXD_S3** (S3 IO44) | S3 RX |

Primary S3 debug is the USB-C connector (native USB Serial/JTAG via S3 GPIO19/20). JP_DEBUG_S3 is secondary/bench access.

### JP_DEBUG_H2 — 1×6 header 2.54 mm (UART0 + EN + BOOT for H2, manual control only)

| Pin | Net | Notes |
|---|---|---|
| 1 | **3V3_RAIL** | power, 0.3 A max |
| 2 | **GND** | |
| 3 | **U0TXD_H2** (H2 IO24) | H2 TX — to external USB-UART RX |
| 4 | **U0RXD_H2** (H2 IO23) | H2 RX — from external USB-UART TX |
| 5 | **H2_RESET_N** (H2 EN) | manual only: jumper-wire a dongle GPIO to this pin and hand-assert low to reset H2 |
| 6 | **H2_BOOT_SEL** (H2 IO9) | manual only: hand-assert low before reset to enter UART download mode |

**This header is NOT auto-reset compatible.** No DTR/RTS-to-EN/BOOT control transistor network exists on the PCB. External USB-UART dongles that rely on DTR/RTS auto-reset (esptool `--before default_reset` / Arduino IDE auto-flash) will not cycle EN and BOOT automatically via pins 5/6 — the dongle's DTR/RTS are passive voltage rails as seen from this header. The workflow is manual: externally drive pin 6 low, externally pulse pin 5 low, release, then run esptool with `--before no_reset --after no_reset`. For fully automated programming, drive H2 via S3 (S3 owns H2 EN through R_H2_RST_SER and H2 BOOT_SEL through R_H2_BOOT_SER) or use the H2 native USB-JTAG test pads (TP_H2_USB_DP/DN).

### H2 USB Serial/JTAG test pads

H2's built-in USB 2.0 Full-Speed peripheral on IO26/IO27 is not routed to a connector. Expose as 1.0 mm pads:

| TP | Net |
|---|---|
| TP_H2_USB_DN | H2_USB_DN (H2 IO26) |
| TP_H2_USB_DP | H2_USB_DP (H2 IO27) |

Access via wire-solder pigtail or pogo-pin jig for direct JTAG debug of H2 without going through S3.

### Test points (all 1.0 mm exposed round pad, no drill, silkscreen label, L1 access)

| Label | Net |
|---|---|
| TP_3V3 | 3V3_RAIL |
| TP_5V | 5V_RAIL |
| TP_5V_LOGIC | 5V_LOGIC |
| TP_GND | GND |
| TP_MCU_SDA | I2C_SDA_3V3 |
| TP_MCU_SCL | I2C_SCL_3V3 |
| TP_MUX_RST | MUX_RESET_N |
| TP_U0TXD_S3 | U0TXD_S3 |
| TP_U0RXD_S3 | U0RXD_S3 |
| TP_U0TXD_H2 | U0TXD_H2 |
| TP_U0RXD_H2 | U0RXD_H2 |
| TP_H2_UART_TX | H2_UART_TX |
| TP_H2_UART_RX | H2_UART_RX |
| TP_H2_RST | H2_RESET_N |
| TP_H2_IRQ | H2_IRQ |
| TP_H2_BOOT | H2_BOOT_SEL |
| TP_H2_USB_DP | H2_USB_DP |
| TP_H2_USB_DN | H2_USB_DN |
| TP_MUX_SDA1..8 | MUX_SDA1..8 |
| TP_MUX_SCL1..8 | MUX_SCL1..8 |
| TP_ROW1..8_5V | ROW1..8_5V_OUT |
| TP_ROW1..8_SDA | ROW1..8_SDA_5V |
| TP_ROW1..8_SCL | ROW1..8_SCL_5V |

## 13. Mux bypass path

Zero-ohm solder bridges from the S3 I2C bus direct to the Row 1 level-shifter, bypassing the TCA9548A:

- **JP_BYPASS_SDA** 0 Ω 0402, DNP by default — bridges **I2C_SDA_3V3** → **MUX_SDA1** when populated.
- **JP_BYPASS_SCL** 0 Ω 0402, DNP by default — bridges **I2C_SCL_3V3** → **MUX_SCL1** when populated.

Purpose: isolate TCA9548A failure from PCA9306 failure from cable failure during bring-up.

Operation: populate both jumpers AND hold MUX_RESET_N low (test clip) to force the TCA9548A high-Z. S3 then drives the Row 1 PCA9306 level shifter directly → ROW1 XH-4.

Default state: **DNP**. Only populate for diagnostic runs. Remove before deployment.

## 14. Inter-MCU UART link — specification

| Parameter | Value |
|---|---|
| Physical layer | CMOS 3.3 V logic, push-pull, single-ended |
| Nets | H2_UART_TX (S3→H2), H2_UART_RX (H2→S3), H2_RESET_N (S3→H2), H2_IRQ (H2→S3, open-drain), H2_BOOT_SEL (S3→H2, strap override) |
| Baud rate | **921 600 bps** |
| Data format | 8 data bits, no parity, 1 stop bit (8N1) |
| Flow control | **None** (hardware RTS/CTS not used; protocol-level flow control via ACK/NACK and bounded-buffer credits) |
| Framing | **COBS** (Consistent Overhead Byte Stuffing) with 0x00 delimiter byte |
| Integrity | **CRC-16/CCITT-FALSE** (poly 0x1021, init 0xFFFF, no reflect, xorout 0x0000) on frame payload |
| Roles | S3 is MASTER. H2 is RESPONDER. |
| Transaction model | Command/response. Every S3 command expects an H2 reply (ACK, NACK, or data). |
| H2_IRQ semantics | H2 asserts low when it has asynchronous data (e.g. inbound Zigbee message). S3 responds by issuing a POLL command. |
| Max frame length | 256 bytes payload (before COBS + CRC). |
| S3 UART peripheral | UART1 |
| H2 UART peripheral | UART1 (IO4=RX, IO5=TX — GPIO matrix routed) |
| H2 UART0 (IO23/24) | Reserved for H2 ROM bootloader + debug console at 115200 8N1, via JP_DEBUG_H2 only |

**Hard rule: no SPI between S3 and H2.** Capture ERC must enforce that no net named `*_SPI_*`, `*_MISO*`, `*_MOSI*`, `*_SCK*`, `*_CS_H2*` exists.

## 15. Power summary

| Net | Source | Load |
|---|---|---|
| USB_5V_IN | USB-C VBUS via FB1 | U1 pin 5, U2 pin 1, C_USB_IN |
| JACK_5V | barrel via Q1 | 5V_RAIL (direct), D_BACKFEED anode, C_BULK_INPUT, C_JACK_IN |
| 5V_LOGIC | U2 LM66100 OR D_BACKFEED | U4 pin 2 (buck in), C_LOGIC_IN |
| 5V_RAIL | JACK_5V (direct) | F1..F8, PCA9306 VREF2 (×8), C_BULK_5V_RAIL, C_BULK_5V_CER, C_BULK_5V_HF |
| 3V3_RAIL | U4 AP63300 + L_BUCK | S3 VDD, H2 VDD, TCA9548A VCC, PCA9306 VREF1 + EN (×8), all 3V3-side pullups, power LED, S3/H2 EN pullups |
| GND | common return | all |

Maximum system input current (spec): **8 A** total at 5V_RAIL via barrel jack.
Per-row polyfuse: 2 A hold.
3V3 current budget: ~800 mA peak (S3 500 mA WiFi burst + H2 150 mA BLE/Zigbee TX + 150 mA headroom). Buck rated 3 A — 3.7× margin.

## 16. I2C pullup summary

| Net | Master-side R | Row-side R | Endpoint A | Endpoint B |
|---|---|---|---|---|
| I2C_SDA_3V3 | 4.7 kΩ to 3V3 | — | S3 GPIO8 | TCA9548A SDA |
| I2C_SCL_3V3 | 4.7 kΩ to 3V3 | — | S3 GPIO9 | TCA9548A SCL |
| MUX_SDA1..8 | 4.7 kΩ to 3V3 | — | TCA9548A downstream SDA (per row) | PCA9306 SDA1 (Row n) |
| MUX_SCL1..8 | 4.7 kΩ to 3V3 | — | TCA9548A downstream SCL (per row) | PCA9306 SCL1 (Row n) |
| ROW{n}_SDA_5V | — | 4.7 kΩ to 5V | PCA9306 SDA2 | J{2+n} pin 3 via PESD |
| ROW{n}_SCL_5V | — | 4.7 kΩ to 5V | PCA9306 SCL2 | J{2+n} pin 4 via PESD |

Firmware rule: mux channel-select register accepts only single-bit values (0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80). Multi-bit masks are rejected — parallel row pullups would exceed ATmega328P I_OL.

## 17. Package and pin-1 reference (BOM validation)

| Ref | Part | Package | Pin-1 marker | Authoritative datasheet |
|---|---|---|---|---|
| U1 | USBLC6-2SC6 | SOT-23-6 / SC-74-6 | Dot, top-left with part number readable L-to-R. Pin 1 = I/O 1 (D−). Pin 4 tied to pin 3 (I/O 2), **NOT ground**. | ST DS3019 |
| U2 | LM66100DCKR | SOT-23-5 | Pin 1 = IN, pin 5 = OUT. Single instance; USB path only. | TI SNVSBL1 |
| U4 | AP63300WU-7 | TSOT-26 (W suffix) | Dot on pin 1 corner. Pin 1 = SW, pin 6 = BST. | Diodes AP63300 datasheet rev 2+ |
| U5 | ESP32-S3-WROOM-1-N16R8 | 65-pin SMD module (18 × 25.5 mm) | Marking on module silk; pin 1 at antenna-opposite corner. | Espressif ESP32-S3-WROOM-1/1U datasheet rev 1.4+ |
| U_H2 | ESP32-H2-MINI-1-N4 | 28-pad SMD module (13.2 × 16.6 mm) | Marking on module silk. | Espressif ESP32-H2-MINI-1 datasheet rev 1.1+ |
| U6 | TCA9548APWR | TSSOP-24 PW | Dot/dimple on one narrow end; pin 1 = A0. **Trap**: RGE (QFN) variant has different pad pattern. Order **only PWR suffix**. | TI SCPS195 |
| U7..U14 | PCA9306DCTR | VSSOP-8 (DCT suffix) | Dot on pin 1 corner; pin 1 = EN. **Trap**: `PCA9306DP1R` is SOT-23-6 with different pinout — do not substitute. | TI SCPS169 |
| U15..U22 | PESD3V3L5UW | SOT-363 (SC-88) | Pin 1 marker per Nexperia datasheet. | Nexperia PESD3V3L5UW datasheet |
| U_LED | TLC5947DAP | HTSSOP-28 (Power-PAD, DAP suffix) | Dot on pin 1 corner; pin 1 = BLANK. **Trap**: non-DAP (TLC5947DBT) is TSSOP without thermal pad — runs hotter. Order **DAP suffix** for our continuous 24-channel drive. | TI SBVS107 |
| Q1 | AO3401A | SOT-23-3 | Gate = pin 1, source = pin 3, drain = pin 2. | AOS AO3401A datasheet |
| D1 | SMAJ5.0CA | DO-214AC (SMA) | CA suffix = bidirectional, no polarity. | Littelfuse SMAJ series datasheet |
| D_ROW1..8 | SMBJ5.0A | DO-214AA (SMB) | A suffix = unidirectional; cathode band to +5 V. | Littelfuse SMBJ series datasheet |
| D_BACKFEED | SS14 | DO-214AC (SMA) | Cathode band on V_OUT side; anode on JACK_5V, cathode on 5V_LOGIC. | ON Semi / Vishay SS14 datasheet |

---

## CRITICAL DESIGN RULES (MUST FOLLOW)

1. **High-current row power MUST NOT pass through LM66100.** JACK_5V → 5V_RAIL direct; LM66100 (U2) is on USB path only.
2. **Inter-MCU communication MUST be UART only.** SPI between S3 and H2 is forbidden. ERC enforces this.
3. **Buck regulator layout (§5) MUST follow AP63300 datasheet.** SW node ≤ 3 mm², L_BUCK ≤ 4 mm from U4, C_BOOT ≤ 3 mm from U4 between pins 1 (SW) and 6 (BST).
4. **All IC pinouts MUST be verified against vendor datasheets at capture time.** No approximate pin mappings — §6, §7, §9, §10 tables are authoritative.
5. **5V_RAIL (row power) and 5V_LOGIC (regulator input) are distinct nets.** Do not short them on PCB.
6. **D_BACKFEED Schottky is the only connection between JACK_5V and 5V_LOGIC.** Polarity: anode JACK_5V, cathode 5V_LOGIC. Reversal back-drives the buck from logic into row power.
7. **Antenna keep-outs are hard layout rules.** 15×7 mm under S3 antenna (top-left corner), 11×6 mm under H2 antenna (bottom-right corner), cleared on all 4 layers. Modules on **diagonally opposite corners**. ≥ 60 mm antenna edge-to-edge separation required (~115 mm achieved on the 120 × 80 mm Rev B board).
8. **H2 is a slave.** S3 is the sole system controller. H2 cannot drive MUX, row I2C, or power state.
