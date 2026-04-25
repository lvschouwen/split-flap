# Unit PCB v2 — locked design decisions

> **Single source of truth for the unit PCB.** Design agents bind to this file ONLY. No other file in this repo is authoritative for the v2 unit design.
>
> Last edited: 2026-04-25 (post-review backplane pivot + scottbez1 audit; see issue #76).
>
> Peer documents: `MASTER_DECISIONS.md` (master), `BACKPLANE_DECISIONS.md` (backplane). The unit **consumes** power + bus from the **backplane** (not from another unit) — anything in conflict between docs defers to `MASTER_DECISIONS.md` for system-level shape and `BACKPLANE_DECISIONS.md` for the per-slot connector contract.

## System shape (revised 2026-04-25)

- **One unit PCB per split-flap module.** 16 modules per case sit on a backplane PCB (4×4-slot segments, 320 mm each); the unit drops onto a per-slot 2×3 box header on the backplane.
- **Single MCU**: STM32G030K6T6 (Cortex-M0+, 32 KB flash / 8 KB RAM, **LQFP-32** at 7×7 mm). Was TSSOP-20 pre-2026-04-25 but pin map referenced PA12 / PB0 / PB1 which TSSOP-20 does not bond.
- **Stepper motor**: 12 V 28BYJ-48 unipolar, mechanical carry-over from v1. Off-board via 5-wire flying lead to a JST-XH 5-pin jack on the PCB.
- **Motor driver**: **TPL7407L** (TI, 7-channel low-side MOSFET array, ~0.1 Ω R_DS(on)) in SOIC-16. 4 of 7 channels drive the stepper coils; unused channels grounded. Replaces ULN2003A (Darlington, ~1.4 V Vce drop) per scottbez1 audit — pin-compatible drop-in, ~0.5 W less heat per unit at 300 mA stepping current.
- **Position sensing**: AS5600 absolute magnetic encoder on I²C (address 0x36) with **small RC filter on SDA/SCL** (100 Ω series + 100 pF shunt) to suppress buck-switching-node coupling. On-axis diametral magnet glued to the 28BYJ-48 output shaft. **No homing sweep** on boot; firmware detects + corrects missed steps using closed-loop feedback.
- **Bus**: RS-485 half-duplex at **500 kbaud 8N1** via SN65HVD75D transceiver (3.3 V, matches master). STM32G030 USART1 with hardware-assisted DE/RE via the USART DE alternate function. Wire format = COBS(payload || CRC16-BE) 0x00 per `firmware/lib/common/`.
- **Addressing (locked 2026-04-25): UID-based binary-tree discovery** driven by the master. Each unit's STM32G030 96-bit factory UID is the identity. Master broadcasts prefix-match queries; matching units echo their UID; collisions resolved by the master narrowing the prefix. Once enumerated, master assigns a 1-byte short address per unit which is stored in flash (wear-leveled ring, last 2 KB sector). **No DIP switches. No wake-pin scheme.** Pre-2026-04-25 the unit referenced a single-ended wake signal on RJ45 pin 1 — that scheme is deleted.
- **Firmware update path**: **OTA over RS-485** (master pushes images to each unit's application bootloader). First programming via SWD test pads using a pogo-pin jig. Test/bring-up alternative: **standalone UART debug mode** — when the SWD pogo-jig is connected without an active SWD interface, the unit firmware exposes a text protocol (`home`, `step N`, `angle?`) on the SWD pads' UART repurposing, allowing single-unit bench bring-up without the master/backplane (pattern adopted from scottbez1 #76 audit). **No USB** on the unit PCB.
- **Protection**: SMAJ51A TVS (matches master rail clamp class), per-unit P-channel MOSFET reverse-block on 48 V input, RS-485-rated SM712 ESD array on bus pins (matches master).
- **Status indicator**: 1 MCU-driven LED encoding state via blink pattern.
- **Fault reporting**: RS-485 polling only — unit sets status bits in its reply; master polls. No hardware back-channel.

## Backplane connector (replaces RJ45 IN/OUT)

> Per-unit RJ45 jacks are obsoleted by the backplane architecture. Each unit connects to the backplane through a single board-to-board header.

| Pin | Net | Notes |
|---|---|---|
| 1 | +48V | Paralleled with pin 3 for current capacity |
| 2 | RS-485 A | Differential pair with pin 4 |
| 3 | +48V | Paralleled with pin 1 |
| 4 | RS-485 B | Differential pair with pin 2 |
| 5 | GND | Paralleled with pin 6 |
| 6 | GND | Paralleled with pin 5 |

Connector type: **2×3 box header, 2.54 mm pitch** (male on unit, female on backplane). Standard SMD or THT 2.54 mm 2×3 part. Unit pins ~0.5 mm long after PCB; backplane socket polarised (keyed) to prevent reverse insertion.

Cable spec doesn't apply — this is a board-to-board interface inside the case.

## Power architecture

| Item | Decision |
|---|---|
| Input | 48 V from backplane connector pins 1, 3 (paralleled), GND on pins 5, 6 |
| Per-slot fuse | 0.2 A polyfuse on backplane (not on unit) |
| Input TVS | **SMAJ51A** bidirectional (V_BR ≈ 56.7 V; matches master rail clamp; pre-2026-04-25 used SMBJ58CA which would not have clamped before TPS54308's 60 V abs-max during master-side surge events) |
| Reverse-polarity | **Per-unit P-channel MOSFET reverse-block on +48 V**. Topology: source on input side, drain on load side, gate pulled high through 10 kΩ, gate-to-source Zener (12 V) clamp. Body diode points input → load (conducts at first power-up; channel then enhances). **Drain/source labels were reversed pre-2026-04-25**. Part: AO3401 (SOT-23, V_DS ≥ 30 V, R_DS(on) ~50 mΩ at V_GS=−10 V) — sufficient at 30 mA steady. Pre-2026-04-25 doc listed "SQ4953AEY or AO3401" with conflicting SOT-23/SO-8 footprints; AO3401 SOT-23 locked. |
| Motor rail (12 V) | **TPS54360DDA** (TI 60 V / 3.5 A sync buck, **HSOP-8 PowerPAD**). Replaces TPS54308DBV (SOT-23-6 with no thermal pad — its ≤80 °C/W spec was physically unachievable; #76 audit). V_ref = 0.8 V; FB divider populated for 12 V. |
| Logic rail (3.3 V) | **HT7833** LDO (SOT-89, ~500 mA rated, ~0.5 V dropout). Dissipates ~0.4 W at ~50 mA logic load — within SOT-89 thermal budget. |
| Per-unit OCP | Backplane per-slot 0.2 A polyfuse (not on unit). |

**Bus current budget** (12 V 28BYJ-48 motor + ~50 mA logic):
- Per unit steady-state at 48 V input: ~25 mA estimated (10 mA motor-hold + 15 mA logic-path through LDO, buck efficiency ~90%). **Re-measure on first prototype** — scottbez1 audit flagged that 28BYJ-48 stepping current is closer to 300 mA peak at 12 V than the 74 mA hold-current assumption.
- 16 units × 25 mA per case = ~400 mA per case at 48 V — well within backplane polyfuse budget.

## Inter-chip communication

| Path | Signal | Notes |
|---|---|---|
| RS-485 bus | STM32 USART1 ↔ SN65HVD75D ↔ backplane connector A/B | 500 kbaud, hardware DE via USART, COBS+CRC16-BE framing |
| Sensor | STM32 I²C1 ↔ 100 Ω series ↔ 100 pF shunt ↔ AS5600 | 400 kHz fast mode, addr 0x36, RC filter added 2026-04-25 |
| Stepper | STM32 4 GPIO ↔ TPL7407L IN1–IN4 → 28BYJ-48 coils | 8-step half-step sequencing in firmware |
| Status LED | STM32 GPIO → 1 kΩ → LED → GND | ~1.3 mA drive |
| Debug | 4 test pads (SWDIO, SWCLK, NRST, GND) | Pogo-pin jig (1.5 mm pads on 2.54 mm pitch) |

## GPIO budget (STM32G030K6T6, LQFP-32)

LQFP-32 exposes 28 GPIOs minus VDD/VSS/NRST → **~25 usable application GPIOs**.

| Function | Pins |
|---|---|
| USART1 TX / RX / DE | 3 |
| I²C1 SDA / SCL | 2 |
| TPL7407L IN1–IN4 | 4 |
| Status LED | 1 |
| **Total assigned** | **10** |
| **Spare** | **≥15** |

AS5600 DIR pin tied to GND or VCC — does not consume a GPIO.

### GPIO allocation policy (locked)

1. **SWD pair (PA13/PA14) reserved** for programming + debug. Not remapped at runtime.
2. **BOOT0 tied LOW** via 10 kΩ pull-down. No hardware bootloader entry path — OTA is firmware-initiated.
3. **NRST conditioning**: 100 nF cap NRST→GND + 10 kΩ pull-up to 3V3. Standard reset hygiene.
4. **USART1 DE uses hardware AF** — do NOT implement software RS-485 direction toggling.
5. **Specific pin numbers** documented in `UNIT_DIGITAL_DESIGN.md` preferred map. With LQFP-32, all map entries (PA12, PB0, PB1) are now bonded.

## LED set (1 total)

| # | Label | Driven by | Color | Encoding |
|---|---|---|---|---|
| 1 | STATUS | MCU GPIO | green or blue 0603 | off = dead · 2 Hz = awaiting address · solid = enumerated/idle · 4 Hz burst = stepping · 2× fast blink w/ pause = fault |

Single LED by design — saves BOM, area, and MCU pins across 128 units.

## Connectors

| Connector | Part class | Count |
|---|---|---|
| Backplane | 2×3 box header 2.54 mm pitch (male on unit) | 1 |
| Motor | JST-XH 5-pin vertical, 2.5 mm pitch | 1 |
| SWD | 4 test pads (SWDIO, SWCLK, NRST, GND) — 1.5 mm pads, 2.54 mm pitch | — |

No USB. No RJ45. No debug header (pads only). No auxiliary connectors.

## Termination (revised 2026-04-25 — moves to backplane)

> **Important architectural change**: with the backplane pivot, the chain ends at the case-level RJ45 on the chain-end backplane segment, NOT on a unit. Therefore the per-unit termination + jumper + shunt scheme from the pre-2026-04-25 design is **removed from the unit**. Termination lives on the backplane (PCBA stuffing variant; populated only on chain-end case backplane).

The unit has **no termination components**. The bus presents as a 6-pin board-to-board header to the backplane; termination is the backplane's responsibility.

Failsafe bias is provided **at the master only** (1 kΩ each leg); unit and backplane add no bias.

## Mechanical decisions

- **Board dimensions: target 75 × 35 mm** (revised 2026-04-25 from 65 × 35 mm — accommodates the larger STM32G030 LQFP-32 footprint, the AS5600 ferrous keep-out plus motor connector, and the TPS54360 HSOP-8 PowerPAD with adequate thermal pour).
- **Layer count: 2** (1.6 mm FR-4, 1 oz outer, HASL finish acceptable).
- **AS5600 placement**: on-axis with the 28BYJ-48 output shaft. Origin at AS5600 IC center (0,0). **Mechanical bracket designed by mech freelancer**; PCB hands off the AS5600 coordinate + magnet keep-out + mounting-hole positions in a STEP file. PCB does not constrain the bracket geometry.
- **Mounting**: 2× M3 isolated through-holes — final positions coordinated with the mechanical bracket via STEP exchange.
- **Backplane connector orientation**: 2×3 box header on the back-edge of the unit PCB so the unit slides perpendicular into the backplane along the case-back wall.

## Prototype / production target

- **Prototype run: 10 boards** via JLC PCBA. Enables 4 unit chain tests (one case worth) + spares for mechanical and destructive testing.
- Not production-ready. BOM items flagged "CHECK" at freeze time are acceptable substitutions for the prototype.
- **Schematic capture path**: KiCad preferred to enable the **KiBot + kikit production pipeline** (CI-driven JLCPCB output, LCSC alt-part fields). Adopted post-#76 audit from scottbez1.

## Design artifacts (in this directory)

- `UNIT_DECISIONS.md` (this file) — source of truth; agents bind to this only.
- `UNIT_POWER_DESIGN.md` — power subsystem schematic-level detail.
- `UNIT_DIGITAL_DESIGN.md` — digital subsystem + GPIO map + protocol-level notes.
- `UNIT_MECHANICAL_DESIGN.md` — placement brief, floorplan, AS5600 integration.
- `UNIT_BOM.csv` — JLC-native BOM.
- `UNIT_BRINGUP.md` — per-board bring-up sequence including standalone UART test mode.

## Resolved issues (2026-04-25 #76 audit)

- ~~U1 LDO P/N~~ — locked to HT7833 SOT-89.
- ~~U2 ESD array~~ — locked to SM712-02HTG (matches master; replaces SP0504BAATG which would clamp legitimate RS-485 traffic at 5 V).
- ~~U4 GPIO-to-pin assignments~~ — finalised in `UNIT_DIGITAL_DESIGN.md` against LQFP-32.
- ~~U5 RJ45 jack P/N~~ — N/A; unit no longer carries RJ45s.
- ~~U7 WAKE_OUT drive style~~ — N/A; UID-based discovery.
- ~~U8 AS5600 DIR pin level~~ — confirmed at bring-up; default GND (CW). Non-blocking.
- ~~U9 master-bias cross-check~~ — confirmed; master 1 kΩ each leg, unit/backplane no bias.
- ~~U10 termination jumper pitch~~ — N/A; termination on backplane.

## Still deferred (non-blocking)

- **U3 — Diametral magnet P/N + grade.** 6×2.5 mm N35 vs N42. Validate with an AS5600 breakout first. Off-board item.
- **U6 — Mounting hole positions.** Determined alongside 3D bracket design (mech freelancer).
- **U_STEP — Stepping current real-world measurement.** First-prototype task. Re-spec backplane polyfuse sizing if 28BYJ-48 stepping pulls > 300 mA peak.

## AS5600 datum problem (post external review)

AS5600 placement is **not a PCB placement note — it is a mechanical datum problem**. The unit is **not layout-ready** until all of the following are explicit and signed off by the mech freelancer (STEP exchange):

- **Magnet diameter and thickness** (currently 6 × 2.5 mm N35; not finalised — see U3).
- **Motor shaft XY datum** relative to the unit PCB origin (AS5600 IC center).
- **Allowed AS5600 XY error** vs the motor shaft axis (typical ams spec: ≤ 0.25 mm radial offset for full-resolution operation).
- **Target air gap** (1.5 mm nominal, allowed 0.5–3 mm) and **min/max guarantee** that the bracket + tolerance stack-up actually delivers.
- **Board mounting tolerance** within the bracket (M3 hole positions, oversize, locating features).

These five must appear in a single dimensioned drawing or STEP file before the unit PCB enters layout. Without them, the electronics can be perfect and the unit can still fail to read absolute angle.

## TPS54360 thermal plan (post external review)

TPS54360DDA HSOP-8 PowerPAD thermal copper / via plan must be specified **before layout**, not discovered during fab. Decisions:

- Exposed pad copper pour ≥ 200 mm² on the bottom layer with thermal stitching (≥ 6× 0.3 mm vias under the pad on the top, drawing heat to the bottom pour).
- Top-side copper relief around the pad to prevent solder-paste pull-off during reflow.
- Switch-node copper kept compact (≤ 100 mm² to limit EMI radiation) and away from AS5600 / I²C traces.

## SWD pogo + standalone UART access (post external review)

The unit's bring-up depends on:

- **SWD pogo fixture geometry** documented (4× 1.5 mm pads on 2.54 mm pitch, edge-accessible, oriented for jig insertion direction). Not negotiated at fab time.
- **Standalone UART test mode pad access** documented — the SWD pads (PA13/PA14) repurposed as UART RX/TX when no SWD master is present. Bring-up jig must be able to drive both modes from the same pogo footprint.

## LAYOUT_CONSTRAINT: unit pre-layout checklist

- **AS5600 IC center tied to motor shaft datum** with explicit tolerance (per the mech freelancer's STEP).
- **Magnet gap target** (1.5 mm nominal) and min/max guarantee from the bracket assembly.
- **Buck switch node** kept compact and **away from AS5600 / I²C traces** — at minimum, keep ≥ 5 mm separation and route AS5600 I²C on the opposite layer with GND pour between.
- **AS5600 I²C local RC filter (100 Ω series + 100 pF shunt)** placed close to the AS5600 side of the I²C net, not close to the MCU side, so the filter rejects buck-switching coupling at the encoder.
- **RS-485 ESD (SM712-02HTG)** placed close to the backplane connector entry, not close to the SN65HVD75 — protect the connector entry, not the transceiver.
- **Test pads** for: 12V (post-buck), 3V3, V48 after reverse-FET, NRST, SWDIO, SWCLK, UART/test-mode RX, UART/test-mode TX, RS-485 A, RS-485 B, RS-485 DE, RS-485 TX, RS-485 RX (where space allows).
- **TPL7407 IN5–IN7 hard-tied to GND** (not via 10 kΩ — false-turn-on risk during MCU reset).
- **Motor connector orientation and pinout** must match the actual 28BYJ-48 harness/motor variant supplied — verified at first-prototype mechanical fit.

## REVIEW_STRONG_OPINION: AS5600 alignment is the unit's hardest problem

If AS5600 / magnet alignment is not specified as a drawing with tolerances, the electronics can be perfect and the unit can still fail. Treat the mech freelancer hand-off as a critical-path layout-readiness blocker, not a parallel workstream.
