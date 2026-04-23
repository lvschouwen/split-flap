# PCB v2 — GPIO pinouts

Full GPIO allocation for every programmable device in the v2 system: the master's ESP32-S3 and ESP32-H2, and the unit's STM32G030F6P6. Also the locked RJ45 cable pinout and peripheral pin maps (SN65HVD75, INA237) for cross-reference during capture and bring-up.

> This file is firmware-facing. For full electrical netlists (passives, protection, power topology), see [`SCHEMATIC.md`](./SCHEMATIC.md). For physical connector pin-1 orientation, see the silkscreen callouts at the end of each SCHEMATIC section.

## Conventions

- **Logical addresses**: 1-byte, 0x01..0x40 per bus (64 units / bus × 2 buses = 128 units / master).
- **Bus identifier**: `A` (J_BUS_A, ESP32-S3 UART2) / `B` (J_BUS_B, ESP32-S3 UART0 remapped).
- **RS-485 wire polarity**: conforms to TIA-485-A. `BUS_A` / `BUS_B` nets are the non-inverting (A) and inverting (B) bus lines respectively. Driver idles `A > B` (differential idle = space); fail-safe bias at the master makes `A − B ≈ +200 mV` with no transmitter active.

---

## 1. Cable pinout — CAT5e / CAT6 with RJ45 (LOCKED, T568B)

Pin numbering per TIA/EIA 568B, RJ45 clip **down**, pins facing away:

| Pin | T568B wire color | Net (on cable) | Net (master) | Net (unit passthrough) |
|---|---|---|---|---|
| 1 | white-orange | **+48V** | `BUS_{A,B}_48V` | `BUS_48V_PASS` |
| 2 | orange       | **+48V** | `BUS_{A,B}_48V` | `BUS_48V_PASS` |
| 3 | white-green  | **GND**  | `GND`           | `GND` |
| 4 | blue         | **RS485_A** | `BUS_{A,B}_A_MASTER` | `BUS_A` |
| 5 | white-blue   | **RS485_B** | `BUS_{A,B}_B_MASTER` | `BUS_B` |
| 6 | green        | **GND**  | `GND`           | `GND` |
| 7 | white-brown  | **+48V** | `BUS_{A,B}_48V` | `BUS_48V_PASS` |
| 8 | brown        | **+48V** | `BUS_{A,B}_48V` | `BUS_48V_PASS` |
| Shield | — | chassis | `GND` via 10 nF | `GND` via 10 nF |

**Straight-through only.** Crossover cables short +48V to GND. Only make cables with a standard T568B-to-T568B crimp; never T568A-to-T568B.

---

## 2. ESP32-S3-WROOM-1-N16R8 (master, primary MCU)

Complete GPIO allocation. Unused modules pins (for features not implemented in v2, e.g. additional LED rows) are listed as reserved with a "no copper escape" note so capture ERC can flag any accidental net.

### 2.1 S3 GPIO table

| GPIO | Module pin | Net | Direction | Peripheral / role |
|------|-----------|-----|-----------|-------------------|
| IO0 | 27 | **BOOT_S3_BTN** | Input (module PU) | Boot strap — low at reset → ROM bootloader |
| IO1 | 13 | — | — | Reserved (no copper escape beyond expansion pad if any) |
| IO2 | 14 | — | — | Reserved |
| IO3 | 15 | — | — | **Strap — no copper escape** |
| IO4 | 4  | **TLC_SCK** | Output | SPI-like clock to TLC5947 SCLK |
| IO5 | 5  | **TLC_SIN** | Output | TLC5947 SIN (serial data) |
| IO6 | 6  | **TLC_XLAT** | Output | TLC5947 XLAT |
| IO7 | 7  | **TLC_BLANK** | Output | TLC5947 BLANK (hold low in normal op) |
| IO8 | 8  | **INA_SDA** | Open-drain bidir | I²C1 SDA to both INA237s. 4.7 kΩ pullup to 3V3. |
| IO9 | 9  | **INA_SCL** | Open-drain bidir | I²C1 SCL to both INA237s. 4.7 kΩ pullup to 3V3. |
| IO10 | 10 | **RS485_A_TX** | Output | UART2 TX via GPIO matrix → SN65HVD75_A pin 4 (D) |
| IO11 | 11 | **H2_RESET_N** | Output, active low | 1 kΩ series → H2 EN. Default high-Z; pulse low for H2 reset. |
| IO12 | 12 | **H2_IRQ** | Input | H2 open-drain output, 10 kΩ pullup on S3 side |
| IO13 | 13 | **H2_BOOT_SEL** | Output | 1 kΩ series → H2 IO9. Default high-Z; drive low for H2 UART download. |
| IO14 | 14 | **RS485_A_RX** | Input | UART2 RX via matrix ← SN65HVD75_A pin 1 (R) |
| IO15 | 15 | **RS485_A_DE** | Output | UART2 RTS (hardware DE) → SN65HVD75_A pins 2+3 (/RE tied DE) |
| IO16 | 16 | **RS485_B_TX** | Output | UART0 TX remapped via matrix → SN65HVD75_B pin 4 |
| IO17 | 17 | **H2_UART_TX** | Output | UART1 TX at 921600 8N1 → H2 IO4 |
| IO18 | 18 | **H2_UART_RX** | Input | UART1 RX ← H2 IO5 |
| IO19 | 23 | **USB_DN** | Bidir | Native USB D−, to USBLC6 pins 1/6 |
| IO20 | 24 | **USB_DP** | Bidir | Native USB D+, to USBLC6 pins 3/4 |
| IO21 | 25 | **RGB_DATA** | Output | SK6812 strip DIN via 33 Ω R_RGB_SER. RMT driven at 800 kHz. |
| IO26–IO32 | — | — | — | **Module-internal (SPI flash). Do not route.** |
| IO33–IO37 | — | — | — | **Module-internal (octal PSRAM). Do not route.** |
| IO38 | 33 | **RS485_B_RX** | Input | UART0 RX remapped ← SN65HVD75_B pin 1 |
| IO39 | 34 | **RS485_B_DE** | Output | UART0 RTS → SN65HVD75_B pins 2+3 |
| IO40 | 35 | — | — | Reserved (free for expansion) |
| IO41 | 26 | — | — | Reserved |
| IO42 | 28 | — | — | Reserved |
| IO43 | 37 | **TP_U0TXD_S3** | Output | UART0 TX at boot (ROM bootloader). After firmware boot, UART0 is remapped to IO16/IO38 and IO43 becomes a test pad only. |
| IO44 | 36 | **TP_U0RXD_S3** | Input | UART0 RX at boot. Symmetric to IO43. |
| IO45 | 29 | — | — | **Strap — no copper escape** |
| IO46 | 30 | — | — | **Strap — no copper escape** |
| IO47 | 31 | **ALERT_INA_A** | Input | INA237_A ALERT (open-drain), 10 kΩ pullup |
| IO48 | 32 | **ALERT_INA_B** | Input | INA237_B ALERT, 10 kΩ pullup |
| EN | 3  | **EN_S3_NODE** | Input | 10 kΩ pullup + 1 µF cap + SW_RESET_S3 |
| VDD3V3 | 2 | **3V3_RAIL** | Power | Decoupling per SCHEMATIC.md §A.4 |
| GND | 1, 40, 41, EPAD | **GND** | Return | ≥ 9 thermal vias on EPAD |

### 2.2 S3 strap-pin rules

- **IO0**: boot mode. Module pullup + SW_BOOT_S3 to GND. High at reset = SPI flash boot; low at reset = ROM download boot.
- **IO3, IO45, IO46**: no copper escape on any layer beyond module pad.
- **IO43, IO44**: UART0 at boot. Used by the ROM bootloader during esptool flashing. After firmware is running, UART0 is remapped to IO16/IO38 for Bus B.

### 2.3 S3 firmware-visible constraints

- **I²C1 bus (INA237s)**: IO8/IO9, 400 kHz, addresses 0x40 and 0x41. Arduino: `Wire.begin(8, 9); Wire.setClock(400000);`. Two devices only — no mux.
- **UART2 (Bus A RS-485)**: IO10 (TX), IO14 (RX), IO15 (RTS/DE). Configure with hardware DE enable bit — transceiver's DE is auto-asserted during TX and de-asserted at the end of the last stop bit.
  - Arduino: `Serial2.begin(250000, SERIAL_8N1, 14, 10);` then enable RS-485 mode via `uart_set_mode(UART_NUM_2, UART_MODE_RS485_HALF_DUPLEX);` and configure IO15 as RTS.
- **UART0 (Bus B RS-485)**: IO16 (TX), IO38 (RX), IO39 (RTS/DE). Same RS-485 half-duplex mode.
  - ROM boot uses UART0 on IO43/IO44 — firmware must remap immediately after `setup()` via `uart_set_pin(UART_NUM_0, 16, 38, 39, UART_PIN_NO_CHANGE)`. Programming host must use native USB (USB-CDC) or UART0 at IO43/IO44 *before* firmware remaps.
- **UART1 (H2 link)**: IO17/IO18, 921600 8N1, no flow control, COBS + CRC-16/CCITT-FALSE framing. Unchanged from Rev B.
- **RMT (SK6812)**: IO21, 800 kHz NRZ, GRB colour order, 16 LEDs.
- **RGB_DATA signal integrity**: 33 Ω series on S3 side. Populate `LEVEL_RGB` 74LVC1T45 translator only if SK6812 V_IH fails bring-up qualification (DNP default).
- **H2 reset / boot**: IO11 is the reset line through 1 kΩ, IO13 is the boot-select override through 1 kΩ. Both default high-Z (input) during normal operation; driven as outputs only for H2 reset or H2 OTA. Release H2_RESET_N, wait ≥ 100 ms, send version handshake.
- **H2 IRQ**: IO12, falling-edge interrupt, internal weak pullup disabled (external 10 kΩ does the job).
- **INA237 ALERT**: IO47 / IO48, falling-edge interrupt. Firmware configures the INA237 to assert ALERT on over-current or over-voltage thresholds for per-bus trip handling (power down master PSU gracefully or page over Zigbee).

---

## 3. ESP32-H2-MINI-1-N4 (master, radio coprocessor)

Pinout unchanged from Rev B. Full table re-listed here for self-containment.

### 3.1 H2 GPIO table

| H2 GPIO | Module pin | Net | Direction | Role |
|---------|-----------|-----|-----------|------|
| EN | 24 | **H2_RESET_N** | Input | 10 kΩ pullup + 1 µF cap. S3 IO11 drives low via 1 kΩ. |
| IO0 | 7 | — | — | Module-internal strap |
| IO1 | 8 | — | — | Reserved |
| IO2 | 20 | **H2_STRAP_IO2** | Input (strap) | 10 kΩ pullup to 3V3_RAIL |
| IO3 | 21 | — | — | Reserved |
| IO4 | 4 | **H2_UART_TX** | Input (H2 UART1 RX) | From S3 IO17, 921600 8N1 |
| IO5 | 5 | **H2_UART_RX** | Output (H2 UART1 TX) | To S3 IO18 |
| IO8 | 9 | **H2_STRAP_IO8** | Input (strap) | 10 kΩ pullup (ROM msg enable) |
| IO9 | 10 | **H2_BOOT_SEL** | Input (strap) | 10 kΩ pullup + S3 IO13 via 1 kΩ + JP_DEBUG_H2 pin 6 |
| IO10 | 11 | **H2_IRQ** | Output, open-drain | To S3 IO12 |
| IO11 | 12 | **LED_H2_HB** | Output | 1 kΩ series → LED_4 (yellow) |
| IO12 | 16 | **LED_ZIGBEE** | Output | 1 kΩ series → LED_6 (yellow) |
| IO13 | 17 | **LED_BLE** | Output | 1 kΩ series → LED_7 (blue) |
| IO14 | 18 | — | — | Reserved |
| IO22 | 13 | — | — | Reserved |
| IO23 | 14 | **U0RXD_H2** | Input | JP_DEBUG_H2 pin 4 + TP_U0RXD_H2 |
| IO24 | 15 | **U0TXD_H2** | Output | JP_DEBUG_H2 pin 3 + TP_U0TXD_H2 |
| IO25 | 19 | — | — | Reserved |
| IO26 | 23 | **H2_USB_DN** | Bidir | TP_H2_USB_DN 1 mm pad |
| IO27 | 22 | **H2_USB_DP** | Bidir | TP_H2_USB_DP 1 mm pad |
| VDD3V3 | 2 | **3V3_RAIL** | Power | Decoupling per SCHEMATIC.md §A.5 |
| GND | 1, 3, 25, 26, 27, EPAD | **GND** | Return | ≥ 6 thermal vias on EPAD |

### 3.2 H2 strap-pin rules

- **IO2**: tied high via 10 kΩ. Strap sampled at EN release.
- **IO8**: tied high via 10 kΩ (ROM msg enable on U0TXD_H2 at boot).
- **IO9**: 10 kΩ pullup to 3V3. Default high = SPI boot. S3 IO13 can override low via 1 kΩ for UART download.

### 3.3 H2 firmware-visible constraints

- **UART1**: IO4 (RX from S3) / IO5 (TX to S3), 921600 8N1 responder role. COBS + CRC-16 framing.
- **UART0**: IO23/IO24, ROM bootloader + debug log. JP_DEBUG_H2 + test pads only.
- **H2_IRQ**: IO10, open-drain output, asserted low when H2 has async data queued for S3.
- **Coexistence**: pin 802.15.4 to channel 25 or 26, S3 WiFi to channel 1, to minimize co-channel interference.
- **LEDs**: active-high direct drive through 1 kΩ series. Comm-activity LEDs use ≥ 50 ms pulse-stretch in firmware.

---

## 4. STM32G030F6P6 (unit MCU)

Cortex-M0+ @ 64 MHz (HSI16 + PLL), 32 KB flash, 8 KB RAM, TSSOP-20 package. 14 GPIO available.

### 4.1 Unit MCU GPIO table (per ST DS12990, TSSOP-20 pinout)

| Pin | Signal (MCU) | Net | Direction | Peripheral / role |
|---|---|---|---|---|
| 1 | VDD | **3V3_UNIT** | Power | Digital supply |
| 2 | PB7 | **BTN** | Input (internal PU) | Identify / reset button, active low |
| 3 | PB8 | **LED_STATUS** | Output | Status LED, active high, 330 Ω series |
| 4 | PC14 / OSC32_IN / PF0 | — | — | **Reserved — no copper escape.** LSE 32 kHz crystal pins; not used in v2. |
| 5 | PC15 / OSC32_OUT / PF1 | — | — | **Reserved — no copper escape.** |
| 6 | NRST | **NRST** | Input | 10 kΩ pullup to 3V3_UNIT + 100 nF to GND + TP_NRST |
| 7 | VDDA | **3V3_UNIT** | Analog supply | Via optional filter (ferrite / 10 Ω), 100 nF decoupling |
| 8 | PA0 | **V_48_MON** | Analog input | ADC1_IN0 — voltage monitor from divider |
| 9 | PA1 | **RS485_DE** | Output | USART2 DE (hardware driver-enable, TX-gated). Alt func AF1 / AF4 |
| 10 | PA2 | **RS485_TX_MCU** | Output | USART2 TX. Alt func AF1 |
| 11 | PA3 | **RS485_RX_MCU** | Input | USART2 RX. Alt func AF1 |
| 12 | PA4 | **STEPPER_A** | Output | ULN2003 input 1 |
| 13 | PA5 | **STEPPER_B** | Output | ULN2003 input 2 |
| 14 | PA6 | **STEPPER_C** | Output | ULN2003 input 3 |
| 15 | PA7 | **STEPPER_D** | Output | ULN2003 input 4 |
| 16 | PB0 | **HALL_IN** | Input | KY-003 open-collector output (5V-tolerant FT pin) |
| 17 | PB1 | — | — | Reserved (ADC1_IN9 if needed later) |
| 18 | PA13 | **SWDIO** | Bidir | SWD data to JP_SWD pin 3 |
| 19 | PA14 / BOOT0 | **SWCLK** | Bidir | SWD clock to JP_SWD pin 4. Also BOOT0 strap — 10 kΩ `R_MCU_BOOT0` pulldown to GND forces flash boot. |
| 20 | VSS | **GND** | Return | Digital ground |

### 4.2 Unit firmware-visible constraints

- **Clock**: HSI16 (16 MHz internal RC) × PLL (4×) → 64 MHz SYSCLK. No external crystal. Firmware enables the HSI calibration periodically using the RS-485 byte timing as a trim reference if needed.
- **USART2 (RS-485)**: PA2/PA3 at 250000 baud 8N1 by default. **Hardware DE mode** — the USART drives PA1 (DE) high one bit-time before the start bit and low after the last stop bit. No firmware per-byte DE toggle. STM32Cube config: `UARTEx_SetTxFifoThreshold` irrelevant, but set `UART_DE_Init` with `Polarity = DE_POLARITY_HIGH`, `AssertionTime = 0`, `DeassertionTime = 0`, `UseRxIrda = DISABLE`.
- **Motor drive (ULN2003 inputs)**: PA4..PA7. Firmware holds all four low at idle to de-energize coils (no holding torque needed between movements and it reduces static current → rest of the bus has more headroom).
- **Motor enable contract**: the unit firmware refuses to drive stepper pins unless a `STEP` command arrived from the master within the last 200 ms. Power-on default is all-outputs-low; unit enters "motor-armed" state only in response to master `ARM` + `STEP` sequence.
- **Hall sensor**: PB0 as pure digital input. KY-003 has built-in 10 kΩ pullup on the module. No ADC use.
- **ADC1**: single channel `IN0` on PA0 reads `V_48_MON`. Firmware samples at 10 Hz, 16-sample average. Result is scaled by 16× and reported in 0.25 V increments (single byte, 0–63.75 V range) in every RS-485 reply frame.
- **Identify button**: PB7 with internal pull-up. Long press (≥ 3 s) erases the logical-address flash page and triggers `NVIC_SystemReset()`. Short press (< 500 ms) sends an unsolicited `IDENTIFY` frame on next master poll slot and blinks `LED_STATUS` at 5 Hz for 3 s.
- **Status LED**: PB8, direct-drive through 330 Ω. 3V3 → LED (~2 V forward) → 330 Ω → PB8 sink (active low) OR PB8 → 330 Ω → LED anode → GND (active high, as drawn in SCHEMATIC.md). Pick active high — matches the SCHEMATIC wiring.
- **BOOT0 / SWCLK (PA14) dual role**: with `R_MCU_BOOT0` pulldown always present, BOOT0 reads low at reset → firmware boots from flash. System bootloader is entered by software only (`NVIC_SystemReset()` after writing the magic word into option byte `nBOOT_SEL`), never by hardware strap.
- **Flash write**: logical address lives in the last 2 KB page (addresses `0x0800_7800`–`0x0800_7FFF`). Firmware uses the ST HAL flash API to erase + write that page when the master assigns a new address or when identify-long-press clears it.
- **Unique ID**: 96 bits at `0x1FFF_7590` (per RM0454 §2.5). Firmware reads this at boot to generate its RS-485 enumeration response.

---

## 5. SN65HVD75 RS-485 transceiver (same part, master × 2 + unit × 1)

Single part, three instances.

| Pin | Name | Role |
|---|---|------|
| 1 | R | Receiver output (MCU RX input) |
| 2 | /RE | Receiver enable, active low. **Tied to DE** at PCB — half-duplex auto-direction. |
| 3 | DE | Driver enable, active high. Driven by MCU hardware-DE output (USART RTS). |
| 4 | D | Driver input (MCU TX output) |
| 5 | GND | Ground |
| 6 | A | Non-inverting bus line. TIA-485-A differential A. |
| 7 | B | Inverting bus line. TIA-485-A differential B. |
| 8 | VCC | 3.3 V supply |

### 5.1 Half-duplex wiring

Pins 2 (/RE) and 3 (DE) are shorted at the PCB and driven by a single MCU output:

- **MCU drives line LOW** → /RE=0, DE=0 → receiver enabled, driver tri-stated → **listening**.
- **MCU drives line HIGH** → /RE=1, DE=1 → receiver disabled, driver active → **transmitting**.

STM32's USART hardware-DE feature toggles this line automatically around TX, so firmware never manages it per-byte. ESP32-S3 replicates the same via UART RS-485 mode (`UART_MODE_RS485_HALF_DUPLEX`).

### 5.2 Bus-side termination locations

| Location | Populate? |
|---|---|
| Master Bus A (R_TERM_A 120 Ω + R_BIAS_A_{PU,PD} 680 Ω × 2) | Always |
| Master Bus B (R_TERM_B 120 Ω + R_BIAS_B_{PU,PD} 680 Ω × 2) | Always |
| Unit — intermediate (JP_TERM DNP, R_TERM_UNIT stranded) | Never |
| Unit — end of bus (JP_TERM populated → R_TERM_UNIT 120 Ω across bus) | Only last unit |

---

## 6. INA237 current/voltage monitor (master × 2)

Two instances on the same I²C bus, distinguished by address-strap pins A0/A1.

| Pin | Name | Role |
|---|---|---|
| 1 | IN+ | Positive shunt input (upstream of shunt) |
| 2 | IN− | Negative shunt input (downstream of shunt) |
| 3 | VS | Supply voltage (2.7–5.5 V) |
| 4 | GND | Ground |
| 5 | ALERT | Open-drain interrupt output |
| 6 | SDA | I²C data |
| 7 | SCL | I²C clock |
| 8 | A1 | Address bit 1 |
| 9 | A0 | Address bit 0 |
| 10 | VBUS | Bus-voltage sense tap (85 V max, internal divider) |

### 6.1 Address mapping

Per INA237 datasheet Table 7-2 (address byte = `1000` | A1 | A0):

| Instance | A0 | A1 | I²C address |
|---|---|---|---|
| INA237_A (Bus A) | GND | GND | **0x40** |
| INA237_B (Bus B) | VS | GND | **0x41** |

### 6.2 Shunt + gain settings (firmware)

| Setting | Value |
|---|---|
| Shunt resistance | 50 mΩ |
| Max current | 1.64 A (at the 81.92 mV full-scale ADC range) |
| Current LSB | 50 µA (programmed via `CALIBRATION` register) |
| Voltage range | 0–85 V, LSB 3.125 mV |
| Conversion time | 1.052 ms per channel, continuous shunt + bus mode |
| Averaging | 16 samples (4× oversampling) → effective 60 Hz update |

---

## 7. INA237 ALERT routing

| Signal | Net | S3 GPIO | Firmware role |
|---|---|---|---|
| Bus A over-current / over-voltage | `ALERT_INA_A` | IO47 | ISR marks bus A unhealthy; master may power-gate PSU or page Zigbee |
| Bus B over-current / over-voltage | `ALERT_INA_B` | IO48 | Same for bus B |

Both are open-drain, 10 kΩ pullup to 3V3_RAIL on the S3 side. Configure S3 GPIOs as input with falling-edge interrupt.
