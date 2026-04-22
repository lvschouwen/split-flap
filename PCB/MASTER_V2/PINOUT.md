# Master v2 — GPIO pinouts

**Rev B.** Full GPIO allocation for both MCUs.

## ESP32-S3-WROOM-1-N16R8 (primary)

| GPIO | Module pin | Function | Net | Direction | Notes |
|------|-----------|----------|-----|-----------|-------|
| IO0  | 27 | Strap + BOOT button | **BOOT_S3_BTN** | Input (module pullup) | Active low. Hold during RESET_S3 → ROM bootloader. Trace ≤ 20 mm, GND-flanked. |
| IO1  | 13 | reserved | — | — | Free for expansion |
| IO2  | 14 | reserved | — | — | Free for expansion |
| IO3  | 15 | Strap pin | — | — | **No copper escape** |
| IO4  | 4  | LED | **LED_WIFI_DRV** | Output | 1 kΩ → blue LED → GND. Firmware: WiFi status |
| IO5  | 5  | LED | **LED_ACT_DRV** | Output | 1 kΩ → green LED → GND. Firmware: I2C/web activity pulse |
| IO6  | 6  | LED | **LED_ZB_DRV** | Output | 1 kΩ → yellow LED → GND. Firmware: Zigbee link status |
| IO7  | 7  | reserved | — | — | Free for expansion |
| IO8  | 8  | I2C SDA | **I2C_SDA_3V3** | Open-drain bidir | 4.7 kΩ pullup to 3V3. Trunk to TCA9548A SDA |
| IO9  | 9  | I2C SCL | **I2C_SCL_3V3** | Open-drain bidir | 4.7 kΩ pullup to 3V3. Trunk to TCA9548A SCL |
| IO10 | 10 | MUX reset | **MUX_RESET_N** | Output, active low | 10 kΩ pullup to 3V3. Drives TCA9548A /RESET |
| IO11 | 11 | H2 reset | **S3_TO_H2_RST** → **H2_RESET_N** | Output, active low | 1 kΩ series (R_H2_RST_SER). External 10 kΩ pullup on H2 EN. Default high-Z during normal op; pulse low to reset H2. |
| IO12 | 12 | H2 IRQ | **H2_IRQ** | Input, active low | H2 open-drain output with 10 kΩ pullup to 3V3. H2 asserts low when it has async data. |
| IO13 | 13 | H2 boot select | **S3_TO_H2_BOOT** → **H2_BOOT_SEL** | Output | 1 kΩ series (R_H2_BOOT_SER). External 10 kΩ pullup on H2 IO9. Default high-Z; drive low to put H2 in UART download mode for OTA. |
| IO14 | 14 | reserved | — | — | Free for expansion |
| IO15 | 15 | reserved | — | — | Free for expansion |
| IO16 | 16 | reserved | — | — | Free for expansion |
| IO17 | 17 | H2 UART1 TX | **H2_UART_TX** | Output | 921600 bps 8N1. To H2 IO4. |
| IO18 | 18 | H2 UART1 RX | **H2_UART_RX** | Input | From H2 IO5. |
| IO19 | 23 | USB D− | **USB_DN** | Bidir | To USBLC6 pins 1/6. 90 Ω diff pair with IO20. |
| IO20 | 24 | USB D+ | **USB_DP** | Bidir | To USBLC6 pins 3/4. 90 Ω diff pair with IO19. |
| IO21 | 25 | reserved | — | — | Free for expansion |
| IO26–IO32 | — | internal | — | — | SPI flash (N16). Module-internal. |
| IO33–IO37 | — | internal | — | — | Octal PSRAM (R8). Module-internal. |
| IO38 | 33 | reserved | — | — | Free for expansion |
| IO39 | 34 | reserved | — | — | Free for expansion |
| IO40 | 35 | reserved | — | — | Free for expansion |
| IO41 | 26 | reserved | — | — | Free for expansion |
| IO42 | 28 | reserved | — | — | Free for expansion |
| IO43 | 37 | UART0 TX | **U0TXD_S3** | Output | To JP_DEBUG_S3 pin 3 + TP_U0TXD_S3. ROM boot log. |
| IO44 | 36 | UART0 RX | **U0RXD_S3** | Input | To JP_DEBUG_S3 pin 4 + TP_U0RXD_S3. |
| IO45 | 29 | Strap pin | — | — | **No copper escape** |
| IO46 | 30 | Strap pin | — | — | **No copper escape** |
| IO47 | 31 | reserved | — | — | Free for expansion |
| IO48 | 32 | reserved | — | — | NC (some revs have onboard RGB LED) |
| EN   | 3  | Chip enable + reset | **EN_S3_NODE** | Input | 10 kΩ pullup to 3V3, 1 µF to GND, SW1 RESET_S3 button to GND |
| VDD3V3 | 2 | 3.3 V supply | **3V3_RAIL** | Power | Decoupling per §6 of SCHEMATIC.md |
| GND | 1, 40, 41, EPAD | Ground | **GND** | Return | Stitch EPAD with ≥ 9 thermal vias to L2 |

Module pin numbers per Espressif ESP32-S3-WROOM-1 datasheet.

### S3 strap-pin rules

- **IO0**: boot mode. Module integrates pullup. BOOT_S3 button pulls to GND. Strap: high = normal SPI flash boot, low at reset = download boot.
- **IO3, IO45, IO46**: no copper connection on any layer beyond the module pad. Retains module-default behaviour.

### S3 firmware-visible constraints

- I2C on GPIO8/9: routed through GPIO matrix. Arduino: `Wire.begin(8, 9);`.
- MUX_RESET_N (GPIO10): active low, ≥ 10 µs pulse during bus-stuck recovery. Keep high in normal operation.
- H2_RESET_N / H2_BOOT_SEL: configured as inputs (high-Z) during normal operation; switched to outputs only for H2 reset or OTA entry. 1 kΩ series resistors protect against contention with JP_DEBUG_H2 external dongle.
- H2_IRQ: interrupt-capable input, falling-edge trigger, internal weak pull-up disabled (external 10 kΩ handles it).
- H2_UART_TX/RX on IO17/IO18: mapped to UART1 via GPIO matrix. 921600 bps 8N1.
- LEDs on IO4/5/6: active high, direct drive through 1 kΩ.
- UART0 (IO43/44): boot-log + debug console. JP_DEBUG_S3 + test pads.

---

## ESP32-H2-MINI-1-N4 (radio coprocessor)

| H2 GPIO | Module pin | Function | Net | Direction | Notes |
|---------|-----------|----------|-----|-----------|-------|
| EN      | 24 | Chip enable | **H2_RESET_N** | Input | Active high enables. 10 kΩ pullup to 3V3, 1 µF to GND. S3 IO11 drives low via 1 kΩ to reset. Also exposed on JP_DEBUG_H2 pin 5. |
| IO0     | 7  | Strap (reserved) | — | — | Module-internal strap behaviour. No external connection. |
| IO1     | 8  | reserved | — | — | No copper escape |
| IO2     | 20 | **Strap (ROM_MSG)** | **H2_STRAP_IO2** | Input (strap) | 10 kΩ pullup to 3V3 (R_H2_STRAP2). Fixed high. |
| IO3     | 21 | reserved | — | — | No copper escape |
| IO4     | 4  | UART1 RX (from S3) | **H2_UART_TX** | Input | GPIO matrix to UART1 RX. 921600 bps 8N1. |
| IO5     | 5  | UART1 TX (to S3) | **H2_UART_RX** | Output | GPIO matrix to UART1 TX. 921600 bps 8N1. |
| IO8     | 9  | **Strap (ROM_MSG_PRINT)** | **H2_STRAP_IO8** | Input (strap) | 10 kΩ pullup to 3V3 (R_H2_STRAP8). Fixed high — enables ROM boot messages. |
| IO9     | 10 | **Strap (BOOT_MODE) + S3 override** | **H2_BOOT_SEL** | Input (strap) | 10 kΩ pullup to 3V3 (R_H2_STRAP9). S3 IO13 can pull low via 1 kΩ for UART download. Also exposed on JP_DEBUG_H2 pin 6. Default: high = SPI boot. |
| IO10    | 11 | IRQ to S3 | **H2_IRQ** | Output, open-drain | 10 kΩ pullup on S3 side to 3V3 (R_H2_IRQ). Active low. Firmware asserts when async Zigbee/BLE event pending. |
| IO11    | 12 | reserved | — | — | No copper escape |
| IO12    | 16 | reserved | — | — | No copper escape |
| IO13    | 17 | reserved | — | — | No copper escape |
| IO14    | 18 | reserved | — | — | No copper escape |
| IO22    | 13 | reserved | — | — | No copper escape |
| IO23    | 14 | UART0 RX (debug) | **U0RXD_H2** | Input | To JP_DEBUG_H2 pin 4 + TP_U0RXD_H2. ROM bootloader + log console. |
| IO24    | 15 | UART0 TX (debug) | **U0TXD_H2** | Output | To JP_DEBUG_H2 pin 3 + TP_U0TXD_H2. |
| IO25    | 19 | reserved | — | — | No copper escape |
| IO26    | 23 | USB D− (USB Serial/JTAG) | **H2_USB_DN** | Bidir | To TP_H2_USB_DN (1 mm pad). For optional JTAG debug. |
| IO27    | 22 | USB D+ (USB Serial/JTAG) | **H2_USB_DP** | Bidir | To TP_H2_USB_DP (1 mm pad). For optional JTAG debug. |
| VDD3V3  | 2  | 3.3 V supply | **3V3_RAIL** | Power | Decoupling per §7 of SCHEMATIC.md |
| GND     | 1, 3, 25, 26, 27, EPAD | Ground | **GND** | Return | Stitch EPAD with ≥ 6 thermal vias to L2 |

Module pin numbers per Espressif ESP32-H2-MINI-1 datasheet.

### H2 strap-pin rules

- **IO2**: tied high via R_H2_STRAP2 (10 kΩ to 3V3). Strap sampled at EN release.
- **IO8**: tied high via R_H2_STRAP8 (10 kΩ to 3V3). High = ROM messages enabled on U0TXD_H2 at boot.
- **IO9**: pulled high via R_H2_STRAP9 (10 kΩ to 3V3). Default boot = SPI flash. S3 IO13 can override to low through 1 kΩ series for UART download boot (OTA update of H2 firmware from S3). JP_DEBUG_H2 pin 6 exposes the same net for external dongle access.

### H2 firmware-visible constraints

- UART1 (IO4/IO5) carries the S3 command/response protocol. 921600 bps 8N1, COBS + CRC-16/CCITT-FALSE framing. H2 is responder only.
- UART0 (IO23/IO24): ROM bootloader + debug log. Accessible via JP_DEBUG_H2 only.
- H2_IRQ (IO10): driven as open-drain output. Default high-Z (high via S3 pullup). Firmware pulls low to signal pending async data.
- 802.15.4 channels 11–26: recommended pin to channel 25 or 26 to minimize WiFi co-channel interference with S3. Application-layer constraint — see coexistence note in README.md.
- USB Serial/JTAG on IO26/27: enabled by default in ROM. Allows direct programming of H2 via TP_H2_USB_DP/DN without going through S3. Independent of JP_DEBUG_H2 UART path.

---

## Inter-MCU signal summary

| Signal | Driver | Receiver | Default | Purpose |
|---|---|---|---|---|
| **H2_UART_TX** | S3 IO17 | H2 IO4 | — | Command frames from S3 to H2 |
| **H2_UART_RX** | H2 IO5 | S3 IO18 | — | Response frames from H2 to S3 |
| **H2_RESET_N** | S3 IO11 (1 kΩ series) | H2 EN | High (10 kΩ pullup) | S3 resets H2 |
| **H2_IRQ** | H2 IO10 (open-drain) | S3 IO12 | High (10 kΩ pullup) | H2 signals async event to S3 |
| **H2_BOOT_SEL** | S3 IO13 (1 kΩ series) | H2 IO9 | High (10 kΩ pullup) | S3 puts H2 in download mode for OTA |

**5 signals total between the two MCUs. No more, no less. No SPI.**

## Reserved-for-expansion GPIO list (S3)

S3 free GPIO for future expansion headers or features:

- IO1, IO2 (general use)
- IO7 (general use)
- IO14, IO15, IO16 (general use)
- IO21 (general use)
- IO38, IO39, IO40, IO41, IO42 (general use; no strap, no reserved role)
- IO47, IO48 (general use; IO48 may be RGB on some module revs — check before use)

Expansion header not fitted in Rev B. If added in future, prefer IO38–42.
