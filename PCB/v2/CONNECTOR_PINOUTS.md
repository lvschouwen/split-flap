# Split-Flap v2 — Connector Pinout Contracts

> Single canonical reference for all inter-board wiring. If anything in this file disagrees with a per-board doc, **this file wins** for the pinout — the per-board doc owns the schematic detail behind the pinout.
>
> Created 2026-04-25.

## Topology

```
[ Master PCB ]
    │ 4× RJ45 ports (Bus 1..4)
    │   shielded CAT5e/6 patch (S/FTP or F/UTP, T568B straight-through)
    ↓
[ Case-1 Backplane (4 segments × 4 slots = 16 unit-slots) ]
    │ J_RJ45_OUT (chain out, on outer segment-4)
    │   shielded CAT5e/6 patch
    ↓
[ Case-2 Backplane ]
    ↓ ... (max 2 cases per chain per the C_dVdT 100 nF inrush budget)

Inside each case:
[ Backplane segment-1 ] ─ inter-segment 6-pin ─ [ seg-2 ] ─ ─ [ seg-3 ] ─ ─ [ seg-4 ]
       │                                              │             │             │
       ├ slot 1                       slot 5 ─── 8                slot 9 ─── 12      slot 13 ─── 16
       │ (2×3 box header)
       ↓
   [ Unit PCB ]
```

## Master RJ45 ports (×4) — chain into a case

Pinout is **IEEE 802.3at Mode B-style** (passive PoE — not a true PoE PSE).

| Pin | T568B color | Net | Direction | Notes |
|---|---|---|---|---|
| 1 | white-orange | NC reserved | — | UID discovery does not need a wake pin |
| 2 | orange | NC reserved | — | |
| 3 | white-green | RS-485 A | bidir (master is one end) | True twisted pair with pin 6 in T568B |
| 4 | blue | +48V | master → case | Paralleled with pin 5 |
| 5 | white-blue | +48V | master → case | Paralleled with pin 4 |
| 6 | green | RS-485 B | bidir | Pair with pin 3 |
| 7 | white-brown | GND | master ↔ case | Paralleled with pin 8 |
| 8 | brown | GND | master ↔ case | Paralleled with pin 7 |

**Cable**: shielded CAT5e or CAT6 (S/FTP or F/UTP), straight-through, T568B only. **Not** T568A; do **not** use crossover. Max 32 m total per chain, 3 m per hop.

**Shield bond**: master solid-bonds RJ45 shield to GND. Backplane shields float (DNP). Unit has no RJ45 — N/A.

## Backplane case-level RJ45 (chain in / chain out)

Same Mode B pinout as master. Pin-1 indicator silkscreen `1` next to pin 1 of every RJ45 (consistent across all boards).

The case-level RJ45 pair are **passthrough** in the bus sense: A/B + V48 + GND signals from one case-IN connect directly to the same nets on the case-OUT (after passing through the segment-1 → segment-2 → segment-3 → segment-4 internal trace path with per-slot polyfuse drops on V48). One unit's stuck stepper trips its slot's polyfuse but does not interrupt the bus to the next case.

## Backplane → Unit (per-slot board-to-board)

**Connector**: 2×3 box header, 2.54 mm pitch. Female socket on backplane, male pin header on unit. Polarised/keyed shroud on backplane.

| Pin | Net | Direction | Notes |
|---|---|---|---|
| 1 | +48V | backplane → unit | Paralleled with pin 3 (after per-slot 0.2 A polyfuse) |
| 2 | RS-485 A | bidir | Differential pair with pin 4 |
| 3 | +48V | backplane → unit | Paralleled with pin 1 |
| 4 | RS-485 B | bidir | Differential pair with pin 2 |
| 5 | GND | bidir | Paralleled with pin 6 |
| 6 | GND | bidir | Paralleled with pin 5 |

**Polarisation**: connector keyed so the unit can only insert in one orientation. Pin 1 marked on both backplane silkscreen and unit silkscreen.

## Backplane inter-segment connector

Same connector class as per-slot (2×3, 2.54 mm). Same pinout. 3 inter-segment joints per case (between segments 1↔2, 2↔3, 3↔4).

Mating direction TBD at schematic capture (open issue B1):
- Option A: Male on segment-N's downstream end, female on segment-(N+1)'s upstream end. Single SKU, two orientations.
- Option B: Both ends female on every segment, mated by a small 6-pin male jumper plug. Single segment SKU, single jumper SKU.

## Unit → Motor (28BYJ-48)

**Connector**: JST-XH 5-pin vertical, 2.5 mm pitch. Matches 28BYJ-48 factory connector.

| Pin | Wire color | Net | Notes |
|---|---|---|---|
| 1 | blue | Coil 1 | TPL7407L OUT1 |
| 2 | pink | Coil 2 | TPL7407L OUT2 |
| 3 | yellow | Coil 3 | TPL7407L OUT3 |
| 4 | orange | Coil 4 | TPL7407L OUT4 |
| 5 | red | Common (12V) | TPL7407L COM pin (flyback return) |

## Unit → SWD pogo jig

**Pads**: 4× 1.5 mm SMD pads on 2.54 mm pitch, single row, on one edge of the unit PCB. Pogo-jig footprint (Tag-Connect TC2030 compatible).

| Pad | Net | Notes |
|---|---|---|
| 1 | SWDIO | STM32 PA13 (or UART RX in standalone mode) |
| 2 | SWCLK | STM32 PA14 (or UART TX in standalone mode) |
| 3 | NRST | With 100 nF cap to GND + 10 kΩ pull-up to 3V3 |
| 4 | GND | — |

**Standalone test mode**: when the SWD jig is connected without an active SWD master, unit firmware reuses PA13/PA14 as UART RX/TX exposing a text protocol (`home`, `step N`, `angle?`, `id?`). Allows single-unit bench bring-up.

## Master USB-C

ESP32-S3 native USB on IO19 (D−) / IO20 (D+). USB 2.0 Full-speed, CDC + flash + debug. No VBUS power into the board — VBUS goes to a ferrite + bulk + NC pad (board self-powered from 48 V barrel). UFP role (5.1 kΩ on each CC pin to GND).

## Master SWD header

2×5 1.27 mm Cortex-Debug header (ARM 10-pin standard). Pinout per `DIGITAL_DESIGN.md` §3d.

## Master barrel jack

5.5 / 2.5 mm DC barrel, **center-positive**, 60 V rated. 48 V DC input. Recommended PSU: 48 V / 5 A medical-grade or industrial brick. Polarity silkscreen `⊕—●—⊖` next to the jack.

## Cable shielding bond — system-level rule

**Shield is bonded at the master only.** Every cable shield in the chain ties to GND at the master's RJ45 shield can. All downstream RJ45 shield cans (on case backplanes) **float** (shield-bond R + C are DNP on backplane).

This is a single-bond-point topology — eliminates ground-loop hunting that would otherwise span 32 parallel RC paths if every node bonded its shield.

## RS-485 termination — system-level rule

**Termination is at both chain ends**:
- **Master end**: 120 Ω across A/B at master, populated permanently.
- **Chain-end**: 120 Ω on segment-4 of the chain-end case backplane, **populated only as PCBA stuffing variant** for that case. All other case backplanes leave R_TERM as DNP.

Failsafe bias is at the master only (1 kΩ each leg). Backplane and unit add no bias.

## Out-of-band

These signals do **not** travel between PCBs:
- WAKE / wake-pin / pin-1 enumeration: **does not exist** in v2 (UID-based discovery is firmware-only over RS-485).
- Per-unit fault interrupt back-channel: does not exist; fault status is RS-485-polled.
- I²C / SPI between boards: does not exist; each board has its own internal buses.
