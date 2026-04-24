# PCB v2 — RS-485 over CAT6, 48 V distributed, daisy-chain

Industrial redesign of the split-flap electrical system. Replaces the v1 / Rev-B I2C-over-cable + 5 V-distributed topology with a single RS-485 bus per port, power distributed over the same CAT6 cable, and a new STM32-based unit board. Per-bus current telemetry at the master (INA237) plus per-unit bus-voltage telemetry on the STM32 ADC gives the firmware end-to-end power visibility.

> **Status**: design-stage, capture-ready. This directory is the handoff brief (system architecture, schematic netlist, pinout, BOM). PCB layout is out of scope for this repo — JLCPCB EasyEDA service or a freelance layout engineer takes this brief forward to gerbers.
>
> **Firmware**: out of scope for v2 PCB delivery. STM32G030 unit firmware + master RS-485 driver are separate follow-up workstreams.

## Related issues

- [#73](https://github.com/lvschouwen/split-flap/issues/73) — this redesign (master + unit, docs only)

## What changed from v1 / Rev B

| Area | v1 / Rev B (I2C) | v2 (RS-485) |
|---|---|---|
| Unit-to-master link | I2C @ 100 kHz, 5 V logic, JST-XH 4-pin per row | RS-485 half-duplex @ 250 kbps, 3.3 V logic → differential on CAT6 |
| Cable | 4-wire JST-XH, unshielded, ≤ 1 m per row | CAT5e/CAT6 with RJ45, shielded, ≥ 30 m per daisy-chain |
| Topology | Star from master, 8 rows in parallel | Two independent daisy chains from master, up to 64 units each |
| Per-master capacity | 8 rows × 16 units = 128 units (one row per TCA9548A channel) | 2 buses × 64 units = 128 units (one bus per RJ45 port) |
| Power | 5 V distributed, per-row 2 A polyfuse | 48 V distributed, per-unit 500 mA polyfuse on the local tap |
| Unit MCU | Arduino Nano (ATmega328P, I2C slave, DIP-switch addressed) | STM32G030F6P6 (Cortex-M0+, RS-485 slave, unique-ID enumerated) |
| Unit bootloader | twiboot over I2C (AVR) | STM32 system bootloader over UART via RS-485 (Cortex-M) |
| Mux / level shift on master | TCA9548APWR + 8× PCA9306DCTR | _removed_ — RS-485 is a single electrical bus per port, no mux |
| Addressing | 4-bit DIP switch, fixed | Dynamic, stored in flash, set by master over the bus |
| Power telemetry | _none_ | Master: 2× INA237 (per-bus current + voltage, I²C). Unit: STM32 ADC voltage monitor on local +48 V tap. |

## System architecture

```
                   ┌──────────────────── MASTER ──────────────────────┐
                   │                                                  │
   48 V DC ─ J_PWR ┤ TVS + polyfuse + reverse-polarity PMOS           │
                   │        │                                         │
                   │        ├─► LMR16030 48 V → 5 V buck ─► 5V_RAIL   │
                   │        │                              (LED bar)  │
                   │        └─► AP63300 5 V → 3.3 V buck ─► 3V3_RAIL  │
                   │                                                  │
                   │  ESP32-S3 ◄── UART 921 600 bps ──► ESP32-H2      │
                   │      │                             (Zigbee/BLE)  │
                   │      ├── I²C (IO8/IO9) ─► INA237_A (bus A)       │
                   │      │                 └► INA237_B (bus B)       │
                   │      ├── USART1 ─► SN65HVD75 (Bus A) ─► J_BUS_A  │
                   │      └── USART2 ─► SN65HVD75 (Bus B) ─► J_BUS_B  │
                   │                                                  │
                   │  Per bus: high-side 50 mΩ 1 W shunt on +48V,     │
                   │  INA237 reads V_bus + I_bus, logged + exposed    │
                   │  on web UI / Zigbee                              │
                   │                                                  │
                   │  TLC5947 + SK6812 LED bar (unchanged from Rev B) │
                   │  USB-C (debug + programming, unchanged)          │
                   └──────────────────────────────────────────────────┘
                                      │        │
                                RJ45 bus A    RJ45 bus B
                                      │        │
                ┌───── CAT5/CAT6 ─────┤        ├───── CAT5/CAT6 ─────┐
                │                     │        │                     │
                ▼                     ▼        ▼                     ▼
           ┌────────┐            ┌────────┐ ┌────────┐           ┌────────┐
           │ Unit 1 │──CAT6 ──►  │ Unit 2 │ │ Unit 1 │──CAT6 ──► │ Unit 2 │ …
           └────────┘            └────────┘ └────────┘           └────────┘
             (up to 64 units per bus, daisy-chained)

   Unit:
     RJ45 IN  ──┬── passthrough ── RJ45 OUT
                │
                ├── +48 V tap ── F_LOCAL ── D_LOCAL_TVS ──┬──► V_48_MON divider ─► PA0 (ADC1_IN0)
                │                                         │
                │                                         └──► LMR16006 48→5V buck ─► MCP1700 5→3.3V
                │                                                  │
                │                                                  ▼
                │                                             STM32G030F6P6
                │                                                  │
                └── RS-485 A/B tap ── SM712 TVS ── SN65HVD75 ── USART2
                                                         │
                                                         ├── DE/RE (hardware DE via STM32 G0 USART)
                                                         │
                                                         └── ULN2003 ── 28BYJ-48 stepper
                                                                + KY-003 hall
                                                                + status LED
                                                                + identify button
                                                                + V_48_MON telemetry → master over RS-485
```

## Cabling — CAT5e / CAT6 with RJ45 (LOCKED pin assignment)

Pin numbering per TIA/EIA 568B, measured with the RJ45 clip **down** and pins facing away:

| Pin | T568B wire color | Net | Notes |
|---|---|---|---|
| 1 | white-orange | **+48V** | paired with pin 2 |
| 2 | orange       | **+48V** | paired with pin 1 |
| 3 | white-green  | **GND**  | paired with pin 6 |
| 4 | blue         | **RS485_A** | paired with pin 5 (center pair, tightest twist) |
| 5 | white-blue   | **RS485_B** | paired with pin 4 |
| 6 | green        | **GND**  | paired with pin 3 |
| 7 | white-brown  | **+48V** | paired with pin 8 |
| 8 | brown        | **+48V** | paired with pin 7 |

Rationale:
- **RS-485 on the blue pair (pins 4/5)**: center pair in the connector, tightest twist — minimizes crosstalk from the +48V pairs.
- **GND on the green pair (pins 3/6)**: adjacent to the signal pair so the RS-485 transceivers' ground reference stays tight to the signal pair within the cable.
- **+48V on orange + brown pairs (pins 1/2 + 7/8)**: two full pairs paralleled → 4× 24 AWG conductors for power. DC resistance ≈ 21 mΩ/m (four paralleled). Ampacity per 24 AWG conductor ≈ 1.5 A continuous.

**Maximum safe bus current: 1.5 A @ 48 V ≈ 72 W.** Limiting factor is the 2-conductor GND pair at 24 AWG. CAT6 with 23 AWG conductors (thicker) pushes this to 2.0 A / 96 W.

Cable must be straight-through (wire 1→1, 2→2, …, 8→8). Crossover cables will short +48V to GND and damage both master and the first unit on the bus.

## Power topology

- **PSU**: external 48 V DC, 2 A brick (specified for the system). 96 W raw.
- **Distribution**: pairs 1/2/7/8 of CAT6 carry +48V; pairs 3/6 return GND. Cable is a single electrical bus per RJ45 port; the two pins of each pair are paralleled on the unit PCB. RJ45 OUT connects pin-for-pin to RJ45 IN on the same unit's board.
- **Per-unit protection**: the local +48V tap (inside the unit board, between the passthrough and the local buck) goes through a polyfuse (`F_LOCAL` 500 mA hold) and a bidirectional TVS (`D_LOCAL_TVS` SMBJ58CA) to isolate a single unit failure from the rest of the chain.
- **Bus termination**: 120 Ω + fail-safe bias at the master end (always populated). At the far-end unit, a 120 Ω termination is wired to a jumper (`JP_TERM`) that is populated only on the physical last unit of each bus. Every other unit on the chain leaves `JP_TERM` open.

## Power telemetry

Two independent measurement points so the master can localize any power anomaly:

### Master — INA237 per bus (current + voltage at master-side of cable)

| Item | Value |
|---|---|
| Part | INA237AIDGSR (TI, MSOP-10, 85 V bus + common-mode, 16-bit, I²C @ 400 kHz) |
| Quantity | 2 (one per RJ45 bus) |
| Topology | High-side shunt on +48V between master rail and each `J_BUS_{A,B}` |
| Shunt | 50 mΩ, 1 W, 2512, 1 % — CSR2512FK50L0 or equivalent. At 1.5 A: 75 mV across shunt, 112 mW dissipation. |
| I²C address | 0x40 (Bus A, A0=GND) / 0x41 (Bus B, A0=VS) |
| I²C bus | ESP32-S3 I²C1 on IO8 (SDA) / IO9 (SCL), 4.7 kΩ pullups to 3V3 |
| Range | V_bus 0–85 V → resolution 3.125 mV/LSB. V_shunt ±81.92 mV → resolution 2.5 µV/LSB. Current (50 mΩ shunt): ±1.64 A → 50 µA/LSB. |

What it tells you: total current drawn by each bus in real time, plus the bus-side voltage at the master (useful for spotting a PSU sag vs. an upstream cable drop).

Why INA237 and not INA219: INA219 is rated for 26 V bus maximum — would be destroyed by 48 V. INA237 is the 85 V-capable direct analog of INA219 (same I²C pattern, same register-set philosophy).

### Unit — STM32 ADC + resistor divider (voltage at the far end of the cable)

| Item | Value |
|---|---|
| Divider | `R_MON_TOP` 150 kΩ 1 % 0603 + `R_MON_BOT` 10 kΩ 1 % 0603 → ratio 1/16 |
| Filter | `C_MON` 100 nF X7R 0603 between divider midpoint and GND → τ ≈ 9.4 ms |
| ADC input | PA0 (STM32G030 ADC1_IN0, 12-bit) |
| Measurement range | 0–52.8 V maps to 0–3.3 V ADC (slight compression above 52 V; comfortable margin at nominal 48 V) |
| Resolution | 48 V / 4096 × 16 ≈ 13 mV per LSB at the bus |
| Quiescent draw | 48 V / 160 kΩ = 300 µA = 14.4 mW per unit, continuous |
| Reporting | Firmware samples `V_48_MON` periodically and reports over RS-485 on master poll or when an out-of-band alarm (out-of-spec voltage) fires |

What it tells you: the **actual bus voltage at the far end of the cable**, where ohmic drop across the chain is visible. Correlating master-side current + unit-side voltage lets the firmware distinguish "PSU weak" from "long cable / many units" from "single unit shorted."

## Firmware contract (binding on both master and unit firmware)

These rules are pinned into the hardware so the firmware contract is enforceable, not just aspirational:

1. **The master is the only controller.** Units are RS-485 slaves only. No unit initiates traffic autonomously.
2. **Units do not run motors on their own.** The ULN2003 stepper outputs are only energized in response to an explicit master-issued command. Homing and calibration routines are also master-triggered.
3. **Bus mastering**: the RS-485 driver is enabled (DE = 1) only while the MCU is actively transmitting a byte. The STM32 G0 family's USART hardware DE feature handles this automatically — no firmware per-byte DE management.
4. **Addressing**: units come up with no assigned address. The master enumerates on boot by broadcasting `ENUMERATE`; each unit responds with its 96-bit unique ID; the master assigns a 1-byte logical address, which the unit persists in its last flash page.
5. **Identify button**: a short press (< 500 ms) on a unit blinks its status LED for 3 s and triggers an unsolicited `IDENTIFY` frame to the master. A long press (≥ 3 s) clears the stored logical address and forces re-enumeration on the next master boot.
6. **Telemetry**: every unit-reply frame carries its latest `V_48_MON` sample (1-byte compressed, 0.25 V resolution over 0–63.75 V). The master reads both INA237s every 100 ms and fans per-bus current / voltage to the UI.

## Key specs

| Item | Value |
|---|---|
| System power input | 48 V DC, 2 A (spec), via master barrel jack (5.5 / 2.5 mm, 60 V rated) |
| Bus physical layer | RS-485 half-duplex, 3.3 V logic, differential 120 Ω |
| Bus baud rate | **250 kbps** 8N1 (conservative for 30 m runs with 64 taps) |
| Bus cabling | CAT5e / CAT6, straight-through, RJ45 male |
| Master capacity | 2 buses × 64 units = **128 units** |
| Unit count per bus | ≤ 64 (hard cap enforced by 1-byte logical addressing, 0x01..0x40) |
| Per-bus cable current cap | **1.5 A** (CAT6 24 AWG GND conductor limit) |
| Master MCU | ESP32-S3-WROOM-1-N16R8 (primary) + ESP32-H2-MINI-1-N4 (radio coprocessor) |
| Master 48→5V | LMR16030SDDAR synchronous buck (60 V in, 3 A out) |
| Master 5→3.3V | AP63300WU-7 synchronous buck (3.8–32 V in, 3 A out) |
| Master RS-485 | SN65HVD75D × 2 (one per bus), 3.3 V, half-duplex |
| Master power telemetry | 2× INA237AIDGSR + 2× 50 mΩ shunts (one per bus, high-side) |
| Master LED bar | TLC5947 (14 × 5 V fixed) + SK6812-mini × 16 RGB (unchanged from Rev B) |
| Unit MCU | STM32G030F6P6 (TSSOP-20, 32 KB flash, 8 KB RAM, Cortex-M0+ @ 64 MHz) |
| Unit 48→5V | LMR16006YDDCR synchronous buck (60 V in, 0.6 A out) |
| Unit 5→3.3V | MCP1700-3302E/TT LDO (1.6 µA quiescent, 250 mA out) |
| Unit RS-485 | SN65HVD75D (same part as master) |
| Unit stepper driver | ULN2003AD (SOIC-16), 4 channels used for 28BYJ-48 unipolar |
| Unit voltage telemetry | 150 kΩ / 10 kΩ divider + 100 nF filter on +48V tap → PA0 ADC |
| Unit connectors | 2× RJ45 shielded THT (IN + OUT daisy-chain), 1× JST-XH-5 motor, 1× 3-pin hall sensor |
| Master board target | 120 × 80 mm, 4-layer |
| Unit board target | 30 × 50 mm, 2-layer |

## CAT6 voltage drop (sanity check)

24 AWG copper: ~84 mΩ/m per conductor. With +48V on 4 paralleled conductors and GND on 2 paralleled conductors, the **loop resistance** per metre of cable is:

```
R_loop = 84 mΩ/m ÷ 4  +  84 mΩ/m ÷ 2  =  21 + 42  =  63 mΩ/m
```

Worst-case end-of-chain voltage at full cable current:

| Cable length | R_loop total | V drop @ 0.5 A | V drop @ 1.0 A | V drop @ 1.5 A |
|---|---|---|---|---|
| 10 m | 0.63 Ω | 0.32 V | 0.63 V | 0.95 V |
| 30 m | 1.89 Ω | 0.95 V | 1.89 V | 2.84 V |
| 50 m | 3.15 Ω | 1.58 V | 3.15 V | 4.73 V |

End-of-chain unit sees ≥ 43 V even on a 50 m fully-loaded bus. The LMR16006 accepts 4.0–60 V input, so the unit still boots and operates cleanly anywhere on the chain. The unit's own `V_48_MON` reading is the ground-truth telemetry for this drop.

## Files

- `README.md` (this file) — system architecture handoff brief
- `GPIO_POLICY.md` — frozen rules governing master ESP32-S3 GPIO allocation (allocation order, reserved pins, enforcement). Specific pin numbers pending — see `OPEN_QUESTIONS.md`.
- `OPEN_QUESTIONS.md` — design decisions still open on v2 (GPIO architecture, power topology, protection class, bias values, layer counts, etc.). Each entry has options + recommendation. Resolve top-to-bottom before schematic respin.
- `SCHEMATIC.md` — textual netlist for the master and the unit. **Stale with respect to `OPEN_QUESTIONS.md` Q4–Q12, Q14, Q17, Q18**; pending resolution.
- `PINOUT.md` — full GPIO tables for ESP32-S3, ESP32-H2, STM32G030F6P6, plus RJ45 pin-to-net table. **ESP32-S3 section will be rewritten from scratch once `OPEN_QUESTIONS.md` Q1–Q3 are resolved** — current pin numbers are legacy Rev B carryover, not authoritative.
- `BOM.csv` — combined bill of materials for both boards (`Board` column distinguishes MASTER vs UNIT). Subject to changes from the open questions.
- `master_v2.kicad_sch`, `master_v2.kicad_pcb`, `master_v2_mockup.svg` — board-outline scaffolding retained from the Rev B attempt. Board dimensions (120 × 80 mm) are the only re-usable artefact; component placement must be recaptured from the new `SCHEMATIC.md` on top of a fresh layout.

## Handoff workflow (sequential)

1. **Schematic capture** (master + unit as two separate projects) in target EDA from `SCHEMATIC.md` + `BOM.csv`. ERC must pass with zero errors.
2. **Pre-layout review** — check that every critical rule in `SCHEMATIC.md §Critical Design Rules` is honoured in the captured schematic.
3. **Placement + routing** — master 120 × 80 mm 4-layer, unit 30 × 50 mm 2-layer.
4. **DRC report** delivered with gerbers, plus a controlled-impedance coupon on the RS-485 differential pair of each board.
5. **Fab & assembly** — JLCPCB PCBA, ENIG finish for the master (USB-C solder robustness); HASL fine for the unit.
6. **Bring-up** — power the master first with no units attached, verify RS-485 idle-bias levels and both INA237s over I²C, then hot-plug one unit at a time and confirm enumeration + `V_48_MON` reports on each connection.
