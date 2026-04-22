# Master v2 — GPIO pinouts

**Rev B.** Full GPIO allocation for both MCUs.

## Conventions

- **Row labels are one-indexed: Row 1 .. Row 8.** These are the 8 downstream channels of the TCA9548A I2C mux. Silkscreen, web UI, logs, BOM, and firmware constants all use 1..8. Internal firmware translates to the mux's 0..7 channel index via `splitflap::rowToChannelIndex(row) = row - 1` in `firmware/lib/common/config.h`.
- **Unit labels within a row are one-indexed: unit 1 .. unit 16.** DIP 0000 → unit 1 at I2C address 0x01. DIP 1111 → unit 16 at I2C address 0x10. Same convention as ESPMaster.
- **Capacity: 8 rows × 16 units per row = 128 unit ceiling.**

## ESP32-S3-WROOM-1-N16R8 (primary)

| GPIO | Module pin | Function | Net | Direction | Notes |
|------|-----------|----------|-----|-----------|-------|
| IO0  | 27 | Strap + BOOT button | **BOOT_S3_BTN** | Input (module pullup) | Active low. Hold during RESET_S3 → ROM bootloader. Trace ≤ 20 mm, GND-flanked. |
| IO1  | 13 | reserved | — | — | Free for expansion |
| IO2  | 14 | reserved | — | — | Free for expansion |
| IO3  | 15 | Strap pin | — | — | **No copper escape** |
| IO4  | 4  | TLC5947 SCK | **TLC_SCK** | Output | SPI clock to TLC5947 pin SCLK. LED-bar shift register clock. See SCHEMATIC §13. |
| IO5  | 5  | TLC5947 SIN | **TLC_SIN** | Output | SPI data to TLC5947 pin SIN. Shifts in 24 × 12-bit PWM values for the fixed LED row. |
| IO6  | 6  | TLC5947 XLAT | **TLC_XLAT** | Output | Latches TLC5947 shift register into PWM counters. Pulse high after every full 288-bit write. |
| IO7  | 7  | TLC5947 BLANK | **TLC_BLANK** | Output | Active-high disables all TLC5947 outputs. Firmware: hold low in normal op; drive high for global LED off (sleep / unified dimming). |
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
| IO21 | 25 | SK6812 RGB data | **RGB_DATA** | Output | To first SK6812 DIN on the 16-LED RGB strip. 800 kHz timing driven by the RMT peripheral. See SCHEMATIC §14. |
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
- TLC5947 SPI (IO4/5/6/7): 30 MHz max SCK. Push 288 bits (24 × 12-bit PWM) then pulse XLAT. Hold BLANK low in normal operation. Arduino: `SPI.begin(4, -1, 5);` (SCK, MISO unused, MOSI).
- RGB_DATA (IO21): driven by the RMT peripheral at the SK6812 bitstream rate (800 kHz, 0.35/0.7 µs high times). Use the `FastLED` or `Adafruit_NeoPixel` library configured for GRB ordering.
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
| IO11    | 12 | LED heartbeat | **LED_H2_HB** | Output | 1 kΩ series → yellow LED (LED_4) → GND. Firmware: 1 Hz blink while main loop is running. Independent of S3 link. |
| IO12    | 16 | LED Zigbee | **LED_ZIGBEE** | Output | 1 kΩ series → yellow LED (LED_6) → GND. Firmware: off = no network, solid = joined, flicker = radio TX/RX (50 ms pulse-stretch). |
| IO13    | 17 | LED BLE | **LED_BLE** | Output | 1 kΩ series → blue LED (LED_7) → GND. Firmware: off = inactive, solid = advertising/paired, flicker = GATT TX/RX (50 ms pulse-stretch). |
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
- LED_H2_HB / LED_ZIGBEE / LED_BLE on IO11/12/13: active-high, direct-drive through 1 kΩ series resistor. Activity LEDs use a ≥ 50 ms minimum on-time pulse-stretch so byte-level TX/RX is visible to the eye.
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

S3 free GPIO available for future expansion headers or features:

- IO1, IO2 (general use)
- IO14, IO15, IO16 (general use)
- IO38, IO39, IO40, IO41, IO42 (general use; no strap, no reserved role)
- IO47, IO48 (general use; IO48 may be RGB on some module revs — check before use)

If an expansion header is added, prefer IO38–42.

## LED bar (fixed row + RGB strip)

Physical placement: along the **front-facing long edge** of the PCB, two stacked rows of side-emit SMD LEDs. The 3D-printed case exposes this edge through a slot labelled `LED ROW — FRONT` on the silkscreen.

### Fixed row — 19 LEDs, driven by TLC5947 + H2 + power rails

Numbered left-to-right as viewed from the front.

| # | Color | Driver | Net | Indicates |
|---|-------|--------|-----|-----------|
| LED_1  | green      | hw 5V rail (R_LED_5V)   | `LED_5V_RAIL`  | 5V rail present (≥ 4.7 V) |
| LED_2  | green      | hw 3V3 rail (R_LED_3V3) | `LED_3V3_RAIL` | 3V3 rail present (≥ 3.1 V) |
| LED_3  | green      | S3 via TLC5947 ch0      | `LED_S3_HB`    | S3 heartbeat — 1 Hz pulse |
| LED_4  | yellow     | H2 IO11 direct          | `LED_H2_HB`    | H2 heartbeat — 1 Hz pulse, independent of S3 |
| LED_5  | blue       | S3 via TLC5947 ch1      | `LED_WIFI`     | WiFi: off=no link, solid=connected, flicker=traffic |
| LED_6  | yellow     | H2 IO12 direct          | `LED_ZIGBEE`   | Zigbee: off=no link, solid=joined, flicker=TX/RX |
| LED_7  | blue | H2 IO13 direct          | `LED_BLE`      | BLE: off=inactive, solid=advertising/paired, flicker=GATT |
| LED_8  | green      | S3 via TLC5947 ch2      | `LED_I2C_ACT`  | I2C bus activity — pulse on any transaction |
| LED_9  | white      | S3 via TLC5947 ch3      | `LED_UART_ACT` | S3↔H2 UART activity — pulse on byte traffic |
| LED_10 | green      | S3 via TLC5947 ch4      | `LED_ROW_1`    | Row 1 selected — pulse when mux bit 0 is set |
| LED_11 | green      | S3 via TLC5947 ch5      | `LED_ROW_2`    | Row 2 selected |
| LED_12 | green      | S3 via TLC5947 ch6      | `LED_ROW_3`    | Row 3 selected |
| LED_13 | green      | S3 via TLC5947 ch7      | `LED_ROW_4`    | Row 4 selected |
| LED_14 | green      | S3 via TLC5947 ch8      | `LED_ROW_5`    | Row 5 selected |
| LED_15 | green      | S3 via TLC5947 ch9      | `LED_ROW_6`    | Row 6 selected |
| LED_16 | green      | S3 via TLC5947 ch10     | `LED_ROW_7`    | Row 7 selected |
| LED_17 | green      | S3 via TLC5947 ch11     | `LED_ROW_8`    | Row 8 selected |
| LED_18 | orange     | S3 via TLC5947 ch12     | `LED_OTA`      | OTA in progress (solid) |
| LED_19 | red        | S3 via TLC5947 ch13     | `LED_FAULT`    | Fault / recovery mode |

TLC5947 channels 14..23 are unpopulated at assembly, broken out to a 1×10 unpopulated header (`JP_LED_EXP`) for future LEDs.

### RGB strip — 16 SK6812-mini side-emit, daisy-chained

Single data line from S3 IO21 (`RGB_DATA`), 3V3 logic, 5V supply rail. Software-defined patterns. No fixed meaning per LED — firmware owns the bitmap.

### Current-limit resistors

- Fixed LEDs driven by TLC5947: no external resistor (TLC5947 is a constant-current sink; `R_IREF` sets the per-channel current to ~15 mA).
- Fixed LEDs driven by H2 GPIOs (LED_4 / LED_6 / LED_7): 1 kΩ series (R_LED_H2_HB, R_LED_ZIGBEE, R_LED_BLE) → ~2 mA forward current, comfortable for all three colors at 3V3.
- Power LEDs (LED_1 / LED_2): 1 kΩ series (R_LED_5V, R_LED_3V3) from the respective rail.
- SK6812 strip: internal constant-current — no external resistors.

### Firmware pulse-stretch for activity LEDs

Raw UART / WiFi / I2C traffic toggles faster than the eye resolves. Every comm-activity LED (`LED_WIFI`, `LED_ZIGBEE`, `LED_BLE`, `LED_I2C_ACT`, `LED_UART_ACT`) uses a minimum-50 ms on-time stretch in firmware: each byte or packet retriggers a `last_activity_ms = millis()`; the LED reads `(millis() - last_activity_ms) < 50`.
