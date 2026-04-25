# Unit PCB v2 — Digital subsystem design

Complement to `UNIT_DECISIONS.md` §Inter-chip communication. Schematic-level detail, GPIO map, and protocol-level notes. Last edited 2026-04-25 (#76 backplane pivot + UID discovery + part swaps).

## Block diagram

```
Backplane connector (2×3 box header, 2.54 mm)
  │  pins 1, 3: +48V (paralleled)
  │  pins 5, 6: GND (paralleled)
  │  pin 2: RS-485 A
  │  pin 4: RS-485 B
  │
  ├── A/B ─── SM712 ESD clamp ── SN65HVD75D ── DI / RO / DE ── STM32 USART1 (500 kbaud, COBS+CRC16-BE)
  │
  └── 48V → SMAJ51A TVS → AO3401 P-FET reverse-block → TPS54360DDA buck → 12V
                                                                            │
                                                                          HT7833 LDO → 3V3
                                                                            │
                                            STM32 I²C1 ── 100Ω+100pF RC filter ── AS5600 (addr 0x36)
                                            STM32 4× GPIO ── TPL7407L IN1-4 ── 28BYJ-48 (via JST-XH)
                                            STM32 1× GPIO ── STATUS LED
                                            STM32 SWD pads (1.5 mm pads, 2.54 mm pitch) ── pogo-pin jig
```

## RS-485 bus wiring (one transceiver, one backplane connector)

The unit taps across A/B (backplane connector pins 2, 4) via a single SN65HVD75D near the MCU. There is no IN/OUT pair on the unit — the bus is presented by the backplane already and the unit is a single stub on the multi-drop bus.

- **No termination on the unit.** Termination lives on the backplane (PCBA stuffing variant; populated only on the chain-end case backplane segment per `BACKPLANE_DECISIONS.md`).
- **No failsafe bias on the unit.** Bias is at the master only (1 kΩ each leg).
- The unit-side stub from connector to SN65HVD75 A/B pins is ≤ 30 mm — well within RS-485 stub-length limits at 500 kbaud.

## SN65HVD75D (RS-485 transceiver)

| SN65HVD75 pin | Connected to | Notes |
|---|---|---|
| D (DI) | STM32 USART1_TX (PA9) | Drives bus when DE asserted |
| DE | STM32 USART1_DE (PA12, AF1) | Hardware RS-485 direction control (PA12 is bonded on LQFP-32) |
| /RE | Tied to DE | Half-duplex; RX when DE low |
| R (RO) | STM32 USART1_RX (PA10) | Receive from bus |
| A | Backplane connector pin 2 | Bus A (multi-drop stub) |
| B | Backplane connector pin 4 | Bus B (multi-drop stub) |
| VCC | 3.3 V | 100 nF local bypass |
| GND | GND | — |

Baud: **500 kbaud 8N1**. Wire format: COBS(payload || CRC16-BE) 0x00 framing per `firmware/lib/common/`. SN65HVD75 slew-rate-limited driver matches the rate comfortably; no additional common-mode choke or ferrite needed at this speed.

## I²C1 / AS5600 (RC filter added 2026-04-25)

| AS5600 pin | Connected to | Notes |
|---|---|---|
| VDD (1) | 3.3 V | 100 nF local bypass |
| VDD3V3 (8) | internal 3.3V node | Tie to VDD per datasheet when powered from 3.3 V |
| OUT (2) | NC | Not using analog/PWM output |
| GND (3, 7) | GND | — |
| DIR (4) | GND (CW default) | Pick orientation at bring-up; tie to VCC if reversed |
| SDA (5) | 100 Ω series → STM32 I2C1_SDA (PB7) | 4.7 kΩ pull-up to 3.3 V within 5 mm; 100 pF shunt on AS5600 side |
| SCL (6) | 100 Ω series → STM32 I2C1_SCL (PB6) | Same RC topology |

I²C clock: **400 kHz fast mode**. AS5600 default address: **0x36**.

**RC filter rationale (added 2026-04-25 per #76 audit)**: with a 600 kHz buck switch node ~10 mm from the I²C bus on a 75×35 mm board, common-mode noise can couple onto SDA/SCL. 100 Ω + 100 pF gives a low-pass corner at ~16 MHz — well above 400 kHz I²C operation but rejects buck-harmonic edges. Cost: 4 passives, ~€0.04. Routes SDA/SCL away from the switch node loop regardless.

## TPL7407L / stepper drive (replaces ULN2003 — 2026-04-25 per scottbez1 audit)

> **Replacement**: TPL7407L is a pin-compatible MOSFET drop-in for ULN2003 (TI's modern equivalent). R_DS(on) ~0.1 Ω vs ULN2003's ~1.4 V Vce(sat) Darlington saturation — saves ~0.5 W per unit at 300 mA stepping current. Same SOIC-16 footprint.

| TPL7407L pin | Connected to | Notes |
|---|---|---|
| IN1–IN4 (1–4) | STM32 GPIO PA0–PA3 | 3.3 V GPIO direct drive — TPL7407L Vih ≈ 2.0 V, OK |
| IN5–IN7 (5–7) | GND | **Hard-tied directly to GND copper, NOT through 10 kΩ** (corrected 2026-04-25 — pre-existing R_ULN_PD removed) |
| OUT1–OUT4 (13–16) | JST-XH 5-pin connector pins 1–4 | Motor coil drives |
| COM (9) | 12 V rail | Provides flyback return through internal MOSFET body diodes |
| GND (8) | GND (motor-return zone) | — |

JST-XH 5-pin pinout (matches 28BYJ-48 factory connector):
- Pin 1: Coil 1 (blue)
- Pin 2: Coil 2 (pink)
- Pin 3: Coil 3 (yellow)
- Pin 4: Coil 4 (orange)
- Pin 5: Common (red) → 12 V rail

**Stepping mode**: 8-step half-stepping (phase sequence A, AB, B, BC, C, CD, D, DA) in firmware. Smoother than full-step and standard for 28BYJ-48.

## Status LED

- STM32 GPIO → 1 kΩ series resistor → LED anode → cathode to GND (sink, GPIO drives HIGH to light).
- Alternative: source-drive with LED to VCC via 1 kΩ (GPIO LOW to light). Pick whichever matches firmware idle state preference.
- Current: (3.3 − 2.0) / 1000 ≈ **1.3 mA** — bright enough indoors, well under MCU GPIO limits.

## SWD programming interface (revised 2026-04-25)

Four **1.5 mm pads on 2.54 mm pitch** (industry-standard Tag-Connect TC2030 footprint or simple 0.05" pads), single row, on one edge of the board:
- **SWDIO** ↔ STM32 PA13
- **SWCLK** ↔ STM32 PA14
- **NRST** ↔ STM32 NRST pin (with 100 nF cap NRST→GND + 10 kΩ pull-up to 3V3)
- **GND** ↔ GND

No header. Pogo-pin jig touches pads during factory programming. Field updates go over RS-485. Pre-2026-04-25 spec was 2 mm pads on 1.5 mm spacing — too tight (0.5 mm clearance is below most pogo-jig design rules); corrected per #76 audit.

**Standalone test mode (added 2026-04-25 per scottbez1 audit)**: when the SWD pogo-jig connects without an active SWD master, unit firmware exposes a text protocol on PA13/PA14 repurposed as UART-RX/TX. Commands: `home`, `step <N>`, `angle?`, `id?`. Allows single-unit bench bring-up without master/backplane. Documented in `UNIT_BRINGUP.md`.

**BOOT0** on STM32G030 is tied LOW via 10 kΩ pull-down. No hardware bootloader entry — OTA is firmware-initiated via RS-485 command.

## Preferred GPIO pin map (STM32G030K6T6, LQFP-32)

| Pin | Signal | Rationale |
|---|---|---|
| PA0 | TPL_IN1 | GPIO, no AF conflict |
| PA1 | TPL_IN2 | — |
| PA2 | TPL_IN3 | — |
| PA3 | TPL_IN4 | — |
| PA6 | STATUS_LED | TIM3_CH1 available for optional brightness PWM |
| PA9 | USART1_TX | AF1 — RS-485 DI |
| PA10 | USART1_RX | AF1 — RS-485 RO |
| PA12 | USART1_DE | AF1 — hardware RS-485 direction (bonded on LQFP-32) |
| PA13 | SWDIO / standalone-mode UART RX | Mandatory + dual-purpose |
| PA14 | SWCLK / standalone-mode UART TX | Mandatory (also BOOT0 alt-func — tied LOW externally) |
| PB6 | I2C1_SCL | AF6 — default |
| PB7 | I2C1_SDA | AF6 — default |
| PB0 | spare | Test pad — bonded on LQFP-32 (was unbonded on TSSOP-20 pre-2026-04-25) |
| PB1 | spare | Test pad — bonded on LQFP-32 |
| PA4, PA5, PA7, PA8, PA11, PA15, PB2-PB5, PB8, PB9 | spare | Reserved + test pads (LQFP-32 bonds many more pins than TSSOP-20) |

Spare GPIO pool: ≥ 15 — generous future-proofing on LQFP-32. The pre-2026-04-25 TSSOP-20 map referenced PA12, PB0, PB1 which were unbonded on that package — corrected by package upgrade.

## Auto-enumeration protocol (revised 2026-04-25 — UID-based binary tree)

> **Architectural change**: pre-2026-04-25 spec used a single-ended wake signal on RJ45 pin 1 to serialise enumeration. With the backplane pivot (no per-unit RJ45) and per `MASTER_DECISIONS.md` system-shape, enumeration is now **UID-based binary-tree search over RS-485** — pure firmware, no per-unit hardware support.

Master-driven binary tree search adapted from 1-Wire ROM search:

1. **Boot**: unit reads its 96-bit factory UID from STM32 system memory. Sets `address = stored-or-none` from flash. Drives bus output high-Z when DE not asserted.
2. **Master broadcasts `SEARCH_PREFIX(prefix_bits, prefix_value)`**: all unaddressed units whose UID matches the prefix respond with their full UID + a CRC.
3. **Master listens**:
   - 1 unit responded with valid CRC → master records the UID.
   - Multiple units responded → bus collision → CRC fails → master narrows the prefix by one bit and recurses.
   - 0 responses → no units in this branch.
4. **Master assigns a 1-byte short address** to each discovered UID via `ASSIGN(uid, addr=N)` — the addressed unit stores N in flash (wear-leveled ring, last 2 KB sector — see D5).
5. **Master persists the UID→address map in NVS** (also UID→physical-slot map for calibration continuity across re-flashes).

Cold first-ever boot: ~5–20 s for 32 units (scales as binary depth × per-bit RTT). Warm boot (UIDs already known): ~100 ms.

Re-enumeration: master can broadcast `RESET_ADDR` to clear all addresses, then restart the search. Hot-replace: master sees one un-claimed UID after periodic re-search → assigns next available address.

## Open issues (non-blocking)

- ~~**D1**~~ **Closed 2026-04-25.** WAKE_OUT scheme deleted; UID-based discovery replaces it.
- **D2** — AS5600 DIR level (CW = GND, CCW = VCC) confirmed at bring-up. Default GND.
- ~~**D3**~~ **Closed 2026-04-25.** Termination moved to backplane.
- **D4** — Unused spare GPIOs: ≥ 15 spares on LQFP-32. Test pads on a few selected (PB0, PB1, PA4, PA8) for future rework.
- **D5** — STM32G030 wear-leveling: ≤ 100 enumeration events lifetime expected; flash endurance ~10k → ~100× margin. Wear-leveled ring in last 2 KB sector with 256 B records; rewrite only on address change or fault latch. Locked spec, non-blocking.
- **D6** — OTA security: master-unit authentication for firmware-push is a firmware-level concern. Hardware does not enforce signature verification — firmware must.
- ~~**D7**~~ **Closed 2026-04-25.** Standalone test mode locked (UART repurpose on SWD pads — see SWD section).
