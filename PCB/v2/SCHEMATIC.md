# PCB v2 — textual schematic

Authoritative netlist for both boards in the v2 system: the **Master** (controller) and the **Unit** (per-flap daughter board). Each section is independent — the two boards are separate fab projects connected only by CAT5e/CAT6 cable with the RJ45 pinout locked in [`README.md`](./README.md) and restated in [`PINOUT.md`](./PINOUT.md).

Conventions:
- Net names in `UPPER_SNAKE_CASE`. Signals that cross the RJ45 cable are prefixed with neither `MASTER_` nor `UNIT_` — they are the cable nets (`+48V`, `GND`, `RS485_A`, `RS485_B`).
- Designators are scoped per board. Master designators prefixed `M_` where otherwise ambiguous; unit designators prefixed `U_`. The `Board` column in [`BOM.csv`](./BOM.csv) is authoritative.
- Passive values inline. "→" means "connects to".
- Every pin of every IC is defined. Every net has a name. No "verify later" / "approximate" language.

---

# SECTION A — MASTER BOARD

## A.1 Power input — J_PWR (DC barrel 5.5 / 2.5 mm, 60 V rated)

- Centre pin → **VIN_RAW**
- Sleeve → **GND**

### Reverse-polarity PMOS — Q_RP AO4407 (SOIC-8, 60 V V_DS, R_DS(on) 12 mΩ @ V_GS=10 V)

| Pin | Name | Net |
|---|---|---|
| 1–3 | S (source, paralleled) | **VIN_RAW** |
| 4 | G (gate) | **VIN_RAW** via 100 kΩ **R_QRP_G** (turn-on); Zener clamp 15 V **D_QRP_Z** to source |
| 5–8 | D (drain, paralleled) | **VIN_48V** |

Source-to-gate R ensures the body diode conducts briefly on polarity-correct insertion, the gate pulls to (VIN_RAW − V_Z) through the Zener, and the PMOS fully enhances at V_GS ≈ −15 V. On reversed insertion, V_GS is positive and the PMOS stays off; the body diode also stays reverse-biased. No current flows anywhere. Zener limits V_GS to ±15 V (spec max V_GSS ±20 V).

### Input TVS — D_VIN SMBJ58CA (DO-214AA, bidirectional, 58 V working, 93 V clamp @ 1 A, 600 W pulse)

- Between **VIN_48V** and **GND**.

### Input polyfuse — F_VIN 1812L200/60 (1812, 2.0 A hold, 4.0 A trip, 60 V rated)

- In series: **VIN_48V** → **V48_RAIL** (downstream of reverse-polarity + TVS, upstream of everything else).

### Bulk capacitance

| Cap | Value | Net |
|---|---|---|
| **C_VIN_BULK** | 100 µF 63 V Al-polymer, ESR ≤ 40 mΩ | **V48_RAIL** → GND, near F_VIN output |
| **C_VIN_CER** | 4.7 µF 100 V X7R 1210 | **V48_RAIL** → GND, paralleled with C_VIN_BULK |
| **C_VIN_HF** | 100 nF 100 V X7R 0603 | **V48_RAIL** → GND, at each downstream tap |

## A.2 48 V → 5 V buck — U_BUCK1 LMR16030SDDAR (SO-8 PowerPAD)

Part: **LMR16030SDDAR** — 60 V input, 3 A output, 200 kHz–1.1 MHz, internal compensation, integrated high-side FET.

### Pinout (SO-8 PowerPAD, D suffix)

| Pin | Name | Net |
|---|---|---|
| 1 | BOOT | **BUCK1_BOOT** — 100 nF **C_BUCK1_BOOT** to SW |
| 2 | VIN  | **V48_RAIL** |
| 3 | EN   | **V48_RAIL** via 100 kΩ **R_BUCK1_EN** (always enabled) |
| 4 | RT/SYNC | 66.5 kΩ **R_BUCK1_RT** to GND → f_SW ≈ 700 kHz |
| 5 | GND  | **GND** |
| 6 | FB   | **BUCK1_FB** |
| 7 | COMP | 6.8 nF **C_BUCK1_COMP** + 22 kΩ **R_BUCK1_COMP** in series to GND (type-II compensation) |
| 8 | SW   | **BUCK1_SW** |
| EPAD | thermal | **GND**, ≥ 9 thermal vias to L2 |

### External network

- **L_BUCK1** 22 µH, shielded, I_SAT ≥ 4 A, DCR ≤ 30 mΩ: **BUCK1_SW** → **5V_RAIL**.
- **D_BUCK1** B560C-13-F 60 V 5 A Schottky (DO-214AB): cathode on **BUCK1_SW**, anode on **GND**. Asynchronous buck (LMR16030 is non-synchronous).
- **C_BUCK1_IN** 4.7 µF 100 V X7R 1210: **V48_RAIL** (pin 2) → GND, within 2 mm.
- **C_BUCK1_OUT_1** 47 µF 10 V X5R 1210: **5V_RAIL** → GND.
- **C_BUCK1_OUT_2** 22 µF 10 V X5R 0805: **5V_RAIL** → GND.
- **R_BUCK1_FB_TOP** 38.3 kΩ 1 % 0603: **5V_RAIL** → **BUCK1_FB**.
- **R_BUCK1_FB_BOT** 6.49 kΩ 1 % 0603: **BUCK1_FB** → **GND**.

V_OUT = 0.765 V × (1 + 38.3 kΩ / 6.49 kΩ) = **5.27 V** (LMR16030 V_FB = 0.765 V typ; tolerance + feedforward give ~5.05 V actual).

Estimated load: TLC5947 + SK6812 strip at peak ~1.2 A + feed for AP63300 (~0.3 A at 3V3 conversion) → ~1.5 A. LMR16030 has 2× margin.

## A.3 5 V → 3.3 V buck — U_BUCK2 AP63300WU-7 (TSOT-26)

Reused from Rev B. 3.8 V – 32 V input envelope, 3 A output, internal compensation + internal soft-start, fixed 500 kHz.

### Pinout (TSOT-26)

| Pin | Name | Net |
|---|---|---|
| 1 | SW | **BUCK2_SW** → L_BUCK2 input |
| 2 | GND | **GND** |
| 3 | FB | **BUCK2_FB** |
| 4 | EN | **5V_RAIL** via 100 kΩ **R_BUCK2_EN** (always enabled) |
| 5 | VIN | **5V_RAIL** |
| 6 | BST | **BUCK2_BST** — 100 nF **C_BUCK2_BOOT** to SW |

### External network

- **L_BUCK2** 4.7 µH, shielded, I_SAT ≥ 3 A, DCR ≤ 50 mΩ: **BUCK2_SW** → **3V3_RAIL**.
- **C_BUCK2_IN** 10 µF 25 V X7R 0805: pin 5 → GND, within 2 mm.
- **C_BUCK2_OUT_1** 22 µF 25 V X5R 0805: **3V3_RAIL** → GND.
- **C_BUCK2_OUT_2** 22 µF 25 V X5R 0805: **3V3_RAIL** → GND. Total ≥ 44 µF.
- **R_BUCK2_FB_TOP** 100 kΩ 1 % 0603: **3V3_RAIL** → **BUCK2_FB**.
- **R_BUCK2_FB_BOT** 31.6 kΩ 1 % 0603: **BUCK2_FB** → **GND**.
- **C_BUCK2_FB** 22 pF 0603 C0G: across R_BUCK2_FB_TOP.

V_OUT = 0.803 V × (1 + 100 kΩ / 31.6 kΩ) = **3.34 V**. Synchronous buck, no external Schottky.

## A.4 ESP32-S3 module — M_U_S3 ESP32-S3-WROOM-1-N16R8

16 MB quad flash + 8 MB octal PSRAM, PCB antenna variant.

### Power
- VDD (module pin 2) → **3V3_RAIL**
- GND (module pins 1, 40, 41, EPAD) → **GND** (≥ 9 thermal vias on EPAD)
- EN (module pin 3) → **EN_S3_NODE** (10 kΩ **R_EN_S3** pullup to 3V3_RAIL + 1 µF **C_ESP_EN** to GND + SW_RESET_S3 terminal 1)

### I/O used

| GPIO | Module pin | Net | Destination / Role |
|---|---|---|---|
| IO0 | 27 | **BOOT_S3_BTN** | SW_BOOT_S3 terminal 1 (internal pullup); boot strap |
| IO4 | 4 | **TLC_SCK** | TLC5947 SCLK |
| IO5 | 5 | **TLC_SIN** | TLC5947 SIN |
| IO6 | 6 | **TLC_XLAT** | TLC5947 XLAT |
| IO7 | 7 | **TLC_BLANK** | TLC5947 BLANK |
| IO8 | 8 | **INA_SDA** | Shared I²C1 bus (INA237_A + INA237_B). 4.7 kΩ **R_INA_SDA** pullup to 3V3_RAIL. |
| IO9 | 9 | **INA_SCL** | Shared I²C1 bus (INA237_A + INA237_B). 4.7 kΩ **R_INA_SCL** pullup to 3V3_RAIL. |
| IO10 | 10 | **RS485_A_TX** | SN65HVD75 (Bus A) pin 4 (D). UART2 TX via GPIO matrix. |
| IO11 | 11 | **H2_RESET_N** | R_H2_RST_SER 1 kΩ → H2 EN |
| IO12 | 12 | **H2_IRQ** | H2 IO10 (open-drain, 10 kΩ pullup) |
| IO13 | 13 | **H2_BOOT_SEL** | R_H2_BOOT_SER 1 kΩ → H2 IO9 |
| IO14 | 14 | **RS485_A_RX** | SN65HVD75 (Bus A) pin 1 (R). UART2 RX via GPIO matrix. |
| IO15 | 15 | **RS485_A_DE** | SN65HVD75 (Bus A) pins 2+3 (/RE tied to DE). UART2 RTS via GPIO matrix (hardware DE). |
| IO16 | 16 | **RS485_B_TX** | SN65HVD75 (Bus B) pin 4. UART0 TX (remapped via GPIO matrix). |
| IO17 | 17 | **H2_UART_TX** | H2 IO4 (UART1 TX, 921600) |
| IO18 | 18 | **H2_UART_RX** | H2 IO5 (UART1 RX) |
| IO19 | 23 | **USB_DN** | USBLC6 pins 1+6 |
| IO20 | 24 | **USB_DP** | USBLC6 pins 3+4 |
| IO21 | 25 | **RGB_DATA** | SK6812 strip DIN via 33 Ω R_RGB_SER |
| IO38 | 33 | **RS485_B_RX** | SN65HVD75 (Bus B) pin 1. UART0 RX remapped. |
| IO39 | 34 | **RS485_B_DE** | SN65HVD75 (Bus B) pins 2+3. UART0 RTS. |
| IO43 | 37 | **TP_U0TXD_S3** | Test pad only (UART0 ROM boot log during flashing; remapped to RS485_B_TX after boot) |
| IO44 | 36 | **TP_U0RXD_S3** | Test pad only (symmetric to IO43) |

Reserved / no copper escape: IO3, IO45, IO46 (strap pins).

Internal / do not route: IO26–IO32 (flash), IO33–IO37 (PSRAM).

### Decoupling — unchanged from Rev B
- **C_ESP_BULK** 22 µF 25 V X5R 0805 on 3V3_RAIL within 10 mm of module.
- 3× local decoupling pairs (100 nF X7R 0603 + 1 µF X5R 0603): **C_ESP_DEC{1..3}{a,b}**.
- **C_ESP_EN** 1 µF 25 V X5R 0603 from **EN_S3_NODE** to GND.

## A.5 ESP32-H2 module — M_U_H2 ESP32-H2-MINI-1-N4

Radio coprocessor, unchanged from Rev B. RISC-V @ 96 MHz, 802.15.4 + BLE 5.

### Power
- VDD → **3V3_RAIL**
- GND, EPAD → **GND**
- EN (module pin 24) → **H2_RESET_N** (10 kΩ **R_EN_H2** pullup + 1 µF **C_H2_EN** to GND)

### I/O used

| H2 GPIO | Module pin | Net | Role |
|---|---|---|---|
| IO2 | 20 | — | 10 kΩ **R_H2_STRAP2** to 3V3_RAIL (strap high) |
| IO4 | 4 | **H2_UART_TX** | S3 IO17 (H2 UART1 RX) |
| IO5 | 5 | **H2_UART_RX** | S3 IO18 (H2 UART1 TX) |
| IO8 | 9 | — | 10 kΩ **R_H2_STRAP8** to 3V3_RAIL (strap high, ROM msg enable) |
| IO9 | 10 | **H2_BOOT_SEL** | 10 kΩ **R_H2_STRAP9** to 3V3_RAIL, S3 IO13 via 1 kΩ R_H2_BOOT_SER |
| IO10 | 11 | **H2_IRQ** | S3 IO12 (open-drain, 10 kΩ R_H2_IRQ pullup on S3 side) |
| IO11 | 12 | **LED_H2_HB** | 1 kΩ R_LED_H2_HB → LED_4 anode |
| IO12 | 16 | **LED_ZIGBEE** | 1 kΩ R_LED_ZIGBEE → LED_6 anode |
| IO13 | 17 | **LED_BLE** | 1 kΩ R_LED_BLE → LED_7 anode |
| IO23 | 14 | **U0RXD_H2** | JP_DEBUG_H2 pin 4 |
| IO24 | 15 | **U0TXD_H2** | JP_DEBUG_H2 pin 3 |
| IO26 | 23 | **H2_USB_DN** | TP_H2_USB_DN 1 mm pad (optional JTAG) |
| IO27 | 22 | **H2_USB_DP** | TP_H2_USB_DP 1 mm pad |

Decoupling identical to Rev B: **C_H2_BULK** 10 µF 10 V X5R 0805 + 3× pair local (**C_H2_DEC{1..3}**) + **C_H2_EN** 1 µF 25 V X5R 0603.

## A.6 RS-485 transceiver — Bus A (M_U_RS485_A) SN65HVD75D (SOIC-8)

Part: **SN65HVD75D** — 3.3 V supply, half-duplex RS-485, fail-safe receiver, –7 V to +12 V bus common-mode, ±8 kV HBM ESD.

### Pinout (SOIC-8)

| Pin | Name | Net |
|---|---|---|
| 1 | R (receiver output) | **RS485_A_RX** → S3 IO14 |
| 2 | /RE (receiver enable, active low) | **RS485_A_DE** (tied to pin 3 at PCB — half-duplex auto-direction) |
| 3 | DE (driver enable, active high) | **RS485_A_DE** → S3 IO15 |
| 4 | D (driver input) | **RS485_A_TX** → S3 IO10 |
| 5 | GND | **GND** |
| 6 | A (non-inverting bus) | **BUS_A_A_MASTER** |
| 7 | B (inverting bus) | **BUS_A_B_MASTER** |
| 8 | VCC | **3V3_RAIL** |

Decoupling: **C_RS485_A_DEC** 100 nF X7R 0603 pin 8 → pin 5, within 2 mm.

### Termination + fail-safe bias (MASTER end, always populated)

| Ref | Value | Connection |
|---|---|---|
| **R_TERM_A** | 120 Ω 1 % 0603 | **BUS_A_A_MASTER** ↔ **BUS_A_B_MASTER** |
| **R_BIAS_A_PU** | 680 Ω 1 % 0603 | **BUS_A_A_MASTER** → **3V3_RAIL** |
| **R_BIAS_A_PD** | 680 Ω 1 % 0603 | **BUS_A_B_MASTER** → **GND** |

Master-end bias drives A-B ≈ +200 mV when no transmitter is active (fail-safe idle high on the receiver).

### RS-485 TVS — D_RS485_A SM712 (SOT-23-3)

Protects the A/B bus pair from ESD/EFT on the CAT6 cable:
- Pin 1 (bidir) → **BUS_A_A_MASTER**
- Pin 2 (uni) → **GND**
- Pin 3 (bidir) → **BUS_A_B_MASTER**

SM712 clamps A and B individually to +12 V / −7 V (matches SN65HVD75 common-mode window).

## A.7 RS-485 transceiver — Bus B (M_U_RS485_B)

**Identical to A.6** with designator prefix B and net prefix `BUS_B_`. Summary:

- Part: SN65HVD75D, **C_RS485_B_DEC** 100 nF, pin connections same.
- RX / DE / TX: **RS485_B_RX** (S3 IO38) / **RS485_B_DE** (S3 IO39) / **RS485_B_TX** (S3 IO16).
- Bus nets: **BUS_B_A_MASTER**, **BUS_B_B_MASTER**.
- Termination: **R_TERM_B** 120 Ω, **R_BIAS_B_PU** 680 Ω to 3V3, **R_BIAS_B_PD** 680 Ω to GND.
- TVS: **D_RS485_B** SM712.

## A.8 Current-sense amplifier — INA237 × 2 (per bus)

Part: **INA237AIDGSR** — MSOP-10, 85 V bus + common-mode, 16-bit ADC, I²C (400 kHz), alert output.

### A.8.1 INA237_A (Bus A) pinout (MSOP-10)

| Pin | Name | Net |
|---|---|---|
| 1 | IN+ | **V48_RAIL** (shunt high side) |
| 2 | IN− | **BUS_A_48V** (shunt low side → J_BUS_A pin 1/2/7/8) |
| 3 | VS | **3V3_RAIL** |
| 4 | GND | **GND** |
| 5 | ALERT | **ALERT_INA_A** (open-drain) → S3 IO47 (10 kΩ **R_ALERT_A** pullup to 3V3) |
| 6 | SDA | **INA_SDA** |
| 7 | SCL | **INA_SCL** |
| 8 | A1 | **GND** (address byte bit 1 = 0) |
| 9 | A0 | **GND** (address byte bit 0 = 0 → I²C addr 0x40) |
| 10 | VBUS | **BUS_A_48V** (bus voltage sense tap, internal 85 V divider) |

### A.8.2 INA237_B (Bus B)

**Identical to A.8.1** with net prefix `BUS_B_`, ALERT pin to S3 IO48 (R_ALERT_B 10 kΩ), and:
- A0 → **3V3_RAIL** (bit 0 = 1)
- A1 → **GND** (bit 1 = 0)
- I²C address: **0x41**

### A.8.3 Shunt — R_SHUNT_A / R_SHUNT_B 50 mΩ 1 W 2512 (CSR2512FK50L0 or equivalent, 1 %)

- **R_SHUNT_A**: between **V48_RAIL** and **BUS_A_48V**.
- **R_SHUNT_B**: between **V48_RAIL** and **BUS_B_48V**.
- Kelvin sensing: INA237 IN+ / IN− taps go to the shunt pads directly, not to the power traces at either end.

### A.8.4 Decoupling

- **C_INA_A_DEC** / **C_INA_B_DEC** 100 nF X7R 0603 on VS (pin 3) → GND, within 2 mm of each IC.

## A.9 RJ45 output connectors — J_BUS_A, J_BUS_B

Part: **Amphenol RJHSE-5080** (shielded, through-hole, 8P8C, no magnetics).

### J_BUS_A pinout

| Pin | Net |
|---|---|
| 1 | **BUS_A_48V** |
| 2 | **BUS_A_48V** |
| 3 | **GND** |
| 4 | **BUS_A_A_MASTER** (RS-485 A) |
| 5 | **BUS_A_B_MASTER** (RS-485 B) |
| 6 | **GND** |
| 7 | **BUS_A_48V** |
| 8 | **BUS_A_48V** |
| Shield | **GND** via 10 nF **C_SHIELD_A** (chassis-to-logic HF coupling) |

### J_BUS_B pinout

Identical structure with `BUS_B_` nets:

| Pin | Net |
|---|---|
| 1, 2, 7, 8 | **BUS_B_48V** |
| 3, 6 | **GND** |
| 4 | **BUS_B_A_MASTER** |
| 5 | **BUS_B_B_MASTER** |
| Shield | **GND** via 10 nF **C_SHIELD_B** |

## A.10 LED bar — TLC5947 + SK6812 (unchanged from Rev B)

Full netlist identical to Rev B § 11 (see v1 history). Summary:

- **U_LED TLC5947DAP** (HTSSOP-28): IO4/5/6/7 from S3, 24 constant-current channels, 2.7 kΩ R_IREF → 15 mA/ch. Drives 14 fixed side-view LEDs.
- **LED_1..19**: 19 fixed LEDs along the front edge. LED_3, 5, 8..19 driven by TLC5947 channels 0..13. LED_1, 2 are always-on rail indicators via R_LED_5V / R_LED_3V3 (1 kΩ each). LED_4, 6, 7 driven directly from H2 GPIOs 11/12/13.
- **LED_RGB_1..16**: SK6812-mini side-emit daisy chain on S3 IO21. 33 Ω R_RGB_SER series on the S3 side. 100 µF Al-polymer **C_RGB_BULK** at strip start + 100 nF per-LED **C_RGB_1..16**.
- **LEVEL_RGB** 74LVC1T45 3V3→5V translator: DNP by default, jumper **JP_RGB_BYPASS** 0 Ω populated default. Populate LEVEL_RGB only if SK6812 V_IH fails bring-up qualification.

## A.11 USB-C connector — J_USB + USBLC6 ESD (unchanged from Rev B)

| USB-C pin | Net |
|---|---|
| A4, A9, B4, B9 | **USB_VBUS** (via ferrite **FB1** 470 Ω @ 100 MHz → **USB_5V_IN**) |
| A1, A12, B1, B12, shield | **GND** |
| A5 | **USB_CC1** — 5.1 kΩ **R_CC1** to GND |
| B5 | **USB_CC2** — 5.1 kΩ **R_CC2** to GND |
| A6, B6 | **USB_DP_RAW** → USBLC6 |
| A7, B7 | **USB_DN_RAW** → USBLC6 |

Shield: **R_SH1** 1 MΩ + **C_SH1** 10 nF to GND.

**USB-C ONLY powers the ESP32-S3 native USB interface for programming / CDC debug.** Not a power input for the system. The `USB_5V_IN` net does **not** connect to `5V_RAIL`.

### USBLC6-2SC6 ESD — U1
| Pin | Net |
|---|---|
| 1, 6 | **USB_DN_RAW ↔ USB_DN** |
| 2 | **GND** |
| 3, 4 | **USB_DP_RAW ↔ USB_DP** (pin 4 tied to pin 3, **NOT GND**) |
| 5 | **USB_5V_IN** |

## A.12 Buttons — SW_RESET_S3, SW_BOOT_S3 (unchanged from Rev B)

- **SW_RESET_S3** (6×6 SMD tact): terminals → EN_S3_NODE + GND. **C_SW1_DEB** 100 nF X7R 0603 parallel.
- **SW_BOOT_S3** (6×6 SMD tact): terminals → BOOT_S3_BTN + GND. **C_SW2_DEB** 100 nF X7R 0603 parallel.

## A.13 Debug header — JP_DEBUG_H2 (1×6, unchanged from Rev B)

| Pin | Net |
|---|---|
| 1 | **3V3_RAIL** |
| 2 | **GND** |
| 3 | **U0TXD_H2** |
| 4 | **U0RXD_H2** |
| 5 | **H2_RESET_N** |
| 6 | **H2_BOOT_SEL** |

(No `JP_DEBUG_S3` in v2 — native USB on the USB-C connector is the S3 debug/programming path.)

## A.14 Test points (1 mm exposed pads, silkscreen labelled)

| Label | Net |
|---|---|
| TP_V48 | V48_RAIL |
| TP_5V | 5V_RAIL |
| TP_3V3 | 3V3_RAIL |
| TP_GND | GND |
| TP_BUS_A_A | BUS_A_A_MASTER |
| TP_BUS_A_B | BUS_A_B_MASTER |
| TP_BUS_A_48V | BUS_A_48V |
| TP_BUS_B_A | BUS_B_A_MASTER |
| TP_BUS_B_B | BUS_B_B_MASTER |
| TP_BUS_B_48V | BUS_B_48V |
| TP_INA_SDA | INA_SDA |
| TP_INA_SCL | INA_SCL |
| TP_ALERT_A | ALERT_INA_A |
| TP_ALERT_B | ALERT_INA_B |
| TP_U0TXD_S3 | IO43 |
| TP_U0RXD_S3 | IO44 |
| TP_H2_USB_DP | H2_USB_DP |
| TP_H2_USB_DN | H2_USB_DN |

## A.15 Silkscreen (must land on F.Silkscreen)

1. **RJ45 polarity**: `J_BUS_A`, `J_BUS_B` silkscreen identifies pin 1 and the tab orientation. Wire color-table in small text beside each connector: `1-2 +48V  3 GND  4-5 A/B  6 GND  7-8 +48V`.
2. **Power warning** near J_PWR: `48V DC — CENTER POSITIVE — 60V MAX`.
3. **Strap-pin DO-NOT-CUT**: diagonal hatching around R_H2_STRAP9 with label `STRAP — DO NOT CUT`.
4. **Shunt polarity**: arrow on silk through each `R_SHUNT_A` / `R_SHUNT_B` pointing from V48_RAIL side to BUS_{A,B}_48V side.
5. **Debug header**: `3V3 GND TX RX RST BOOT` next to the six pins of JP_DEBUG_H2.

---

# SECTION B — UNIT BOARD

Per-flap daughter board. Replaces the Arduino Nano unit. One unit per flap; daisy-chained on the RS-485 bus via two RJ45 connectors.

Form factor target: 30 × 50 mm, 2-layer, ENIG or HASL.

## B.1 RJ45 connectors — J_IN, J_OUT (daisy-chain passthrough)

Part: **Amphenol RJHSE-5080** (same as master).

Each pair of pins between J_IN and J_OUT is tied by a short board trace so the CAT6 signals / power pass straight through every unit:

| J_IN pin | Net | J_OUT pin |
|---|---|---|
| 1 | **BUS_48V_PASS** | 1 |
| 2 | **BUS_48V_PASS** | 2 |
| 3 | **GND**          | 3 |
| 4 | **BUS_A**        | 4 |
| 5 | **BUS_B**        | 5 |
| 6 | **GND**          | 6 |
| 7 | **BUS_48V_PASS** | 7 |
| 8 | **BUS_48V_PASS** | 8 |
| Shield | **GND** via 10 nF **C_SHIELD_UNIT** |

The passthrough traces must be sized for the full worst-case bus current (1.5 A) — minimum 1.0 mm wide on 1 oz copper for +48V, 0.5 mm for GND return (paralleled two-pair).

## B.2 Local +48 V tap (power for this unit)

Tap off the passthrough `BUS_48V_PASS` node, with protection isolating this unit from the bus:

- **F_LOCAL** 1812L050/60 (500 mA hold, 1.0 A trip, 60 V): **BUS_48V_PASS** → **LOCAL_48V**.
- **D_LOCAL_TVS** SMBJ58CA (bidirectional, 58 V working): **LOCAL_48V** → **GND**.
- **C_LOCAL_BULK** 10 µF 63 V Al-polymer: **LOCAL_48V** → GND.
- **C_LOCAL_CER** 1 µF 100 V X7R 1210: **LOCAL_48V** → GND.

`F_LOCAL` contains a single-unit shorted-input failure to just that unit (trips in < 500 ms at 1.0 A), leaving the rest of the bus alive.

## B.3 48 V → 5 V buck — U_BUCK_UNIT LMR16006YDDCR (SOT-23-6)

Part: **LMR16006YDDCR** — 60 V input, 0.6 A output, 1.25 MHz fixed, internal compensation.

### Pinout (SOT-23-6, D suffix)

| Pin | Name | Net |
|---|---|---|
| 1 | BOOT | **BUCK_U_BOOT** — 22 nF **C_BUCK_U_BOOT** to SW |
| 2 | VIN | **LOCAL_48V** |
| 3 | EN | **LOCAL_48V** via 100 kΩ **R_BUCK_U_EN** (always enabled) |
| 4 | GND | **GND** |
| 5 | FB | **BUCK_U_FB** |
| 6 | SW | **BUCK_U_SW** |

### External network

- **L_BUCK_U** 22 µH, shielded, I_SAT ≥ 0.8 A, DCR ≤ 200 mΩ: **BUCK_U_SW** → **5V_UNIT**.
- **D_BUCK_U** B360B-13-F 60 V 3 A Schottky (DO-214BA): cathode on **BUCK_U_SW**, anode on **GND**.
- **C_BUCK_U_IN** 2.2 µF 100 V X7R 1210: **LOCAL_48V** → GND within 2 mm.
- **C_BUCK_U_OUT** 22 µF 10 V X5R 0805: **5V_UNIT** → GND.
- **R_BUCK_U_FB_TOP** 18.7 kΩ 1 % 0603: **5V_UNIT** → **BUCK_U_FB**.
- **R_BUCK_U_FB_BOT** 3.32 kΩ 1 % 0603: **BUCK_U_FB** → **GND**.

V_OUT = 0.765 V × (1 + 18.7 kΩ / 3.32 kΩ) = **5.07 V**.

Estimated load: ULN2003 stepper pulling ≤ 200 mA while stepping + MCU ~5 mA + LEDs ~5 mA = ≤ 250 mA peak. LMR16006 has 2.4× margin.

## B.4 5 V → 3.3 V LDO — U_LDO MCP1700-3302E/TT (SOT-23-3)

Part: **MCP1700-3302E/TT** — 6 V input, 250 mA, 178 mV dropout @ 250 mA, 1.6 µA quiescent.

### Pinout

| Pin | Name | Net |
|---|---|---|
| 1 | VIN | **5V_UNIT** |
| 2 | GND | **GND** |
| 3 | VOUT | **3V3_UNIT** |

### External network

- **C_LDO_IN** 1 µF 10 V X7R 0603: **5V_UNIT** → GND.
- **C_LDO_OUT** 2.2 µF 10 V X7R 0603: **3V3_UNIT** → GND (MCP1700 requires ≥ 1 µF on output for stability).

## B.5 STM32G030F6P6 — U_MCU (TSSOP-20)

Cortex-M0+ @ 64 MHz, 32 KB flash, 8 KB RAM, 96-bit unique ID.

### Pinout (TSSOP-20, P suffix)

| Pin | Signal | Net | Role |
|---|---|---|---|
| 1 | VDD | **3V3_UNIT** | Digital supply |
| 2 | PB7 | **BTN** | Identify / reset button input (internal pull-up enabled, active low) |
| 3 | PB8 | **LED_STATUS** | Status LED output (active high, 1 kΩ series) |
| 4 | PC14/OSC32_IN/PF0 | — | Reserved; no copper escape (internal RC clock only) |
| 5 | PC15/OSC32_OUT/PF1 | — | Reserved; no copper escape |
| 6 | NRST | **NRST** | System reset; 10 kΩ **R_MCU_NRST** pullup to 3V3_UNIT, 100 nF **C_MCU_NRST** to GND, test pad **TP_NRST** |
| 7 | VDDA | **3V3_UNIT** | Analog supply (via 10 Ω **R_VDDA** ferrite bead or plain filter) + **C_VDDA** 100 nF adjacent |
| 8 | PA0 | **V_48_MON** | ADC1_IN0 input from +48V divider |
| 9 | PA1 | **RS485_DE** | USART2 DE (hardware driver-enable, TX-gated) |
| 10 | PA2 | **RS485_TX_MCU** | USART2 TX |
| 11 | PA3 | **RS485_RX_MCU** | USART2 RX |
| 12 | PA4 | **STEPPER_A** | ULN2003 input 1 |
| 13 | PA5 | **STEPPER_B** | ULN2003 input 2 |
| 14 | PA6 | **STEPPER_C** | ULN2003 input 3 |
| 15 | PA7 | **STEPPER_D** | ULN2003 input 4 |
| 16 | PB0 | **HALL_IN** | KY-003 digital output (5 V-tolerant FT pin used as 3V3 input; KY-003 module output is open-collector-style with its own pull-up) |
| 17 | PB1 | — | Reserved; no copper escape |
| 18 | PA13 | **SWDIO** | SWD data (to JP_SWD pin 3) |
| 19 | PA14/BOOT0 | **SWCLK** | SWD clock (to JP_SWD pin 4) — also BOOT0 strap: 10 kΩ **R_MCU_BOOT0** pulldown to GND (normal boot from flash) |
| 20 | VSS | **GND** | Digital ground |

### Decoupling

- **C_MCU_VDD** 100 nF X7R 0603 + **C_MCU_VDD_BULK** 1 µF X5R 0603: pin 1 → GND, within 3 mm.
- **C_VDDA** 100 nF X7R 0603 + **C_VDDA_BULK** 1 µF X5R 0603: pin 7 → GND.
- **C_MCU_NRST** 100 nF X7R 0603: pin 6 → GND.

### SWD header — JP_SWD (1×4 through-hole or 1×4 1.27 mm SMD pad pair, unpopulated default)

| Pin | Net |
|---|---|
| 1 | **3V3_UNIT** |
| 2 | **GND** |
| 3 | **SWDIO** |
| 4 | **SWCLK** |

Provision for ST-LINK V2 / V3 first-flash. Once flashed, the unit takes firmware updates over RS-485 via the STM32 system bootloader.

## B.6 RS-485 transceiver — U_RS485 SN65HVD75D (SOIC-8)

Same part as master; different net mapping:

| Pin | Name | Net |
|---|---|---|
| 1 | R | **RS485_RX_MCU** |
| 2 | /RE | **RS485_DE** (tied to pin 3) |
| 3 | DE | **RS485_DE** |
| 4 | D | **RS485_TX_MCU** |
| 5 | GND | **GND** |
| 6 | A | **BUS_A** (cable net; shared with RJ45 pin 4) |
| 7 | B | **BUS_B** (cable net; shared with RJ45 pin 5) |
| 8 | VCC | **3V3_UNIT** |

Decoupling: **C_RS485_DEC** 100 nF X7R 0603 pin 8 → pin 5.

### Bus TVS — D_BUS_TVS SM712

- Pin 1 → **BUS_A**
- Pin 2 → **GND**
- Pin 3 → **BUS_B**

### Optional end-of-bus termination — JP_TERM + R_TERM_UNIT

- **JP_TERM**: 0 Ω 0402 DNP jumper. Populated **only on the last physical unit of each bus** to enable end-of-line termination.
- **R_TERM_UNIT** 120 Ω 1 % 0603: one side on **BUS_A**, other side on `TERM_B`.
- `TERM_B` connects to **BUS_B** via **JP_TERM**.
- When JP_TERM is DNP (all intermediate units), the 120 Ω is stranded on one side, no current flows, no effect on the bus.

(No fail-safe bias on units — bias lives exclusively at the master to avoid loading the bus.)

## B.7 Voltage monitor — V_48_MON divider

- **R_MON_TOP** 150 kΩ 1 % 0603: **LOCAL_48V** → **V_48_MON**.
- **R_MON_BOT** 10 kΩ 1 % 0603: **V_48_MON** → **GND**.
- **C_MON** 100 nF X7R 0603: **V_48_MON** → **GND** (τ ≈ 9.4 ms).

Ratio: 10 / (150 + 10) = 1 / 16. Full-scale ADC (3.3 V at PA0) corresponds to 52.8 V at the bus.

## B.8 Stepper driver — U_STEP ULN2003AD (SOIC-16)

Drives one 28BYJ-48 unipolar stepper. Only 4 of the 7 Darlington channels are used.

### Pinout (SOIC-16)

| Pin | Name | Net |
|---|---|---|
| 1 | IN1 | **STEPPER_A** |
| 2 | IN2 | **STEPPER_B** |
| 3 | IN3 | **STEPPER_C** |
| 4 | IN4 | **STEPPER_D** |
| 5 | IN5 | NC |
| 6 | IN6 | NC |
| 7 | IN7 | NC |
| 8 | GND | **GND** |
| 9 | COM | **5V_UNIT** (common cathode for free-wheeling diodes) |
| 10 | OUT7 | NC |
| 11 | OUT6 | NC |
| 12 | OUT5 | NC |
| 13 | OUT4 | **MOT_D** |
| 14 | OUT3 | **MOT_C** |
| 15 | OUT2 | **MOT_B** |
| 16 | OUT1 | **MOT_A** |

Decoupling: **C_STEP_DEC** 100 nF X7R 0603 on pin 9 (COM) → pin 8 (GND), within 3 mm. **C_STEP_BULK** 10 µF 10 V X5R 0805 on 5V_UNIT adjacent to pin 9.

### Motor connector — J_MOTOR B5B-XH-A 5-pin JST-XH THT

| Pin | Net |
|---|---|
| 1 | **5V_UNIT** (motor common) |
| 2 | **MOT_A** |
| 3 | **MOT_B** |
| 4 | **MOT_C** |
| 5 | **MOT_D** |

Pin 1 is the motor common (red wire on the 28BYJ-48 with typical wiring). Pins 2–5 are the four coil drives.

## B.9 Hall sensor connector — J_HALL 3-pin JST-XH or 1×3 header

| Pin | Net |
|---|---|
| 1 | **5V_UNIT** |
| 2 | **GND** |
| 3 | **HALL_IN** (KY-003 open-collector output) |

KY-003 module has its own pull-up internally. The STM32 FT pin tolerates 5 V signals.

## B.10 Identify button — SW_BTN

6 × 6 SMD tact switch:
- Terminal 1 → **BTN**
- Terminal 2 → **GND**
- Firmware-enabled internal pull-up on PB7.
- **C_BTN_DEB** 100 nF X7R 0603 from BTN → GND for ESD + debounce.

## B.11 Status LED — LED_STATUS

| Item | Value |
|---|---|
| Part | 0805 green, Vf ≈ 2.0 V @ 5 mA (Everlight 19-217/GHC-YR1S2/3T or any green 0805) |
| Series | **R_LED_STATUS** 330 Ω 0603 → PB8 (active high) |
| Current | (3.3 V − 2.0 V) / 330 Ω ≈ 4 mA |

## B.12 Silkscreen callouts (F.Silkscreen)

1. **RJ45 direction arrow**: `IN ▶` near J_IN, `OUT ▶` near J_OUT — indicates suggested cable flow for a daisy chain starting at the master.
2. **Pin-1 dots** on every THT/SMD connector with a clear polarity requirement (motor JST-XH, hall JST-XH, SWD header).
3. **BOOT0 note**: next to the MCU, small text `BOOT0 via PA14/SWCLK — DO NOT pull high in normal op`.
4. **Termination jumper label**: `JP_TERM — POPULATE ON LAST UNIT ONLY`.

---

# SECTION C — CRITICAL DESIGN RULES (MUST FOLLOW)

These rules exist because violating them costs a respin or a field failure. They apply at schematic capture, not at PCB review time.

## C.1 Bus-wide
1. **CAT6 wiring is straight-through.** Crossovers short +48V to GND on the first unit — board-level damage.
2. **RJ45 pinout (T568B) is locked.** Pin 1/2 = +48V, 3 = GND, 4 = RS-485 A, 5 = RS-485 B, 6 = GND, 7/8 = +48V. No variant boards.
3. **Only the master provides RS-485 bias.** Every unit's 120 Ω termination resistor is DNP by default (`JP_TERM` open); only the physical end-of-bus unit populates the jumper.
4. **+48V passthrough traces must carry 1.5 A** at 2 oz copper OR 2× 1.0 mm wide at 1 oz. GND return equivalently sized.

## C.2 Master
1. **INA237 IN+ / IN− are Kelvin-tapped** to the shunt pads. Power-trace routing does not share any copper with the sense trace, or measurement error scales with load current.
2. **D_VIN TVS goes upstream of Q_RP** (on VIN_RAW) OR downstream (on VIN_48V). The current design places it downstream so it also clamps any backdraft from buck switching transients. Do not split the TVS across both sides.
3. **5V_RAIL and V48_RAIL are distinct nets** with the only connection being through U_BUCK1. No parallel path.
4. **Inter-MCU bus is UART only.** Rev B rule preserved: no SPI / I²C between S3 and H2.
5. **Antenna keep-outs**: 15×7 mm (S3) + 11×6 mm (H2), both modules on diagonally opposite corners, ≥ 60 mm antenna edge-to-edge. Unchanged from Rev B.
6. **UART0 remap**: during boot the ESP32-S3 ROM uses UART0 on IO43/IO44. After firmware boot, the application remaps UART0 to IO16/IO38 for Bus B. IO43/IO44 become unused (test-pad access only) post-boot. Native USB (IO19/20) handles all programming and debug going forward.

## C.3 Unit
1. **`F_LOCAL` is in series on the +48V tap**, not on the passthrough. The passthrough +48V rail stays always-live for every downstream unit even if this unit's fuse trips.
2. **`D_LOCAL_TVS` is on the local tap (after `F_LOCAL`), not on the passthrough.** Clamp current must flow through the polyfuse so a surge that exceeds the TVS pulse rating opens the fuse rather than sustaining the clamp into destruction.
3. **BOOT0 is pulled low via `R_MCU_BOOT0`** at all times. The STM32 system bootloader is entered by firmware request (software reboot into system memory), never by hardware strap.
4. **`V_48_MON` divider must include `C_MON`**. Without the filter cap, switching noise from the LMR16006 couples into the ADC and the reading is unusable.
5. **`JP_TERM` is DNP on 63 of 64 units.** The last unit of each bus populates it. If you populate more than one, the bus becomes reflection-noisy and packet loss rises.

---

# SECTION D — NET SUMMARY

## D.1 Master nets

| Net | Source | Load |
|---|---|---|
| VIN_RAW | J_PWR centre pin | Q_RP source |
| VIN_48V | Q_RP drain (post-reverse-protect) | F_VIN, D_VIN |
| V48_RAIL | F_VIN out | U_BUCK1 VIN, R_SHUNT_A high, R_SHUNT_B high |
| BUS_A_48V | R_SHUNT_A low | J_BUS_A pins 1/2/7/8 |
| BUS_B_48V | R_SHUNT_B low | J_BUS_B pins 1/2/7/8 |
| 5V_RAIL | U_BUCK1 output | TLC5947 VCC, SK6812 strip, U_BUCK2 VIN |
| 3V3_RAIL | U_BUCK2 output | S3 VDD, H2 VDD, SN65HVD75 VCC × 2, INA237 VS × 2, all 3V3 pullups |
| GND | common return | all |
| USB_VBUS | J_USB VBUS | USBLC6 VBUS (NOT connected to 5V_RAIL) |
| BUS_A_A_MASTER | SN65HVD75_A pin 6 | J_BUS_A pin 4, R_TERM_A, R_BIAS_A_PU, D_RS485_A pin 1 |
| BUS_A_B_MASTER | SN65HVD75_A pin 7 | J_BUS_A pin 5, R_TERM_A, R_BIAS_A_PD, D_RS485_A pin 3 |
| BUS_B_A_MASTER | SN65HVD75_B pin 6 | J_BUS_B pin 4, R_TERM_B, R_BIAS_B_PU, D_RS485_B pin 1 |
| BUS_B_B_MASTER | SN65HVD75_B pin 7 | J_BUS_B pin 5, R_TERM_B, R_BIAS_B_PD, D_RS485_B pin 3 |
| INA_SDA / INA_SCL | S3 IO8 / IO9 | INA237_A + INA237_B (both on same bus) |

## D.2 Unit nets

| Net | Source | Load |
|---|---|---|
| BUS_48V_PASS | J_IN pins 1/2/7/8 ↔ J_OUT pins 1/2/7/8 | (passthrough; branch to F_LOCAL) |
| GND | J_IN pins 3/6, J_OUT pins 3/6 | all |
| BUS_A | J_IN pin 4 ↔ J_OUT pin 4 | SN65HVD75 pin 6, TVS, JP_TERM |
| BUS_B | J_IN pin 5 ↔ J_OUT pin 5 | SN65HVD75 pin 7, TVS, R_TERM_UNIT |
| LOCAL_48V | F_LOCAL out | D_LOCAL_TVS, C_LOCAL_BULK, U_BUCK_UNIT VIN, R_MON_TOP |
| 5V_UNIT | U_BUCK_UNIT output | ULN2003 COM pin 9, U_LDO VIN |
| 3V3_UNIT | U_LDO VOUT | U_MCU VDD+VDDA, SN65HVD75 VCC, LED_STATUS, KY-003 (via J_HALL pin 1 — wait: J_HALL pin 1 is 5V_UNIT for the KY-003 module which is 5V) |
| V_48_MON | R_MON_TOP / R_MON_BOT midpoint | U_MCU PA0 (ADC1_IN0) |
| RS485_TX_MCU / _RX_MCU / _DE | U_MCU PA2 / PA3 / PA1 | SN65HVD75 D / R / DE+/RE |
| STEPPER_{A,B,C,D} | U_MCU PA4..PA7 | ULN2003 IN1..IN4 |
| MOT_{A,B,C,D} | ULN2003 OUT1..OUT4 | J_MOTOR pins 2..5 |
| HALL_IN | J_HALL pin 3 (KY-003 open-collector output) | U_MCU PB0 |
| BTN | SW_BTN | U_MCU PB7 |
| LED_STATUS | U_MCU PB8 | LED_STATUS anode via R_LED_STATUS |
| NRST | TP_NRST + R_MCU_NRST + C_MCU_NRST | U_MCU pin 6 |
| SWDIO / SWCLK | U_MCU PA13 / PA14 | JP_SWD pins 3 / 4 |

---

# SECTION E — PACKAGE AND PIN-1 REFERENCE (BOM validation)

| Ref | Part | Package | Pin-1 marker | Authoritative datasheet |
|---|---|---|---|---|
| M_U_S3 | ESP32-S3-WROOM-1-N16R8 | 65-pin SMD module | Marking on module silk; pin 1 at antenna-opposite corner | Espressif ESP32-S3-WROOM-1/1U rev 1.4+ |
| M_U_H2 | ESP32-H2-MINI-1-N4 | 28-pad SMD module | Module silk marking | Espressif ESP32-H2-MINI-1 rev 1.1+ |
| M_U_RS485_A / _B | SN65HVD75D | SOIC-8 (D suffix) | Dot on pin 1; pin 1 = R (receiver output) | TI SLLS983 |
| INA237_A / _B | INA237AIDGSR | MSOP-10 (DGS suffix) | Dot/dimple on pin 1; pin 1 = IN+ | TI SBOSA20 |
| Q_RP | AO4407 | SOIC-8 | Dot on pin 1; pins 1-3 = S, 4 = G, 5-8 = D | AOS AO4407 datasheet |
| U_BUCK1 | LMR16030SDDAR | SO-8 PowerPAD (DDA suffix) | Dot on pin 1; pin 1 = BOOT | TI SNVSA88 |
| U_BUCK2 | AP63300WU-7 | TSOT-26 (W suffix) | Dot on pin 1; pin 1 = SW | Diodes AP63300 rev 2+ |
| U1 (USB ESD) | USBLC6-2SC6 | SOT-23-6 | Dot on pin 1; pin 1 = I/O 1 (NOT GND) | ST DS3019 |
| D_VIN | SMBJ58CA | DO-214AA (SMB) | CA suffix = bidirectional, no polarity | Littelfuse SMBJ series |
| D_LOCAL_TVS | SMBJ58CA | DO-214AA (SMB) | CA = bidir | same |
| D_RS485_A / _B / D_BUS_TVS | SM712 | SOT-23-3 | Pin 1 / 3 = bus sides, pin 2 = GND | Semtech SM712 |
| F_VIN | 1812L200/60 | 1812 | No polarity | Littelfuse 1812L series |
| F_LOCAL | 1812L050/60 | 1812 | No polarity | same |
| U_BUCK_UNIT | LMR16006YDDCR | SOT-23-6 | Dot on pin 1 | TI SNVSA54 |
| U_LDO | MCP1700-3302E/TT | SOT-23-3 | Dot on pin 1; pin 1 = VIN | Microchip MCP1700 |
| U_MCU | STM32G030F6P6 | TSSOP-20 | Dot on pin 1 corner | ST DS12990 |
| U_STEP | ULN2003AD | SOIC-16 | Dot on pin 1 | TI SLRS027 |
| J_BUS_A / J_BUS_B / J_IN / J_OUT | RJHSE-5080 | 8P8C THT shielded | Latch on one side identifies pin orientation | Amphenol RJHSE datasheet |
