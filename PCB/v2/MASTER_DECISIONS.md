# Master PCB v2 — locked design decisions

> **Single source of truth for the master PCB.** Design agents bind to this file ONLY. No other file in this repo is authoritative for the v2 master design.
>
> Last edited: 2026-04-24.
>
> Unit PCB is **out of scope** for this round — no unit decisions here.

## System shape

- **Single MCU**: ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB Octal PSRAM), soldered SMD.
- **No second MCU / radio coprocessor on the master board.** S3's native Wi-Fi + BLE covers connectivity needs; no Zigbee/Thread on-board.
- **No decorative LED bar.** Health indicators only.
- **4 RJ45 ports on master**, each driving a 32-unit RS-485 daisy-chain. 128 units total.
- **Unit cabling rule (locked even though unit PCB is out of scope)**: IN/OUT jacks on the unit are **electrically symmetric** (passthrough). Two physical rows of 16 units can be bridged end-to-end into one 32-unit bus. Termination jumper on the unit is populated **only** on the true electrical end of each chain.
- **Cabling standard**: CAT5e or CAT6, straight-through (TIA-568B), RJ45 shielded connectors. No magnetics (magnetics would block DC power and corrupt RS-485).

## RJ45 pinout (passive PoE-compatible — 802.3at Mode B convention)

This board follows the **IEEE 802.3at Mode B pinout convention** for power delivery, so it uses standard PoE-style pin assignments. It is NOT a true PoE PSE — there is no detection/classification handshake. A real 802.3af/at/bt switch will refuse to energize the cable (no PD signature present), which is safe behavior. Power is supplied by the master itself, always on (within eFuse limits).

| Pin | T568B wire color | Net | Notes |
|---|---|---|---|
| 1 | white-orange | NC (reserved) | Standard Mode B leaves pair 1/2 for Ethernet data — not used in this design |
| 2 | orange | NC (reserved) | " |
| 3 | white-green | RS-485 A | Green pair (3/6) is a true twisted pair in CAT6 — suitable for differential |
| 4 | blue | +48V | Blue pair (4/5) = Mode B V+ (paralleled for ~3 A conductor capacity) |
| 5 | white-blue | +48V | paralleled with pin 4 |
| 6 | green | RS-485 B | paired with pin 3 |
| 7 | white-brown | GND | Brown pair (7/8) = Mode B V− (paralleled) |
| 8 | brown | GND | paralleled with pin 7 |

**Cable requirement**: straight-through only (pin N → pin N). Crossover cables will short +48V to GND and damage both master and the first unit. All cables shipped/recommended with the system must be straight-through CAT5e/CAT6 patch cables.

**Current capacity over CAT6 (24 AWG)**: each conductor ~1.5 A; paralleled pair ~3 A. eFuse trip at 1.7 A per bus leaves ~75% headroom on the power pairs.
- **Max 128 units per master** (4 buses × 32 units).

## Power architecture

| Item | Decision |
|---|---|
| Input | 48 V DC barrel jack, 5.5 / 2.5 mm, 60 V rated |
| Reverse-polarity | LM74700-Q1 ideal-diode controller + external N-MOSFET. FAULT pin → ESP32 GPIO. |
| Input TVS (primary) | SMDJ58CA bidirectional (3 kW / 1 ms), clamps at ~93 V at full surge |
| Input surge secondary clamp | 47 Ω 1 W series + SMBJ60A between SMDJ node and LM74700/LMR36015 VIN — keeps downstream IC feed ≤ 65 V abs-max during a full TVS clamp event (~€0.40 BOM, mandatory) |
| FAULT LED isolation | **BSS138 N-MOSFET inverter** drives FAULT LED from PROT_FAULT_OR, so the wired-OR line only has to sink the 10 kΩ pull-up (~0.33 mA) rather than LED + pull-up (~2.3 mA) — stays within LM74700's 1 mA FAULT I_OL spec |
| Rail architecture | **Single buck 48V → 3V3** via **LMR36015FDDA** (TI, 60 V sync buck, 1.5 A, 2.1 MHz, HSOIC-8 PowerPAD). No 5V rail on the master. Every IC (ESP32-S3, MAX14830, SN65HVD75, INA237) is natively 3.3V; nothing needs 5V. USB-VBUS is isolated from board rails. |
| Per-bus protection | **TPS259827YFFR eFuse per bus (×4)**. 60 V rated, adjustable I_LIM (set ~1.7 A), ENABLE + FAULT + PGOOD. DSBGA-10 (chip-scale). JLC PCBA compatible (C2906816). |
| Per-bus current telemetry | INA237AIDGSR per bus (×4), 50 mΩ high-side shunt, I²C. Addresses 0x40–0x43. |

**Cost note (flagged by user):** per-bus eFuse is ~€10 total across 4 buses. Accepted for safety. **Revisit candidate** if a later cost-trim pass is needed — fallback would be polyfuse + INA237-ALERT-driven software cutoff.

## Inter-chip communication

- **RS-485 UART expansion**: **1× MAX14830** (4 UARTs over SPI). All 4 bus UARTs live on this chip, NOT on the S3 native UARTs.
  - Shared SPI bus to S3: MOSI / MISO / SCK / CS / IRQ = 5 ESP32 GPIOs
- **RS-485 transceivers**: 4× SN65HVD75D (3.3 V, half-duplex), one per bus. DE/RE driven by MAX14830 per-channel DE pin (hardware-assisted, no firmware per-byte toggling).
- **Telemetry**: 4× INA237 on I²C bus (internal to the master — not exposed off-board). Addresses 0x40–0x43.
- **eFuse control**: 4× eFuse ENABLE pins driven by **direct ESP32-S3 GPIOs** (one per bus, firmware sequences bus-on at boot). No I/O expander — there was no pin-budget pressure to justify the extra IC.
- **Fault aggregation**:
  - **PROT_FAULT_OR** — wired-OR of LM74700-Q1 FAULT + 4× eFuse FAULT → 1 ESP32 GPIO (IRQ, also drives the FAULT LED). Firmware reads individual status via INA237 voltage readback correlation + eFuse state knowledge.
  - **TELEM_ALERT_OR** — wired-OR of 4× INA237 ALERT → 1 ESP32 GPIO (IRQ). Firmware polls all 4 INA237s over I²C to identify which fired.
- **USB**: USB-C connector wired to ESP32-S3 native USB (IO19/IO20) for flash + debug + CDC.
- **SWD**: 1.27 mm 2×5 Cortex-debug header (JTAG on ESP32-S3).
- **No expansion header.** The master has no off-board connector for daughterboards. Future Zigbee/Thread/radio support would require a board rev or soldering wires to test pads on the native UART1/UART2 pins.

## LED set (8 total)

| # | Label | Driven by | Color | Visible without firmware |
|---|---|---|---|---|
| 1 | PWR_48V | Hardwired on 48 V via resistor | Red | ✅ |
| 2 | PWR_3V3 | Hardwired on 3V3 via resistor | Green | ✅ |
| 3 | FAULT | Hardwired on PROT_FAULT_OR node (OR of LM74700 FAULT + 4× eFuse FAULTs) | Red | ✅ |
| 4 | HEARTBEAT | ESP32-S3 GPIO. Encodes firmware liveness + WiFi status via blink pattern: off = dead; 2 Hz fast = booting/connecting; 1 Hz steady = running + WiFi up; 0.5 Hz slow = running + WiFi down/AP mode. | Blue | ❌ |
| 5 | BUS_ACT_1 | MAX14830 GPIO tied to channel-1 TX activity | Green | Partial (TX-time) |
| 6 | BUS_ACT_2 | MAX14830 GPIO tied to channel-2 TX activity | Green | Partial |
| 7 | BUS_ACT_3 | MAX14830 GPIO tied to channel-3 TX activity | Green | Partial |
| 8 | BUS_ACT_4 | MAX14830 GPIO tied to channel-4 TX activity | Green | Partial |

All 0603 SMD, fixed colors (no RGB). No dedicated WIFI LED — status encoded into HEARTBEAT blink pattern.

## Connectors

| Connector | Part class | Count |
|---|---|---|
| Power input | Barrel jack 5.5/2.5 mm, 60 V rated | 1 |
| Bus ports | Shielded THT RJ45, no magnetics | 4 |
| USB | USB-C receptacle | 1 |
| SWD | 1.27 mm 2×5 Cortex-debug header | 1 |
| Buttons | Tactile 6×6 SMD (RESET, BOOT) | 2 |

No expansion header / off-board I²C exposure / daughterboard socket. Master is a closed-function controller.

## GPIO budget (ESP32-S3-WROOM-1-N16R8)

### Available GPIOs on the N16R8 module

- Exposed IO pins on N16R8: IO0–IO21 + IO38–IO48 = 33 physical pins (IO33–IO37 consumed internally by Octal PSRAM).
- Minus hardware-fixed assignments:
  - IO19, IO20 → USB D+/D− (native USB, mandatory)
  - IO39, IO40, IO41, IO42 → JTAG/SWD (mandatory)
  - IO43, IO44 → UART0 console (policy-reserved; not remapped)
  - IO3, IO45, IO46 → strap pins, no copper escape
  - IO48 → glitch-prone, policy-avoided
- **General-purpose GPIOs available: 21.**

### Demand

| Function | Pins |
|---|---|
| SPI to MAX14830 (MOSI, MISO, SCK, CS, IRQ) | 5 |
| I²C bus (SDA, SCL) for INA237 bank | 2 |
| eFuse ENABLE × 4 (direct, firmware-sequenced bus-on) | 4 |
| PROT_FAULT_OR (wired-OR of LM74700 FAULT + 4× eFuse FAULT) | 1 |
| TELEM_ALERT_OR (wired-OR of 4× INA237 ALERT) | 1 |
| HEARTBEAT LED (on S3, not on MAX14830 — independent of downstream chip health) | 1 |
| **Total assigned** | **14** |
| **Spare** | **7** |

Spare pool (7 pins) reserved with 10 kΩ pull-ups + test pads for future signals. No new function gets assigned without a justification commit here.

### GPIO allocation policy (locked)

1. **Single owner per GPIO** unless explicitly a shared bus (SPI, I²C).
2. **UART0 (IO43/IO44) reserved** for boot/recovery/console. Not remapped at runtime.
3. **Strap pins IO3, IO45, IO46 no-route** beyond the module pad.
4. **IO48 unassigned** (policy avoid; acceptable use limited to non-critical test pad).
5. **INA237 ALERT lines wire-OR'd** onto a single TELEM_ALERT_OR pin (not 4 separate pins). Firmware identifies which fired via I²C poll of each INA237.
6. **eFuse FAULTs + LM74700 FAULT wire-OR'd** onto a single PROT_FAULT_OR pin. Same pin also drives the FAULT LED (hardwired).
7. **Specific pin numbers (IO4, IO5, …) are deferred** — to be allocated by the digital/comms agent with a Preferred + Rationale column, then reviewed here before locking.

## Mechanical decisions (now locked)

- **Board dimensions: 130 × 100 mm** (full derivation in `MECHANICAL_DESIGN.md` §1).
- **Layer count: 4** (1.6 mm FR-4, ENIG finish, 1 oz outer / 0.5 oz inner).
- **Antenna keep-out: 18 × 15 mm** copper-free zone at the top edge, all 4 layers. Module does **not** overhang — kept fully on-board for shipping durability.
- **Mounting: 4× M3 corner holes** (isolated pads, 0 Ω star-ground DNP per corner).
- **Enclosure: custom 3D-printed plastic**, user-fabricated. No metal walls within 15 mm of the antenna end. 4-corner M3 pattern maps to standard desktop-standoff or 3D-printed boss arrangements.

## Prototype / production target

- **Prototype run: 5 boards**, JLC PCBA assembly path, user-fabricated 3D-printed enclosure.
- **Not production-ready.** Parts marked "check stock" in the design docs are acceptable for the prototype run; substitution decisions deferred to BOM-freeze time rather than designed around.
- **Schematic capture path**: JLC EasyEDA service or DIY in KiCad — to be picked at capture time.

## Design artifacts (in this directory)

- `MASTER_DECISIONS.md` (this file) — source of truth; agents bind to this only.
- `POWER_DESIGN.md` — power subsystem schematic-level detail.
- `DIGITAL_DESIGN.md` — digital subsystem + complete ESP32-S3 pin allocation.
- `MECHANICAL_DESIGN.md` — placement brief, stack-up, zones, diff-pair rules.
- `MASTER_BOM.csv` — consolidated bill of materials for the master PCB.

## Still deferred (non-blocking, resolvable at schematic capture or fab-time)

Detail-level items listed in the design docs' own "Open issues" sections (P2, P3, O1, O3, O4, O5, M5, M6, M7, M8, M9). Each has a sensible default documented in its respective doc and does not require a further user decision before schematic capture begins.
