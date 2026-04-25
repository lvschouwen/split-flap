# Master PCB v2 — Digital & Communications Subsystem

> **Scope:** ESP32-S3 module support, USB-C, SWD, MAX14830 SPI-UART bridge, 4× SN65HVD75 RS-485 PHYs, 4× INA237 I²C telemetry, HEARTBEAT LED, eFuse ENABLE × 4, fault aggregation inputs.
>
> **Boundary:** Power rails, eFuses, TVS, ideal-diode, hardwired LEDs (PWR_48V / PWR_3V3 / FAULT) are owned by the power subsystem. This doc only surfaces the signals that cross the boundary.
>
> **Binding reference:** `MASTER_DECISIONS.md` only. Last edited 2026-04-25 (post-review revisions; see issue #76).

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
| 4   | EFUSE_EN1         | O   | 10k PD + 100nF RC + POR-Schottky to /RESET (power side) | Held LOW until 3V3 is healthy AND MCU drives high. eFuse cannot turn on at brown-in. | Bus 1 enable, firmware-sequenced. |
| 5   | EFUSE_EN2         | O   | 10k PD + 100nF RC + POR-Schottky (power side) | "                                          | Bus 2 enable. |
| 6   | EFUSE_EN3         | O   | 10k PD + 100nF RC + POR-Schottky (power side) | "                                          | Bus 3 enable. |
| 7   | EFUSE_EN4         | O   | 10k PD + 100nF RC + POR-Schottky (power side) | "                                          | Bus 4 enable. |
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

- **Decoupling (revised 2026-04-25 per WROOM-1 hardware design guide)**: bulk = **22 µF X5R 0805 + 10 µF X5R 0805 + 100 nF X7R 0402**, all three placed **within 5 mm of the module's pin 2 (3V3 entry)**. Additional 100 nF X7R 0402 on every other module VDD pad pair. The pre-2026-04-25 spec used a single 22 µF MLCC which has X5R high-Q resonance issues at the buck's 2.1 MHz harmonics — replaced with the WROOM-1 reference cap stack.
- **EN pin**: 10 kΩ pull-up to 3V3 + 1 nF cap to GND. RESET button SW_RST shorts EN to GND; 100 nF across for contact debounce.
- **IO0 (BOOT)**: 10 kΩ pull-up to 3V3 (strap = 1 = SPI boot). BOOT button to GND. 100 nF debounce across the switch.
- **IO3 / IO45 / IO46 strap pads**: module-pad only; no copper escape. Note: WROOM-1 module-internal pulls already set the strap state — these pads exist but are not floating MCU pins. No external connection needed; clarification from #76 audit.
- **IO19 / IO20 (USB-CDC) layout note**: native USB pins also act as boot-mode straps for USB_JTAG vs UART download. Layout must verify boot mode is unaffected by USB host attach during reset (per ESP32-S3 datasheet §2.4).
- **Antenna keep-out**: copper, ground pour, and tall components must respect Espressif keep-out. Mechanical agent owns dimensioning.

### 3b. USB-C

- Receptacle: USB-C 2.0-only 16-pin SMD (e.g. GCT USB4085 / LCSC C165948 class — needs both CC1/CC2).
- CC1, CC2: each 5.1 kΩ to GND (UFP role).
- D+/D−: route to IO20 / IO19. USBLC6-2SC6 (C7519, REVIEW_VERIFIED — old C7415 was wrong) ESD clamp on the D± pair close to the connector.
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

- IC: MAX14830ETJ+ (TQFN-32, **CHECK** — REVIEW_BLOCKER: old C2683133 unverified, ETJ+ package not cleanly mapped on JLC). 3V3 operation.
- Crystal: 3.6864 MHz fundamental (ABLS-3.6864MHZ-B4-T, C70581). **Load caps must match the crystal's specified C_L** — for an 18 pF C_L crystal: C_load_cap = 2 × (C_L − C_stray) = 2 × (18 − 3) = **30 pF C0G 0402** on each of XTAL1, XTAL2 to GND. Verify crystal C_L spec at BOM-finalize before fixing cap value.
- SPI wiring: MOSI↔DIN, MISO↔DOUT, SCK↔SCLK, CS↔/CS (IO10). IRQ↔IO14 (open-drain, 10 kΩ PU to 3V3 on digital side).
- **VEXT pin (I/O level reference) tied to 3V3** with its own 100 nF X7R decoupling cap. Locked 2026-04-25.
- Decoupling: 100 nF per VDD + 10 µF bulk.
- /RESET: to 3V3 through 10 kΩ. DNP pad to GND for forced reset.
- GPIO 0..3 (chip GPIOs): configured as outputs driving BUS_ACT_1..4 LEDs (see §3h).
- **Mode-select pins (revised 2026-04-25)**: in SPI mode, the MAX14830's mode-select / chip-select pins are determined by datasheet — A0/A1 are NOT address straps in SPI mode (that was the I²C-mode behaviour). Confirm exact pin handling per MAX14830 datasheet §"SPI mode" before tape-out. The pre-2026-04-25 wording "A0/A1 tied to GND" was a hold-over from I²C-mode docs and needs schematic-capture verification.

### 3f. SN65HVD75D per-bus RS-485 PHY (×4 identical)

- IC: SN65HVD75DR (SOIC-8, C57928 REVIEW_VERIFIED_LCSC). Baud locked to **500 kbaud 8N1**.
- D (TX in) ← MAX14830 TXn; R (RX out) → MAX14830 RXn.
- DE + /RE tied together ← MAX14830 per-channel DE output (hardware auto-direction).
- A / B → common-mode choke (Würth 744232601, **CHECK** — REVIEW_BLOCKER: old C191126 resolved to Vishay VEMD5510C photodiode, 600 Ω @ 100 MHz) → RJ45.
- Transient: SM712-02HTG (C169123) across A–GND / B–GND.
- Termination: 120 Ω 1 % 0805 A↔B, populated (master is chain end).
- **Fail-safe bias (revised 2026-04-25): 1 kΩ 1 % A→3V3, 1 kΩ 1 % B→GND** (was 390 Ω each leg). Idle differential ≈ 280 mV — comfortably above SN65HVD75 200 mV failsafe threshold; idle current per bus 1.6 mA (was 4.2 mA at 390 Ω → 17 mA across 4 buses).
- RJ45 shield: bonded **solid to GND at master only**. Unit/backplane RJ45 shields float (locked 2026-04-25 to avoid ground-loop hunting across 32 parallel RC paths).
- RJ45: shielded THT, no magnetics. Cabling spec: **shielded CAT5e or CAT6 (S/FTP or F/UTP), straight-through, T568B**. Max chain length 32 m total / 3 m per hop / ≤7 V worst-case IR drop.
- **Wire format**: COBS(payload || CRC16-BE) 0x00 framing per `firmware/lib/common/`. Includes broadcast and batched-update message types (`BROADCAST_SET_POSITION`, `BATCH_UPDATE`) for sync animation across all units, plus a `LOOPBACK` message for end-to-end chain integrity check at boot.
- **Pin-1 indicator on RJ45**: silkscreen `1` next to pin-1 of every RJ45. Required (M6 closed 2026-04-25).

### 3g. INA237 telemetry bank + TELEM_ALERT_OR

- IC: INA237AIDGSR (VSSOP-10, **CHECK** — REVIEW_BLOCKER: exact LCSC unverified; INA226 C49851 not a blind substitute). ×4 on shared I²C.
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

### 3j. EFUSE_EN POR gating (cross-boundary, owned power-side)

> Mandatory — without this, eFuses can briefly enable during 48 V brown-in before the MCU is alive to sequence them.

- **U_POR**: TPS3839L33DBVR, monitors 3V3, threshold 3.08 V (typ), open-drain /RESET output, asserted LOW when 3V3 is below threshold.
- **/RESET pull-up**: 10 kΩ to 3V3.
- **Per-EN gating (×4)**: BAT54 Schottky diode anode-at-EFUSE_EN_n / cathode-at-/RESET. When /RESET is asserted (3V3 unhealthy), each diode forward-biases and pulls its EN line to ≈ 0.3 V — well below the eFuse EN threshold (TPS259827 was 1.35 V typ; **pass-2 note:** confirm the TPS26600PWPR EN threshold at schematic capture, candidate replacement part). When /RESET is released (3V3 healthy), diodes reverse-bias and EN follows the MCU GPIO normally.
- **Per-EN RC filter**: 100 nF MLCC from each EN to GND, forming τ ≈ 1 ms with the 10 kΩ pull-down. Rejects brown-in supply noise and any sub-millisecond GPIO glitch during boot. Does not slow legitimate enable transitions noticeably (firmware enables buses in sequence with > 10 ms gaps).
- **Layout**: place U_POR within 5 mm of the LMR36015 V_OUT pin so it senses the same node the digital logic actually runs on. BAT54 diodes near the eFuse EN pins, not near the supervisor.

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

- **O1 — MAX14830 TX-activity LED mirror mode.** Hardware pulse-stretch mode behaviour must be re-checked at datasheet-read time. Firmware mirror is the fallback (ISR sets GPIO on TX non-empty, one-shot timer clears after 20 ms). Non-blocking.
- ~~**O2**~~ **Closed 2026-04-25.** RJ45 pinout locked in `MASTER_DECISIONS.md`: pin 1/2 = NC reserved, pin 3/6 = RS-485 A/B (T568B green pair, true twisted pair), pin 4/5 = +48V (paralleled), pin 7/8 = GND (paralleled). Cable spec: T568B straight-through only.
- ~~**O3**~~ **Closed 2026-04-25.** JTAG nRESET stays 0 Ω DNP — populated only when a debugger's nSRST is explicitly needed.
- ~~**O4**~~ **Closed 2026-04-25.** BUS_ACT LED drive: active-low sink (3V3 → LED → 1 kΩ → MAX_GPIOn).
- ~~**O5**~~ **Closed 2026-04-25.** USB-C shield bond: 0 Ω direct to GND. Hybrid pad pattern remains DNP for future tuning.
- **O6 — Spare GPIO policy**. 7 spares reserved with 10 kΩ PU + test pads, no pre-assigned role. Persist.

---

## 6. BOM additions (digital subsystem)

| Ref | Part | LCSC | Package | Qty | Note |
|---|---|---|---|---|---|
| U1 | ESP32-S3-WROOM-1-N16R8 | C2913202 | Module | 1 | 16 MB flash, 8 MB Octal PSRAM |
| U2 | MAX14830ETJ+ | CHECK | TQFN-32 | 1 | 4-UART SPI bridge (REVIEW_BLOCKER: old C2683133 unverified) |
| U3..U6 | SN65HVD75DR | C57928 | SOIC-8 | 4 | RS-485 PHY 3V3 (REVIEW_VERIFIED_LCSC) |
| U7..U10 | INA237AIDGSR | CHECK | VSSOP-10 | 4 | I²C addr 0x40..0x43 (REVIEW_BLOCKER: INA226 C49851 not a substitute) |
| U11 | USBLC6-2SC6 | C7519 | SOT-23-6 | 1 | USB D± ESD (REVIEW_VERIFIED — old C7415 wrong) |
| Y1 | 3.6864 MHz XTAL, 18 pF | C70581 | 3.2×2.5 | 1 | MAX14830 clock |
| J_USB | USB-C 2.0 receptacle 16-pin | C165948 | SMD | 1 | UFP |
| J_SWD | 2×5 1.27 mm header | C72408 | SMD | 1 | Cortex-debug |
| J_BUS1..4 | Shielded THT RJ45 no-mag | CHECK | THT | 4 | RS-485 bus ports (REVIEW_BLOCKER: old C150877 wrong; lock exact MPN consistent with backplane before fab) |
| SW_RST, SW_BOOT | Tactile 6×6 SMD 160 gf | C318884 | SMD | 2 | — |
| D_HB | LED blue 0603 | C72041 | 0603 | 1 | HEARTBEAT |
| D_ACT1..4 | LED green 0603 | C72043 | 0603 | 4 | BUS_ACT |
| L_CM1..4 | CM choke 744232601 | CHECK | SMD | 4 | RS-485 pair (REVIEW_BLOCKER: old C191126 wrong) |
| TVS_B1..4 | SM712-02HTG | C169123 | SOT-23 | 4 | RS-485 transient |
| R_TERM1..4 | 120 Ω 1 % 0805 | C22787 | 0805 | 4 | Termination |
| R_BIAS_H1..4 | 1 kΩ 1 % 0603 | C21190 | 0603 | 4 | Fail-safe bias high (revised 2026-04-25 from 390 Ω) |
| R_BIAS_L1..4 | 1 kΩ 1 % 0603 | C21190 | 0603 | 4 | Fail-safe bias low (revised 2026-04-25 from 390 Ω) |
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
| C_XTAL1..2 | 30 pF C0G 0402 (assumes 18 pF C_L crystal) | check | 0402 | 2 | MAX14830 XTAL load — verify against crystal C_L spec |
| C_VEXT_MAX | 100 nF 25V X7R 0402 | C1525 | 0402 | 1 | MAX14830 VEXT decoupling (added 2026-04-25) |
| C_DEB_EN, C_DEB_BOOT | 100 nF 0402 | C1525 | 0402 | 2 | EN + BOOT debounce |
| C_EN_RC | 1 nF 0402 | C1614 | 0402 | 1 | EN RC filter |

Quantities are minimums; layout pass may add redundant decoupling. LCSC part numbers are for guidance — confirm JLC stock + basic/extended at fab-submit time.
