# PCB v2

**Revision:** 2026-07-09

Hobby-scale redesign of the split-flap display electronics.

Tracked under **epic #183** (phased plan; supersedes #83).

## Status

- **Phase:** UNFROZEN + re-sequenced (2026-07-08, epic #183; Phase 2/3
  vehicle revised 2026-07-09). The **architecture stands** — ESP32-S3
  master, RS-485 multi-drop (THVD1410, #179), STM32G030K8 per unit
  with silicon UID + IDENTIFY, single 12 V rail, **card-edge
  unit↔backplane interface** (#100 decision; g000ze precedent):
  1. **Phase 1 (#58):** port the v1 master firmware to an ESP32-S3
     devkit speaking I2C to the unchanged v1 units. Runs in parallel
     with the mechanical track.
  2. **Phase 2 (mock-up + capture):** #100 insertion-kinematics
     mock-up (3 stations, dimensioned cross-section — needs a 3D
     printer, not hardware) → lock unit outline/edge geometry → KiCad
     capture of the **unit PCB rev A** → order 10× JLC-assembled.
     Consolidated parts order (~2026-07-17): 25× THVD1410DR,
     2–3× 28BYJ-48 12 V, 2–3 card-edge sockets, ST-Link.
  3. **Phase 3 (protocol on rev A):** v2 bus protocol (framing, UID
     enrollment, IDENTIFY, OTA-over-RS-485, staggering) developed
     directly on rev A unit boards in sockets on a scrap backplane —
     no dev boards or breakouts. Bench measurements #101 (on rev A,
     real power path) and #54. Then capture the production backplane.
  4. **Phase 4:** build + validate **one row (16 units)**; further
     rows are procurement, not design.
- **Cancelled with the custom master board** (#103/#178/#181 closed):
  the 15 A on-board power path, Q1 reverse-block, USB bench-power
  diode-OR. Power distribution moves off-board (brick → fused DIN
  terminal blocks → per-row cables). `SCHEMATIC_MASTER.md`,
  `LAYOUT_MASTER.md`, `MASTER_BOM.csv` and `KICAD_HOWTO_MASTER.md`
  are **superseded as build documents** — kept as reference for the
  PHY/bias/ESD sub-circuits, which move onto the carrier.
- **2026-07-09 decision sweep:** #179 resolved (THVD1410 SOIC-8; master
  1k/1k bias and all SM712 arrays deleted), #100 interface decided
  (card-edge, 2×5 keyed 2.54 mm dual-row: 12V/GND/A/B/spare mirrored on
  both faces, GND first-make), 250 kbaud locked, J7 moot, #104 shrunk
  to clip spec + seat/vibration sanity. Still open from the 2026-07-08
  review: #180 (F_unit sizing, gated on #101), #182 items 2–3 (TVS
  unification, X7R note; item 1 was mooted by the master-board
  cancellation). Details in `OPEN_DECISIONS.md`.
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
  STM32G030K8T6, THVD1410, card-edge fingers, UID + IDENTIFY. The
  board that matters (rev A ×10 is the Phase 3 protocol vehicle).
- **Backplane PCB** — passive socket carrier on the DIN rail
  (2× ~300 mm per row, daisy-chained; redesign of the former bus
  strip PCB after the #100 mock-up).
- **Master** — ESP32-S3 devkit (+ minimal carrier if needed: 2× PHY,
  connectors, 12→5 V buck). The carrier must expose a **factory-reset
  button on GPIO 4 to GND** (#193: bootloader erases otadata after a
  5 s hold through reset; internal pull-up, no external parts). GPIO 4
  is committed in the firmware's bootloader config
  (`firmware/v2/Master/platformio.ini`) — don't reassign it.
- **Design/validation target: 1 row × 16 units**, one small brick.
  The architecture (2 RS-485 buses, rows paired per PHY) scales to
  4×16 = 64 by repetition — capacity is a procurement event.
- **Unit↔bus interface: card-edge-on-backplane** (decided 2026-07-09;
  g000ze/Split-Flap-Display precedent). The #100 mock-up now validates
  insertion kinematics, not the interface choice. The pogo-on-strips
  system in BUS_PCB.md / the LAYOUT docs is superseded.
- **Addressing: STM32 96-bit UID** + per-unit IDENTIFY button. No DIP
  switches, no slot wiring on the bus.

Explicitly **not** in scope: custom master PCB with on-board 15 A
power distribution, DIP switches, 48 V distribution, RJ45
power-and-data, MAX14830
SPI-UART bridge, INA237 per-bus telemetry, AS5600 absolute encoder,
128-unit capacity, product-grade compliance framing.

## Files

| File | Purpose |
|---|---|
| `ARCHITECTURE.md` | System block diagram, voltage and bus flow, addressing |
| `MASTER.md` | Master PCB design notes |
| `MASTER_BOM.csv` | Master PCB BOM |
| `UNIT.md` | Unit PCB design notes (pogo-era; interface sections superseded by card-edge) |
| `UNIT_BOM.csv` | Unit PCB BOM |
| `BUS_PCB.md` | DIN-rail bus PCB design (pogo-strip era — superseded; backplane redesign after #100 mock-up) |
| `BUS_PCB_BOM.csv` | Bus PCB BOM (same caveat) |
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
