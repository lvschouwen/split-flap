# Split-Flap Display PCB v2 — Handoff Bundle

> **Entry point for the freelance PCB designer / JLC EasyEDA service.**
> Created 2026-04-25 (post-#76 architecture pivot).

This directory contains the complete design brief for the v2 redesign of the split-flap display electronics. It supersedes v1 entirely (v1 lives in `../v1/` — frozen at tag `v-esp8266-final`).

## System overview

The display is a wall-mountable split-flap sign built from physical "cases" of 16 modules each. Each module is a small mechanical assembly (stepper + flap drum + per-module electronics PCB). Multiple cases chain together to form a longer display.

**v2 is a three-PCB system:**

| Board | Quantity | Role |
|---|---|---|
| **Master** | 1 per system | ESP32-S3 controller. WiFi + USB-C service port. 4× RJ45 ports, each driving an RS-485 chain of cases. 48 V DC barrel input. |
| **Backplane** | 4 segments per case (16 slots) | Passive distribution: V48 + RS-485 to 16 unit-slots, with per-slot polyfuse. Two case-level RJ45s (chain in/out) on the outer segments. |
| **Unit** | 1 per split-flap module (16 per case) | STM32G030 + AS5600 closed-loop control + TPL7407L stepper drive. Slots into the backplane via a 2×3 box header. |

**Bus**: RS-485 half-duplex, 500 kbaud 8N1. Wire format: COBS+CRC16-BE per `firmware/lib/common/`. Address discovery: master-driven UID-based binary-tree search (no DIP switches, no wake pins).

**Power**: 48 V DC at the master, distributed over CAT6 patch cables to each case backplane (paralleled pairs). Per-bus eFuse on master, per-slot polyfuse on backplane.

**Max system size**: 4 RS-485 chains × 2 cases per chain × 16 units per case = **128 units total**.

## Reading order (recommended for the freelancer)

1. **`README.md`** (this file) — system overview.
2. **`CONNECTOR_PINOUTS.md`** — cross-board contracts (master ↔ backplane ↔ unit). The single page where all inter-board wiring meets.
3. **Per-board `*_DECISIONS.md`** — the locked source-of-truth for each board.
4. **Per-board `*_DESIGN.md` and `*_POWER_DESIGN.md` / `*_DIGITAL_DESIGN.md`** — schematic-level detail.
5. **Per-board `*_MECHANICAL_DESIGN.md` / `*_MECHANICAL.md`** — placement brief, stack-up, zones.
6. **Per-board `*_BOM.csv`** — JLC-native BOM, ready for upload.
7. **Per-board `*_BRINGUP.md`** — first-power sequence + verification.

## File index

### Master PCB (single board)
- `MASTER_DECISIONS.md` — locked decisions (source of truth)
- `POWER_DESIGN.md` — 48 V → V48_RAIL → 3V3, eFuses, INA237 telemetry
- `DIGITAL_DESIGN.md` — ESP32-S3 + MAX14830 + 4× SN65HVD75 + USB-C + SWD
- `MECHANICAL_DESIGN.md` — 130 × 100 mm 4-layer placement brief
- `MASTER_BOM.csv` — JLC-native BOM
- `MASTER_BRINGUP.md` — first-power + per-test-point readings

### Backplane PCB (4 segments × N cases)
- `BACKPLANE_DECISIONS.md`
- `BACKPLANE_DESIGN.md`
- `BACKPLANE_MECHANICAL.md`
- `BACKPLANE_BOM.csv`
- `BACKPLANE_BRINGUP.md`

### Unit PCB (1 per module × 16 per case)
- `UNIT_DECISIONS.md`
- `UNIT_POWER_DESIGN.md`
- `UNIT_DIGITAL_DESIGN.md`
- `UNIT_MECHANICAL_DESIGN.md`
- `UNIT_BOM.csv`
- `UNIT_BRINGUP.md`

### Cross-board / system-level
- `CONNECTOR_PINOUTS.md` — single canonical pinout table across master ↔ backplane ↔ unit
- `CHANGELOG.md` — revision history of the design brief

## What's in scope vs out of scope

**In scope (this directory):**
- Schematic-level electrical design briefs.
- Layer-stack, board-outline, zone-placement, copper-pour rules.
- Connector pinout contracts.
- BOMs in JLC PCBA-ready CSV format.
- Bring-up + verification procedures per board.

**Out of scope:**
- Actual KiCad / EasyEDA schematic capture and layout — handed off to the freelancer / EasyEDA service. Recommend KiCad to enable the **KiBot + kikit production pipeline** (CI-driven JLCPCB output, LCSC alt-part fields per part — adopted from scottbez1's mature workflow).
- Firmware (lives in `../../firmware/`).
- Mechanical / 3D-printed brackets (handed off to a separate mech freelancer; PCB designer hands STEP exchanges).
- Master enclosure (designed-to-fit project-box class, e.g. Hammond 1455N family — chosen after PCB is final).
- Final LCSC part-number verification across the full BOM (delegated to a separate ChatGPT BOM-verify pass per issue #75).

## Pre-fab gates (must clear before tape-out)

1. **DSBGA-10 0.4 mm pitch on JLC standard PCBA tier** — confirm with JLC. Master uses TPS259827YFFR in DSBGA-10. Fallback footprint (TPS25981 in WQFN-12) is also placed on the master PCB; choice at BOM-freeze.
2. **LCSC re-verification across all `CHECK` flagged BOM rows** — delegated to ChatGPT BOM pass (#75).
3. **Stepping current re-measurement** on first prototype unit — 28BYJ-48 at 12 V may pull closer to 300 mA peak than the 74 mA hold-current assumption. Validates backplane polyfuse + unit buck inductor sizing.
4. **MAX14830 SPI-mode strap pin handling** — confirm against datasheet at schematic capture (the pre-2026-04-25 doc had I²C-mode wording).

## Audit history

The current design state reflects two external audits:
- **#76 (2026-04-25)**: 7 parallel agents reviewed the pre-pivot v2 bundle and flagged ~80 issues; the major findings (R_SERIES dissipating 950 W, TPS54308 thermal claim being physically impossible, master/unit wake-protocol mismatch, STM32G030 TSSOP-20 pin map referencing unbonded pins) drove the architectural pivot to a backplane and the part-swaps to TPS54360 / STM32G030K6T6 LQFP-32 / SM712.
- **scottbez1 audit (2026-04-25)**: 3 parallel agents compared this design against scottbez1's mature 108-module Chainlink reference. Validated the architectural choices (per-module MCU + AS5600 closed-loop + RS-485 + UID discovery are the right design for hot-swappable product-scale, even though they're more expensive than scottbez1's centralised driver-per-6-modules). Adopted tactical wins: TPL7407L (vs ULN2003), KiBot+kikit pipeline, broadcast-frame protocol, standalone unit test mode via SWD UART repurpose.

See `CHANGELOG.md` for the full revision history.
