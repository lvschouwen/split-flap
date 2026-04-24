# Unit PCB v2 — locked design decisions

> **Single source of truth for the unit PCB.** Design agents bind to this file ONLY. No other file in this repo is authoritative for the v2 unit design.
>
> Last edited: 2026-04-24.
>
> Peer document: `MASTER_DECISIONS.md`. The unit **consumes** power + data from the master's Mode B RJ45 pinout; anything in conflict between the two docs defers to `MASTER_DECISIONS.md`.

## System shape

- **Single MCU**: STM32G030F6P6 (Cortex-M0+, 32 KB flash / 8 KB RAM, TSSOP-20).
- **Stepper motor**: 12 V 28BYJ-48 unipolar, mechanical carry-over from v1. Off-board via 5-wire flying lead to a JST-XH 5-pin jack on the PCB.
- **Motor driver**: ULN2003A NPN Darlington array, SOIC-16. 4 of 7 channels drive the stepper coils.
- **Position sensing**: AS5600 absolute magnetic encoder on I²C (address 0x36). On-axis diametral magnet glued to the 28BYJ-48 output shaft. **No homing sweep** on boot; firmware detects + corrects missed steps using closed-loop feedback.
- **Bus**: RS-485 half-duplex at **500 kbaud** via SN65HVD75D transceiver (3.3 V, matches master). STM32G030 USART1 with hardware-assisted DE/RE via the USART DE AF.
- **Addressing**: **Auto-enumeration** using a single-ended hardware wake signal on RJ45 pair 1/2. No DIP switch. Details below in RJ45 pinout.
- **Firmware update path**: **OTA over RS-485** (master pushes images to each unit's application bootloader). First programming via SWD test pads using a pogo-pin jig. **No USB** on the unit PCB.
- **Protection**: minimal — SMBJ58CA TVS on 48 V input, 4-channel ESD array across RJ45 data/wake pins. Relies on master's per-bus eFuse + LM74700 ideal-diode for primary over-current and reverse-polarity coverage.
- **Status indicator**: 1 MCU-driven LED encoding state via blink pattern.
- **Fault reporting**: RS-485 polling only — unit sets status bits in its reply; master polls. No hardware back-channel.

## RJ45 pinout (inherits master's Mode B, extends pair 1/2)

Master defines the IEEE 802.3at Mode B pinout (`MASTER_DECISIONS.md` §RJ45 pinout). The unit **extends** pair 1/2 usage from NC to a single-ended wake signal:

| Pin | T568B color | Net | Notes |
|---|---|---|---|
| 1 | white-orange | WAKE | Single-ended 3.3 V logic, low-bandwidth (enumeration only) |
| 2 | orange | NC | Reserved; consider GND-bond near jack if EMI testing reveals return-path issues |
| 3 | white-green | RS-485 A | Same as master |
| 4 | blue | +48V | Paralleled with pin 5 |
| 5 | white-blue | +48V | Paralleled with pin 4 |
| 6 | green | RS-485 B | Same as master |
| 7 | white-brown | GND | Paralleled with pin 8 |
| 8 | brown | GND | Paralleled with pin 7 |

**IN and OUT jacks are electrically symmetric** (passthrough) for every pin EXCEPT pin 1:
- IN-jack pin 1 → input to `WAKE_IN` GPIO (10 kΩ pull-down).
- OUT-jack pin 1 → driven by `WAKE_OUT` GPIO.
- All other pins (2, 3, 4, 5, 6, 7, 8) wire straight-through IN ↔ OUT on the PCB.

**Wake semantics**:
1. Master drives wake HIGH on each bus's port-1 output after its own boot.
2. Each unit's WAKE_OUT starts LOW at boot and is released HIGH **only after** the unit has successfully received an address from master.
3. At any moment, exactly one un-addressed unit sees WAKE_IN=HIGH — making collision-free enumeration trivial.

## Power architecture

| Item | Decision |
|---|---|
| Input | 48 V from RJ45 pair 4/5 & 7/8, always-on (master controls per-bus eFuse enable) |
| Input TVS | SMBJ58CA bidirectional near RJ45 power-pin entry |
| Motor rail (12 V) | **TPS54308DBV** (TI 60 V / 0.5 A sync buck, SOT-23-6). Soft-start ~2 ms. Feeds ULN2003 COM pin and LDO input. |
| Logic rail (3.3 V) | **HT7833** LDO (SOT-89, ~500 mA rated, ~0.5 V dropout). Dissipates ~0.4 W at ~50 mA logic load — within SOT-89 thermal budget. |
| Reverse-polarity | None on unit. Master's ideal-diode covers bus polarity. |
| Per-unit OCP | None. Master's per-bus eFuse (1.7 A) is the protection. |

**Bus current budget** (12 V 28BYJ-48 motor + ~50 mA logic):
- Per unit steady-state at 48 V input: ~25 mA (10 mA motor-hold + 15 mA logic-path through LDO, buck efficiency ~90%)
- 32 units × 25 mA = ~800 mA per bus → 47 % of 1.7 A eFuse trip point → ~53 % headroom for transient motor pulls.

**Boot inrush is intrinsically staggered** by the wake-propagation enumeration — no thundering-herd startup.

## Inter-chip communication

| Path | Signal | Notes |
|---|---|---|
| RS-485 bus | STM32 USART1 ↔ SN65HVD75D ↔ RJ45 A/B | 500 kbaud, hardware DE via USART |
| Sensor | STM32 I²C1 ↔ AS5600 | 400 kHz fast mode, addr 0x36 |
| Stepper | STM32 4 GPIO ↔ ULN2003A IN1–IN4 → 28BYJ-48 coils | 8-step half-step sequencing in firmware |
| Wake | STM32 WAKE_IN (pull-down input) + WAKE_OUT (push-pull output) | Protected by 100 Ω series + ESD clamp |
| LED | STM32 GPIO → 1 kΩ → LED → GND | ~1.3 mA drive |
| Debug | 4 test pads (SWDIO, SWCLK, NRST, GND) | Pogo-pin jig |

## GPIO budget (STM32G030F6P6, TSSOP-20)

TSSOP-20 exposes 18 GPIO minus mandatory reserves (VDD, VSS, NRST, PA13/PA14 for SWD) → **~13 usable application GPIOs**.

| Function | Pins |
|---|---|
| USART1 TX / RX / DE | 3 |
| I²C1 SDA / SCL | 2 |
| ULN2003 IN1–IN4 | 4 |
| WAKE_IN / WAKE_OUT | 2 |
| Status LED | 1 |
| **Total assigned** | **12** |
| **Spare** | **≥1** |

AS5600 DIR pin tied to GND or VCC — does not consume a GPIO.

### GPIO allocation policy (locked)

1. **SWD pair (PA13/PA14) reserved** for programming + debug. Not remapped at runtime.
2. **BOOT0 tied LOW** via 10 kΩ pull-down. No hardware bootloader entry path — OTA is firmware-initiated.
3. **USART1 DE uses hardware AF** — do NOT implement software RS-485 direction toggling.
4. **Specific pin numbers deferred** — schematic-capture agent proposes a Preferred + Rationale map, reviewed here before locking. Preferred allocation documented in `UNIT_DIGITAL_DESIGN.md`.

## LED set (1 total)

| # | Label | Driven by | Color | Encoding |
|---|---|---|---|---|
| 1 | STATUS | MCU GPIO | green or blue 0603 | off = dead · 2 Hz = awaiting address · solid = enumerated/idle · 4 Hz burst = stepping · 2× fast blink w/ pause = fault |

Single LED by design — saves BOM, area, and MCU pins across 128 units.

## Connectors

| Connector | Part class | Count |
|---|---|---|
| Bus IN | Shielded THT RJ45, no magnetics (matches master) | 1 |
| Bus OUT | Shielded THT RJ45, no magnetics | 1 |
| Motor | JST-XH 5-pin vertical, 2.5 mm pitch | 1 |
| SWD | 4 test pads (SWDIO, SWCLK, NRST, GND) — no header | — |

No USB. No debug header. No auxiliary connectors.

## Termination

120 Ω 0603 resistor populated on **every** unit, in series with a **2-pin 2.54 mm header + removable shunt**:
- Default: shunt **not fitted** → terminator disconnected from the bus.
- Chain-end unit: installer slides a standard jumper shunt onto the header → terminator across A/B.
- Single SKU for JLC PCBA; tool-free reconfiguration in the field (e.g. if the chain topology changes).
- Header P/N: generic 1×2 2.54 mm vertical male header (~€0.03); shunt is a standard open-top jumper (~€0.03).
- Alternative: 2 mm pitch if board area is tight — 2.54 mm is the default for installer familiarity.

Failsafe bias for RS-485 idle is provided **by the master side only** (cross-check pending in `MASTER_DECISIONS.md`); units add no bias resistors.

## Mechanical decisions

- **Board dimensions: target 65 × 35 mm** (within 86 × 40 mm hard max).
- **Layer count: 2** (1.6 mm FR-4, 1 oz outer, HASL finish acceptable).
- **AS5600 placement**: on-axis with the 28BYJ-48 output shaft. A 3D-printed bracket holds the PCB behind the motor; a shaft-end stub carries the diametral magnet at 0.5–3 mm airgap from the AS5600 IC face, no ferrous material within 5 mm.
- **Mounting**: 2× M3 isolated through-holes — final positions coordinated with the mechanical bracket.
- **RJ45 orientation**: IN and OUT on opposite short edges so cables exit opposing directions — enables linear daisy-chain routing along a wall of units.

## Prototype / production target

- **Prototype run: 10 boards** via JLC PCBA. Enables 3–5 unit chain tests + spares for mechanical and destructive testing.
- Not production-ready. BOM items flagged "check stock" at freeze time are acceptable substitutions for the prototype.
- **Schematic capture path**: pick at capture time (JLC EasyEDA or KiCad — same decision point as master).

## Design artifacts (in this directory)

- `UNIT_DECISIONS.md` (this file) — source of truth; agents bind to this only.
- `UNIT_POWER_DESIGN.md` — power subsystem schematic-level detail.
- `UNIT_DIGITAL_DESIGN.md` — digital subsystem + GPIO map + protocol-level notes.
- `UNIT_MECHANICAL_DESIGN.md` — placement brief, floorplan, AS5600 integration.
- `UNIT_BOM.csv` — consolidated bill of materials.

## Still deferred (non-blocking, resolvable at schematic capture or fab-time)

- **U1** — LDO exact P/N (HT7833 vs AP2127K-3.3 vs MIC5219-3.3). Default: HT7833 in SOT-89 for thermal margin.
- **U2** — ESD array exact P/N (ESD9B5.0 2-ch + separate wake ESD, vs SP0504 4-ch integrated, vs TPD4E1B06). Default: SP0504 4-ch.
- **U3** — Diametral magnet P/N + grade (6×2.5 mm N35 vs N42). Validate with an AS5600 breakout first.
- **U4** — Exact GPIO-to-pin assignments on TSSOP-20 (see `UNIT_DIGITAL_DESIGN.md` preferred map).
- **U5** — Exact RJ45 jack P/N. Match master's supplier choice for sourcing consistency.
- **U6** — Mounting hole pattern. Determined alongside 3D bracket design.
- **U7** — WAKE_OUT drive style: push-pull (3.3 V swing, fast) vs open-drain + pull-up (short-tolerant). Default push-pull; revisit if EMI testing flags it.
- **U10** — Termination jumper pitch: 2.54 mm (default, installer-familiar) vs 2.00 mm (smaller footprint if space becomes tight at layout).
- **U8** — AS5600 DIR pin level (CW vs CCW) finalized at bring-up.
- **U9** — Master-decisions cross-check: confirm master performs RS-485 termination + failsafe bias at its end of each bus. Add this to `MASTER_DECISIONS.md` in a follow-up session if not already documented.

None of U1–U9 blocks schematic capture.
