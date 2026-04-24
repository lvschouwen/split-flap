# Master PCB v2 — Digital & Communications Subsystem

> **Scope:** ESP32-S3 module support, USB-C, SWD, MAX14830 SPI-UART bridge, 4× SN65HVD75 RS-485 PHYs, 4× INA237 I²C telemetry, HEARTBEAT LED, eFuse ENABLE × 4, fault aggregation inputs.
>
> **Boundary:** Power rails, eFuses, TVS, ideal-diode, hardwired LEDs (PWR_48V / PWR_3V3 / FAULT) are owned by the power subsystem. This doc only surfaces the signals that cross the boundary.
>
> **Binding reference:** `MASTER_DECISIONS.md` only. Last edited 2026-04-24.

---

## 1. Block diagram

```
                            +---------- USB-C (IO19 D-, IO20 D+) ---- ESD (USBLC6-2SC6)
                            |                                         CC1/CC2: 5k1 pull-down
                            |                                         VBUS -> ferrite + bulk + NC
                            |
              +----- BOOT (IO0 btn) ---+
              |                        |
              | +--- RST (EN btn+RC) --+
              | |
      +-------v-v---------------------------------+
      |         ESP32-S3-WROOM-1-N16R8            |
      |                                           |
      |  SPI1: IO10/CS  IO11/MOSI IO12/MISO       +--SPI-+
      |        IO13/SCK IO14/IRQ                  |      |
      |  I2C0: IO8/SDA  IO9/SCL                   |      v
      |  eFuse EN: IO4, IO5, IO6, IO7             |   +---------------+
      |  PROT_FAULT_OR (in, IRQ): IO15            |   |  MAX14830ETJ+ |
      |  TELEM_ALERT_OR (in, IRQ): IO16           |   |  4× UART/SPI  |
      |  HEARTBEAT LED (out):      IO21           |   |  3.6864 MHz X |
      |  JTAG: IO39/40/41/42                      |   |  GPIO1..4 ->  |
      |  UART0 console: IO43/44                   |   |   BUS_ACT LEDs|
      |  Straps (no route): IO3, IO45, IO46       |   +---+--+--+--+--+
      |  Unassigned (policy): IO48                |       |  |  |  |
      +-------^----+---+---+---+------------------+       |  |  |  |  (TX/RX/DE per ch)
              |    |   |   |   |                          v  v  v  v
              |    |   |   |   |                     +----+----+----+----+
              |    |   |   |   |                     |  4× SN65HVD75D    |
    PROT_FAULT_OR  |   |   |   |                     |  half-duplex 3V3  |
      (wired-OR    |   |   |   |                     +-+----+----+----+--+
       open-drain) |   |   |   |                       | A/B per bus
                   |   |   |   |                       v
              eFuse_EN1..4 (to power subsystem)    CM choke + TVS (SM712)
                                                   + 120R term + 390/390 bias
                                                   -> 4× shielded RJ45

          I²C (IO8/IO9) -----> [INA237 ×4: 0x40..0x43]
                                 ALERTs wired-OR -> TELEM_ALERT_OR (IO16)

                                                      SWD 2x5 1.27mm header
                                                      -> IO39/40/41/42 + EN
```

---

## 2. ESP32-S3 pin budget (complete, IO0–IO48)

Legend — **Dir**: I=input, O=output, IO=bidir, USB=native USB PHY, X=not routed.
**Pull**: internal/external as noted. **Strap**: boot-time strap pin behaviour.

| IO# | Net               | Dir | Pull            | Strap / Boot notes                          | Rationale |
|-----|-------------------|-----|------------------|---------------------------------------------|-----------|
| 0   | BOOT_BTN          | I   | 10k ext PU to 3V3 | Strap: boot mode (0=DL, 1=SPI). Button to GND. | Standard Espressif boot button. |
| 1   | (spare)           | -   | 10k PU, test pad | ADC-capable                                 | Reserve. |
| 2   | (spare)           | -   | 10k PU, test pad | -                                           | Reserve. |
| 3   | NC (no route)     | X   | -                | **Strap (JTAG sel). No copper escape.**     | Policy: no-route. |
| 4   | EFUSE_EN1         | O   | none (power side has 10k PD) | default Hi-Z at boot → eFuse OFF | Bus 1 enable, firmware-sequenced. |
| 5   | EFUSE_EN2         | O   | none (power side has 10k PD) | "                                          | Bus 2 enable. |
| 6   | EFUSE_EN3         | O   | none (power side has 10k PD) | "                                          | Bus 3 enable. |
| 7   | EFUSE_EN4         | O   | none (power side has 10k PD) | "                                          | Bus 4 enable. |
| 8   | I2C_SDA           | IO  | 4k7 ext PU to 3V3 | -                                           | INA237 bank shared bus. |
| 9   | I2C_SCL           | IO  | 4k7 ext PU to 3V3 | -                                           | INA237 bank shared bus. |
| 10  | SPI_CS_MAX        | O   | 10k ext PU to 3V3 | Idle-high at boot so MAX14830 stays deselected. | CS to MAX14830. |
| 11  | SPI_MOSI          | O   | none             | -                                           | SPI to MAX14830. |
| 12  | SPI_MISO          | I   | none             | -                                           | SPI from MAX14830. |
| 13  | SPI_SCK           | O   | none             | -                                           | SPI clock. |
| 14  | MAX_IRQ           | I   | 10k ext PU to 3V3 | Active-low open-drain from MAX14830 IRQ.    | Edge-triggered UART RX/status. |
| 15  | PROT_FAULT_OR     | I   | 10k ext PU to 3V3 (owned here) | Active-low, wired-OR open-drain from LM74700 FAULT + 4× eFuse FAULT. | IRQ input + drives hardwired FAULT LED (pull owned digital side, LED branch owned power side). |
| 16  | TELEM_ALERT_OR    | I   | 10k ext PU to 3V3 | Active-low, wired-OR from 4× INA237 ALERT. | IRQ; firmware polls all 4 INA237 to identify source. |
| 17  | (spare)           | -   | 10k PU, test pad | ADC-capable, DAC1                           | Reserve. |
| 18  | (spare)           | -   | 10k PU, test pad | ADC-capable, DAC2                           | Reserve. |
| 19  | USB_DM            | USB | -                | **Fixed: native USB D−.** No other use.     | USB-C. |
| 20  | USB_DP            | USB | -                | **Fixed: native USB D+.** No other use.     | USB-C. |
| 21  | HEARTBEAT_LED     | O   | none (LED+R to GND) | Must idle low at boot (LED off) — no PU.  | Blue LED, firmware blink pattern. |
| 22–25 | —               | -   | -                | **Do not exist on WROOM-1 module.**         | Not bonded out. |
| 26–32 | FLASH internal  | X   | -                | **Internal SPI flash; do not route.**       | N16R8 internal. |
| 33–37 | PSRAM internal  | X   | -                | **Internal Octal PSRAM; do not route.**     | N16R8 internal. |
| 38  | (spare)           | -   | 10k PU, test pad | -                                           | Reserve. |
| 39  | JTAG_MTCK         | IO  | per SWD header   | **Fixed JTAG.**                             | SWD. |
| 40  | JTAG_MTDO         | IO  | per SWD header   | **Fixed JTAG.**                             | SWD. |
| 41  | JTAG_MTDI         | IO  | per SWD header   | **Fixed JTAG.**                             | SWD. |
| 42  | JTAG_MTMS         | IO  | per SWD header   | **Fixed JTAG.**                             | SWD. |
| 43  | UART0_TXD         | O   | -                | **Reserved console TXD. Not remapped.**     | Boot log / recovery. |
| 44  | UART0_RXD         | I   | -                | **Reserved console RXD. Not remapped.**     | Boot log / recovery. |
| 45  | NC (no route)     | X   | -                | **Strap (VDD_SPI). No copper escape.**      | Policy: no-route. |
| 46  | NC (no route)     | X   | -                | **Strap (boot mode). No copper escape.**    | Policy: no-route. |
| 47  | (spare)           | -   | 10k PU, test pad | -                                           | Reserve. |
| 48  | NC (policy avoid) | X   | -                | **Unassigned. Test pad only, not a signal.**| Policy: avoid. |

**Totals:** Assigned = 15 (IO0 BOOT + IO4–16 + IO21). Spare = 7 (IO1, IO2, IO17, IO18, IO38, IO47, + slack). Matches MASTER_DECISIONS budget within IO0-is-strap-plus-button counting convention.

---

## 3. Sub-circuits

### 3a. Module support — decoupling, EN, BOOT

- **Decoupling**: 100 nF 0402 X7R on every VDD pad pair + 1× 10 µF 0805 X5R + 1× 22 µF 0805 X5R bulk adjacent to module 3V3 entry.
- **EN pin**: 10 kΩ pull-up to 3V3 + 1 nF cap to GND. RESET button SW_RST shorts EN to GND; 100 nF across for contact debounce.
- **IO0 (BOOT)**: 10 kΩ pull-up to 3V3 (strap = 1 = SPI boot). BOOT button to GND. 100 nF debounce across the switch.
- **IO3 / IO45 / IO46**: pad-only; no track, no via, no test pad beyond module footprint.
- **Antenna keep-out**: copper, ground pour, and tall components must respect Espressif keep-out. Mechanical agent owns dimensioning.

### 3b. USB-C

- Receptacle: USB-C 2.0-only 16-pin SMD (e.g. GCT USB4085 / LCSC C165948 class — needs both CC1/CC2).
- CC1, CC2: each 5.1 kΩ to GND (UFP role).
- D+/D−: route to IO20 / IO19. USBLC6-2SC6 (C7415) ESD clamp on the D± pair close to the connector.
- VBUS handling: VBUS → ferrite bead (BLM18PG600SN1) → 4.7 µF bulk → **NC (not routed to 3V3 rail)**. Test pad post-ferrite.
- Shield: USB-C shell tied to GND via 0 Ω populated by default. Hybrid (1 MΩ ∥ 10 nF) left as DNP pad.

### 3c. Reset + Boot buttons

- 2× tactile 6×6 mm SMD, ~160 gf.
- SW_RST across EN–GND; 10k PU + 100 nF debounce.
- SW_BOOT across IO0–GND; 10k PU + 100 nF debounce.

### 3d. SWD / JTAG header

- 2×5 1.27 mm Cortex-Debug header.
- Pinout (ARM 10-pin standard):
  1. VTref → 3V3
  2. TMS → IO42
  3. GND
  4. TCK → IO39
  5. GND
  6. TDO → IO40
  7. KEY (NC)
  8. TDI → IO41
  9. GNDDetect → GND
  10. nRESET → EN through **0 Ω, DNP by default** (populate only when a debugger's nSRST is explicitly needed).
- Optional 33 Ω series on TCK/TDO (DNP) for long-cable SI trim.

### 3e. MAX14830 interface

- IC: MAX14830ETJ+ (TQFN-32, C2683133). 3V3 operation.
- Crystal: 3.6864 MHz fundamental (ABLS-3.6864MHZ-B4-T, C70581), 18 pF load caps (2× 18 pF C0G 0402) XTAL1/XTAL2 to GND.
- SPI wiring: MOSI↔DIN, MISO↔DOUT, SCK↔SCLK, CS↔/CS (IO10). IRQ↔IO14 (open-drain, 10 kΩ PU to 3V3 on digital side).
- Decoupling: 100 nF per VDD + 10 µF bulk.
- /RESET: to 3V3 through 10 kΩ. DNP pad to GND for forced reset.
- GPIO 0..3 (chip GPIOs): configured as outputs driving BUS_ACT_1..4 LEDs (see §3h). See Open Issue O1.
- Address straps: A0/A1 tied to GND (only one MAX14830 on SPI).

### 3f. SN65HVD75D per-bus RS-485 PHY (×4 identical)

- IC: SN65HVD75DR (SOIC-8, C64829).
- D (TX in) ← MAX14830 TXn; R (RX out) → MAX14830 RXn.
- DE + /RE tied together ← MAX14830 per-channel DE output (hardware auto-direction).
- A / B → common-mode choke (Würth 744232601, C191126, 600 Ω @ 100 MHz) → RJ45.
- Transient: SM712-02HTG (C169123) across A–GND / B–GND.
- Termination: 120 Ω 1 % 0805 A↔B, populated (master is chain end).
- Fail-safe bias: 390 Ω 1 % A→3V3, 390 Ω 1 % B→GND. Idle differential ≈ 440 mV.
- RJ45 shield: bonded solid to GND at master.
- RJ45: shielded THT, no magnetics (e.g. Amphenol RJHSE-5380 class).

### 3g. INA237 telemetry bank + TELEM_ALERT_OR

- IC: INA237AIDGSR (VSSOP-10, C2897185). ×4 on shared I²C.
- Address straps:
  - U_INA1: A1=GND A0=GND → 0x40
  - U_INA2: A1=GND A0=VS   → 0x41
  - U_INA3: A1=GND A0=SDA  → 0x42
  - U_INA4: A1=GND A0=SCL  → 0x43
- I²C pull-ups: 4.7 kΩ to 3V3 on SDA (IO8) and SCL (IO9). Single pair for the bus.
- ALERT wired-OR: 4× ALERT open-drain → one TELEM_ALERT_OR net → IO16. 10 kΩ PU to 3V3 on the shared node.
- Decoupling: 100 nF per IC.
- Shunt + input filter network owned by power subsystem.

### 3h. BUS_ACT LED drive from MAX14830

- LEDs: 4× 0603 green, ~2 mA nominal.
- Topology: 3V3 → LED → 1 kΩ 0603 → MAX_GPIOn (active-low sink, preferred). Confirm at schematic capture (O4).
- Firmware or hardware TX-activity mirror stretches blinks to ≥ 20 ms for visibility.

### 3i. HEARTBEAT LED

- Blue 0603.
- 3V3 → LED → 1 kΩ → IO21 (sink).
- No external pull. Firmware owns the pin from reset. Blink pattern per MASTER_DECISIONS.

---

## 4. Signal handoff list (power ⇄ digital boundary)

| Signal | Direction (relative to digital) | Owner of pull | Pull value | Notes |
|---|---|---|---|---|
| EFUSE_EN1..4 | Digital → Power | Power side | 10 kΩ PD to GND | Default OFF at boot. |
| PROT_FAULT_OR | Power → Digital | Digital side | 10 kΩ PU to 3V3 | Wired-OR open-drain. Also drives FAULT LED. |
| TELEM_ALERT_OR | Power → Digital | Digital side | 10 kΩ PU to 3V3 | Wired-OR of 4× INA237 ALERT. |
| INA237 I²C (SDA/SCL) | Digital ↔ Power | Digital side | 4.7 kΩ PU to 3V3 | INA237 shunt + filter owned by power. |
| 3V3 rail | Power → Digital | — | — | |
| 48 V rail | Power → (not used by digital) | — | — | Never enters digital subsystem. |
| USB VBUS | Connector → (isolated) | — | — | Ferrite + bulk + NC. |

---

## 5. Open issues

- **O1 — MAX14830 TX-activity LED mirror mode.** Hardware pulse-stretch mode behaviour must be re-checked at datasheet-read time. Firmware mirror is the fallback (ISR sets GPIO on TX non-empty, one-shot timer clears after 20 ms).
- **O2 — RJ45 pinout for A/B + 48V/GND.** MASTER_DECISIONS is silent on pair assignment. Doc assumes A on pin 4, B on pin 5 (blue pair), 48V/GND on other pairs (owned by power subsystem). **User confirmation required.**
- **O3 — JTAG nRESET populate vs DNP.** Default: 0 Ω, DNP. User to confirm.
- **O4 — BUS_ACT LED drive polarity** (sink vs source). Default: sink (active-low). Confirm at capture.
- **O5 — USB-C shield bonding**. Default: 0 Ω direct to GND. Hybrid DNP option available. Confirm.
- **O6 — Spare GPIO policy**. 7 spares reserved with 10 kΩ PU + test pads, no pre-assigned role.

---

## 6. BOM additions (digital subsystem)

| Ref | Part | LCSC | Package | Qty | Note |
|---|---|---|---|---|---|
| U1 | ESP32-S3-WROOM-1-N16R8 | C2913202 | Module | 1 | 16 MB flash, 8 MB Octal PSRAM |
| U2 | MAX14830ETJ+ | C2683133 | TQFN-32 | 1 | 4-UART SPI bridge |
| U3..U6 | SN65HVD75DR | C64829 | SOIC-8 | 4 | RS-485 PHY 3V3 |
| U7..U10 | INA237AIDGSR | C2897185 | VSSOP-10 | 4 | I²C addr 0x40..0x43 |
| U11 | USBLC6-2SC6 | C7415 | SOT-23-6 | 1 | USB D± ESD |
| Y1 | 3.6864 MHz XTAL, 18 pF | C70581 | 3.2×2.5 | 1 | MAX14830 clock |
| J_USB | USB-C 2.0 receptacle 16-pin | C165948 | SMD | 1 | UFP |
| J_SWD | 2×5 1.27 mm header | C72408 | SMD | 1 | Cortex-debug |
| J_BUS1..4 | Shielded THT RJ45 no-mag | C150877 | THT | 4 | RS-485 bus ports |
| SW_RST, SW_BOOT | Tactile 6×6 SMD 160 gf | C318884 | SMD | 2 | — |
| D_HB | LED blue 0603 | C72041 | 0603 | 1 | HEARTBEAT |
| D_ACT1..4 | LED green 0603 | C72043 | 0603 | 4 | BUS_ACT |
| L_CM1..4 | CM choke 744232601 | C191126 | SMD | 4 | RS-485 pair |
| TVS_B1..4 | SM712-02HTG | C169123 | SOT-23 | 4 | RS-485 transient |
| R_TERM1..4 | 120 Ω 1 % 0805 | C22787 | 0805 | 4 | Termination |
| R_BIAS_H1..4 | 390 Ω 1 % 0603 | C25104 | 0603 | 4 | Fail-safe bias high |
| R_BIAS_L1..4 | 390 Ω 1 % 0603 | C25104 | 0603 | 4 | Fail-safe bias low |
| R_I2C_SDA, R_I2C_SCL | 4.7 kΩ 0402 | C25900 | 0402 | 2 | I²C pull-ups |
| R_ALERT_OR, R_FAULT_OR | 10 kΩ 0402 | C25744 | 0402 | 2 | Wired-OR pull-ups (digital-owned) |
| R_MAX_IRQ, R_MAX_RST | 10 kΩ 0402 | C25744 | 0402 | 2 | MAX14830 IRQ PU, /RESET PU |
| R_CS_PU | 10 kΩ 0402 | C25744 | 0402 | 1 | SPI CS idle-high |
| R_BOOT_PU, R_EN_PU | 10 kΩ 0402 | C25744 | 0402 | 2 | IO0 + EN pull-ups |
| R_CC1, R_CC2 | 5.1 kΩ 1 % 0402 | C127584 | 0402 | 2 | USB-C UFP |
| R_HB, R_ACT1..4 | 1 kΩ 0402 | C11702 | 0402 | 5 | LED series |
| R_SPARE1..7 | 10 kΩ 0402 | C25744 | 0402 | 7 | Spare-GPIO pull-ups |
| FB_VBUS | BLM18PG600SN1 ferrite | C1017 | 0603 | 1 | USB VBUS bead |
| C_VBUS | 4.7 µF 10 V X5R 0603 | C19666 | 0603 | 1 | USB VBUS bulk |
| C_BULK_S3 | 22 µF 10 V X5R 0805 | C45783 | 0805 | 1 | ESP32 bulk |
| C_BULK_S3b | 10 µF 10 V X5R 0805 | C15850 | 0805 | 1 | ESP32 bulk |
| C_DEC_* | 100 nF 25 V X7R 0402 | C1525 | 0402 | ~20 | Per-VDD decoupling across all ICs |
| C_XTAL1..2 | 18 pF C0G 0402 | C1653 | 0402 | 2 | MAX14830 XTAL load |
| C_DEB_EN, C_DEB_BOOT | 100 nF 0402 | C1525 | 0402 | 2 | EN + BOOT debounce |
| C_EN_RC | 1 nF 0402 | C1614 | 0402 | 1 | EN RC filter |

Quantities are minimums; layout pass may add redundant decoupling. LCSC part numbers are for guidance — confirm JLC stock + basic/extended at fab-submit time.
