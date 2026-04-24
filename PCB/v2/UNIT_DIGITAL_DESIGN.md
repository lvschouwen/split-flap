# Unit PCB v2 — Digital subsystem design

Complement to `UNIT_DECISIONS.md` §Inter-chip communication. Schematic-level detail, GPIO map, and protocol-level notes.

## Block diagram

```
RJ45 IN                                                   RJ45 OUT
  │                                                             │
  ├── pair 3/6 ────────── A/B bus (tap) ────────────────────── pair 3/6
  │                              │
  │                       SN65HVD75D  ── DI / RO / DE ── STM32 USART1 (500 kbaud)
  │
  ├── pin 1 ── 100 Ω ── ESD clamp ── 10 kΩ pd ── WAKE_IN ── STM32 GPIO
  │                                                            │
  │                                                        [firmware enum FSM]
  │                                                            │
  └── pin 1 ── 100 Ω ── ESD clamp ──── WAKE_OUT ─────── STM32 GPIO
                                                               │
                                    STM32 I²C1 ── AS5600 (addr 0x36)
                                    STM32 4× GPIO ── ULN2003A ── 28BYJ-48
                                    STM32 1× GPIO ── STATUS LED
                                    STM32 SWD pads ── pogo-pin jig
```

## RS-485 bus wiring (one transceiver, two jacks)

IN and OUT RJ45 jacks are **electrically symmetric** on pins 3 (A) and 6 (B):
- IN jack pin 3 ↔ OUT jack pin 3 (bus A, shared node).
- IN jack pin 6 ↔ OUT jack pin 6 (bus B, shared node).
- One SN65HVD75D taps across A/B near the MCU.
- 120 Ω terminator in series with a 2-pin 2.54 mm jumper header across A/B; shunt not fitted by default (terminator disconnected). Chain-end unit gets a shunt fitted at install.

A 32-unit chain looks like one continuous A/B pair with master at one end and the chain-end terminator at the other. Each unit is a stub tap on the bus.

## SN65HVD75D (RS-485 transceiver)

| SN65HVD75 pin | Connected to | Notes |
|---|---|---|
| D (DI) | STM32 USART1_TX (PA9) | Drives bus when DE asserted |
| DE | STM32 USART1_DE (PA12, AF1) | Hardware RS-485 direction control |
| /RE | Tied to DE | Half-duplex; RX when DE low |
| R (RO) | STM32 USART1_RX (PA10) | Receive from bus |
| A | RJ45 pair 3/6 pin 3 | Bus A (paralleled IN/OUT) |
| B | RJ45 pair 3/6 pin 6 | Bus B (paralleled IN/OUT) |
| VCC | 3.3 V | 100 nF local bypass |
| GND | GND | — |

Baud: **500 kbaud 8N1**. SN65HVD75 slew-rate-limited driver matches this rate comfortably — no additional common-mode choke or ferrite needed at this speed.

## Wake signal chain

Per-jack:
- **RJ45 pin 1 → 100 Ω series → ESD clamp → 10 kΩ pull-down → WAKE_IN GPIO** (IN jack side).
- **WAKE_OUT GPIO → 100 Ω series → ESD clamp → RJ45 pin 1** (OUT jack side).

Firmware logic for WAKE_OUT:
- Boot: drive LOW.
- On successful enumeration (address received from master): drive HIGH.
- On address-lost / unrecoverable fault: drive LOW again (releases downstream chain).

Electrically single-ended (uses pin 1 only; pin 2 left NC or GND-bonded). Signal bandwidth is trivial — changes once per enumeration event. Single-ended is fine for 2-3 m CAT6 hop lengths between units.

## I²C1 / AS5600

| AS5600 pin | Connected to | Notes |
|---|---|---|
| VDD (1) | 3.3 V | 100 nF local bypass |
| VDD3V3 (8) | internal 3.3V node | Tie to VDD per datasheet when powered from 3.3 V |
| OUT (2) | NC | Not using analog/PWM output |
| GND (3, 7) | GND | — |
| DIR (4) | GND (CW default) | Pick orientation at bring-up; tie to VCC if reversed |
| SDA (5) | STM32 I2C1_SDA (PB7) | 4.7 kΩ pull-up to 3.3 V within 5 mm |
| SCL (6) | STM32 I2C1_SCL (PB6) | 4.7 kΩ pull-up to 3.3 V within 5 mm |

I²C clock: **400 kHz fast mode**. AS5600 default address: **0x36**.

## ULN2003A / stepper drive

| ULN2003 pin | Connected to | Notes |
|---|---|---|
| IN1–IN4 (1–4) | STM32 GPIO PA0–PA3 | 3.3 V GPIO direct drive — ULN2003 Vih = 2.4 V min, OK |
| IN5–IN7 (5–7) | GND | Tie unused inputs LOW to prevent floating |
| OUT1–OUT4 (13–16) | JST-XH 5-pin connector pins 1–4 | Motor coil drives |
| COM (9) | 12 V rail | Provides flyback return through internal diodes |
| GND (8) | GND | — |

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

## SWD programming interface

Four 2 mm test pads spaced 1.5 mm apart on one edge of the board:
- **SWDIO** ↔ STM32 PA13
- **SWCLK** ↔ STM32 PA14
- **NRST** ↔ STM32 NRST pin
- **GND** ↔ GND

No header. Pogo-pin jig touches pads during factory programming. Field updates go over RS-485.

**BOOT0** on STM32G030 is tied LOW via 10 kΩ pull-down. No hardware bootloader entry — OTA is firmware-initiated via RS-485 command.

## Preferred GPIO pin map (STM32G030F6P6, TSSOP-20)

| Pin | Signal | Rationale |
|---|---|---|
| PA0 | ULN_IN1 | GPIO, no AF conflict |
| PA1 | ULN_IN2 | — |
| PA2 | ULN_IN3 | — |
| PA3 | ULN_IN4 | — |
| PA4 | WAKE_IN | GPIO input with 10 kΩ pull-down |
| PA5 | WAKE_OUT | GPIO push-pull (or open-drain with pull-up) |
| PA6 | STATUS_LED | TIM3_CH1 available for optional brightness PWM |
| PA7 | spare | Reserved + test pad |
| PA9 | USART1_TX | AF1 — RS-485 DI |
| PA10 | USART1_RX | AF1 — RS-485 RO |
| PA12 | USART1_DE | AF1 — hardware RS-485 direction |
| PA13 | SWDIO | Mandatory |
| PA14 | SWCLK | Mandatory (also BOOT0 alt-func — tied LOW externally) |
| PB6 | I2C1_SCL | AF6 — default |
| PB7 | I2C1_SDA | AF6 — default |
| PB0 | spare | Reserved + test pad |
| PB1 | spare | Reserved + test pad |

Spare allocation (≥ 3 GPIOs): unused for v1 silicon — reserved as test pads for future silicon revisions or bring-up debug.

## Auto-enumeration protocol (firmware-level summary)

1. Unit boots. Sets `WAKE_OUT = LOW` (downstream locked out). `address = none`.
2. Unit polls `WAKE_IN`. If LOW, wait (upstream not ready).
3. When `WAKE_IN = HIGH` AND `address = none`: unit sends `ENUM_REQUEST` frame on bus containing its STM32 96-bit UID.
4. Master responds with `ENUM_ASSIGN(address = N)` frame addressed to that UID.
5. Unit stores `N` in flash (emulated EEPROM region) and sets `WAKE_OUT = HIGH`.
6. Next downstream unit now sees `WAKE_IN = HIGH`; repeats from step 2.

Collision handling: because only one un-addressed unit ever sees WAKE_IN=HIGH at a time, there is no bus contention during enumeration. No CSMA/CD logic needed.

Re-enumeration: master can broadcast `RESET_ENUM` to cause all units to clear their addresses and set WAKE_OUT LOW, then restart the wake-propagation sequence.

## Open issues (non-blocking)

- **D1** — WAKE_OUT drive style: push-pull (3.3 V swing, fast, dead-short intolerant) vs open-drain + external 10 kΩ pull-up (short-tolerant but slower edge). Default push-pull; revisit if EMI/cable-fault testing flags it.
- **D2** — AS5600 DIR level (CW = GND, CCW = VCC) confirmed at bring-up. Default GND.
- **D3** — 120 Ω termination mechanical placement: must be solder-jumper-accessible after final enclosure assembly on the chain-end unit. Confirm during mechanical layout.
- **D4** — Unused spare GPIOs: add 4× test pads to one edge for future rework (e.g. temperature sensor, extra LED, UART debug).
- **D5** — STM32G030 EEPROM emulation: confirm flash-wear budget is adequate for address + health counters. Default: wear-leveled ring in the last 4 KB of flash, rewritten only on address change or fault latch.
- **D6** — OTA security: master-unit authentication model for firmware-push commands is a firmware-level concern, not PCB-level. Captured here as a flag for the firmware agent: the hardware does not enforce signature verification — firmware must.
