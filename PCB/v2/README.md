# PCB v2

**Revision:** 2026-07-08

Hobby-scale redesign of the split-flap display electronics.

Tracked under **epic #183** (phased plan; supersedes #83).

## Status

- **Phase:** UNFROZEN + re-sequenced (2026-07-08, epic #183). The
  **architecture stands** — ESP32-S3 master, RS-485 multi-drop,
  STM32G030K8 per unit with silicon UID + IDENTIFY, single 12 V rail —
  but execution is now firmware-first and devkit-first:
  1. **Phase 1 (#58):** port the v1 master firmware to an ESP32-S3
     devkit speaking I2C to the unchanged v1 units.
  2. **Phase 2:** v2 bus protocol (framing, UID enrollment, IDENTIFY,
     OTA-over-RS-485, staggering) on RS-485 breakouts + G030 dev
     boards. Bench measurements #101/#54. Transceiver pick #179.
  3. **Phase 3:** capture the **unit PCB** — the only planned custom
     board — gated on the #100 mock-up (pogo vs card-edge interface
     comparison). Master = devkit on a simple carrier, only if needed.
  4. **Phase 4:** build + validate **one row (16 units)**; further
     rows are procurement, not design.
- **Cancelled with the custom master board** (#103/#178/#181 closed):
  the 15 A on-board power path, Q1 reverse-block, USB bench-power
  diode-OR. Power distribution moves off-board (brick → fused DIN
  terminal blocks → per-row cables). `SCHEMATIC_MASTER.md`,
  `LAYOUT_MASTER.md`, `MASTER_BOM.csv` and `KICAD_HOWTO_MASTER.md`
  are **superseded as build documents** — kept as reference for the
  PHY/bias/ESD sub-circuits, which move onto the carrier.
- **Open electronics amendments** from the 2026-07-08 core review:
  #179 (transceiver grade + failsafe bias), #180 (F_unit sizing,
  gated on #101), #182 (minors). Apply to the unit docs at Phase 3
  capture.
- **2026-07-04 review pass (#99–#105, umbrella closed):** 2-bus
  architecture (rows paired on native UART1/UART2, SC16IS740
  deleted), unit MCU → STM32G030K8T6, LM2937 LDO + SMAJ13A TVS,
  mandatory power-off insertion. Unit/rail geometry impossibility →
  #100 (now the Phase 3 gate).
- **Decision record:** `OPEN_DECISIONS.md` and `KICAD_HANDOFF.md`
  § "Locked decisions" (read through the #183 lens — master-board
  and 64-unit-scope items are superseded).

## Scope (per epic #183)

- **Unit PCB** — identical across units, 28BYJ-48 12 V + TPL7407L,
  STM32G030K8T6, RS-485, UID + IDENTIFY. The one custom board.
- **Master** — ESP32-S3 devkit (+ minimal carrier if needed: 2× PHY,
  bias, ESD, connectors, 12→5 V buck).
- **Design/validation target: 1 row × 16 units**, one small brick.
  The architecture (2 RS-485 buses, rows paired per PHY) scales to
  4×16 = 64 by repetition — capacity is a procurement event.
- **Unit↔bus interface:** decided by the #100 mock-up — pogo-on-strips
  (as documented here) vs card-edge-on-backplane (proposed; see
  g000ze/Split-Flap-Display precedent).
- **Addressing: STM32 96-bit UID** + per-unit IDENTIFY button. No DIP
  switches, no slot wiring on the bus.

Explicitly **not** in scope: custom master PCB with on-board 15 A
power distribution, rigid backplanes (unless #100 picks card-edge),
DIP switches, 48 V distribution, RJ45 power-and-data, MAX14830
SPI-UART bridge, INA237 per-bus telemetry, AS5600 absolute encoder,
128-unit capacity, product-grade compliance framing.

## Files

| File | Purpose |
|---|---|
| `ARCHITECTURE.md` | System block diagram, voltage and bus flow, addressing |
| `MASTER.md` | Master PCB design notes |
| `MASTER_BOM.csv` | Master PCB BOM |
| `UNIT.md` | Unit PCB design notes (with pogo pins, DIN rail clip) |
| `UNIT_BOM.csv` | Unit PCB BOM |
| `BUS_PCB.md` | DIN-rail bus PCB design (2× 300 mm per row, daisy-chained) |
| `BUS_PCB_BOM.csv` | Bus PCB BOM |
| `KICAD_HANDOFF.md` | KiCad 10 workflow + per-PCB hand-off package overview |
| `SCHEMATIC_MASTER.md` | Master PCB schematic + connection spec |
| `SCHEMATIC_UNIT.md` | Unit PCB schematic + connection spec |
| `SCHEMATIC_BUS.md` | Bus PCB schematic + custom artwork spec |
| `LAYOUT_MASTER.md` | Master PCB placement + routing guide |
| `LAYOUT_UNIT.md` | Unit PCB placement + routing guide |
| `LAYOUT_BUS.md` | Bus PCB placement + routing guide |
| `OPEN_DECISIONS.md` | Locked decisions + rationale |
| `KICAD_GETTING_STARTED.md` | **Beginner**: KiCad 10 install + universal setup, library map, ERC/DRC, plot/Gerber export |
| `KICAD_HOWTO_BUS.md` | **Beginner**: click-by-click build of the Bus PCB (start here) |
| `KICAD_HOWTO_UNIT.md` | **Beginner**: build of the Unit PCB (medium hand-holding) |
| `KICAD_HOWTO_MASTER.md` | **Beginner**: build of the Master PCB (least hand-holding; assumes you've done Bus + Unit) |

The RS-485 wire format + opcode set is a firmware concern, not a hardware
one. It is being designed alongside the rewritten unit firmware and is
intentionally not committed in this directory.

The v1 reference (`PCB/v1/`) is the deployed hardware.

## Hand-off model

User runs PCB layout in **KiCad 10.0**. These docs serve as the spec —
schematic capture and routing happen in KiCad using the BOMs +
SCHEMATIC_*.md + LAYOUT_*.md as ground truth. See `KICAD_HANDOFF.md`
for tool-specific workflow.

**Capture does not start until Phase 3 of epic #183** (protocol proven
on the bench, #100 mock-up decided). The KICAD_HOWTO_* docs were
written against the pre-#183 plan; treat them as regeneratable
walkthroughs, not maintained specs — the MASTER howto is superseded
outright, and the BUS/UNIT howtos contain geometry that #100 will
change.

## Building in KiCad (beginner, Phase 3)

1. **`KICAD_GETTING_STARTED.md`** — install + universal setup. Read once.
2. **`KICAD_HOWTO_UNIT.md`** — the board that matters (64 instances).
3. **`KICAD_HOWTO_BUS.md`** — if #100 keeps the passive bus strip.

The howtos reference `SCHEMATIC_*.md`, `LAYOUT_*.md`, and
`KICAD_HANDOFF.md` for the source-of-truth specs — they walk you
through KiCad clicks, the spec docs tell you the correct values.
