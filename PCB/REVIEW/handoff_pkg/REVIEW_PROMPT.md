# PCB v2 — final review prompt (Claude + ChatGPT)

**Paste this prompt + attach all .md and .csv files in this directory.**
Same package goes to both AIs. We want cross-validation, so do not tell
either reviewer what the other said.

---

## Prompt to paste

You are reviewing the final hardware design for a 64-unit (4 rows × 16
units/row) split-flap display before it goes to a freelancer for PCB
layout in EasyEDA Pro. The design is on **2026-04-26** considered locked
except for PCB layout itself. Your job is to find anything still wrong
before we hand off — but **also to validate that what is locked should
stay locked**.

### Scope of this review

In scope (lock these or surface a blocker that prevents locking):
- System architecture (1 master + 8 bus PCBs daisy-chained 2 per row + 64 units)
- Every component MPN in the BOMs (`MASTER_BOM.csv`, `UNIT_BOM.csv`,
  `BUS_PCB_BOM.csv` — one CSV per board so they parse cleanly)
- Every footprint (package, drill, pad)
- Every connector pinout (master row outputs, bus end connectors, unit
  pogo pin contacts, unit J2 stepper, unit J3 hall, debug headers)
- Every electrical-design choice in the schematic markdown files
  (decoupling values, Zener clamps, TVS placement, fuse ratings,
  strap-pin pulls, NRST filtering, polyfuse package)

Out of scope (do not review — already accepted):
- PCB layout (placement, routing, copper pours, stack-up impedance
  control) — that's the freelancer's job
- Mechanical aesthetics (the v1 chassis is fixed and the unit board
  outline is constrained to 80×40 mm)

### What to read

Architecture + design intent:
- `ARCHITECTURE.md` — system view, power budget, DIN rail layout
- `OPEN_DECISIONS.md` — what we already decided and why; flag if you
  think any decision was wrong
- `EASYEDA_HANDOFF.md` — written for the freelancer; flag anything
  ambiguous or missing
- `MASTER.md`, `UNIT.md`, `BUS_PCB.md` — per-board high-level spec
- `SCHEMATIC_MASTER.md`, `SCHEMATIC_UNIT.md`, `SCHEMATIC_BUS.md` —
  detailed component lists, net assignments, ASCII schematic blocks
- `MASTER_BOM.csv` (×1 per system), `UNIT_BOM.csv` (×64), `BUS_PCB_BOM.csv` (×8)

### How to report findings

Reply with a single bulleted list, each item:

```
- [SEVERITY] file.md:section — issue and the specific fix you recommend
```

Severity tiers:

- **BLOCKER** — design will not work / will damage parts / will fail at
  manufacturing. Cannot ship without fix.
- **SHOULD-FIX** — design will work but is suboptimal, fragile, or
  expensive without fix.
- **NIT** — wording, formatting, doc-internal consistency. Optional.

For each BLOCKER/SHOULD-FIX, include the **specific fix** (e.g., "change
qty from 4 to 8" or "swap MPN to ABC123") so we can apply mechanically.

### Decisions we want validated (please weigh in even if no issue)

1. **K7803-1000R3 switching buck on master vs. linear LDO** — chosen
   because 12 V → 3.3 V at ~200 mA in a SOT-89 LDO would dissipate
   ~1.7 W (rated ~0.5 W). Upgraded from 500 mA → 1 A version after
   external review (peak 470 mA leaves zero headroom on 500 mA part).
   Is the K7803-1000R3 (or R-78E3.3-1.0 / V7803-1000) the right
   module? Anything we missed about input/output cap sizing or layout?
2. **LDL1117S33 (SOT-223) on unit instead of HT7833** — HT7833 is
   6.5 V max VIN and was destroying parts at 12 V. LDL1117S33 is 40 V
   max VIN, 1.2 A. Dissipation at 55 mA = 0.48 W in SOT-223 with
   ~1 cm² copper pour. Sound choice?
3. **AO3401A unit Q1 + 10 V Zener clamp (Z1 BZT52C10)** — AO3401A
   VGS rated only ±12 V; 10 V Zener (not 12 V) keeps the FET safely
   inside abs-max under Zener tolerance. Plus 100 Ω gate series +
   10 kΩ gate pull-down. Is the Zener placement correct (cathode to
   source/+12V, anode to gate)?
4. **Same Q1 + Z1 topology mirrored on master with AOD409** — AOD409
   is ±20 V VGS so the clamp is not strictly required, but we mirror
   the unit fix for symmetry + brick over-voltage absorbance. OK or
   over-engineering?
5. **Per-row polyfuses 4 A hold in 2920 SMD** — we explicitly rejected
   1812 because 1812 family does not support 4 A hold. Bourns
   MF-LSMF400/16X-2 nominated. Right call?
6. **JST-VH 4-pin row connectors (5+ A per pin) instead of JST-XH
   (3 A)** — JST-XH was undersized for the 4 A row fuse. Locked.
   Anything we should know about JST-VH at this rating?
7. **Unit J2 = 5-pin JST-XH (4 motor coils + +12 V on pin 5)** —
   chosen so the 28BYJ-48 red wire doesn't need 64 hand-splices into
   the chassis harness. Does this match the 28BYJ-48 standard plug
   pinout? Any current-rating concern on pin 5?
8. **Polarization = asymmetric 3D-printed DIN clip (MakerWorld STL),
   no PG_KEY pogo** — the 5th polarization pogo (PG_KEY) was withdrawn
   because it collided with PG1 and a spring on bare FR-4 isn't a hard
   mechanical interlock. Is the DIN clip approach robust enough as the
   sole polarization mechanism?
9. **STM32G030K6T6 unit MCU on USART1 (PA9/PA10/PA12) with /RE tied
   low** — chosen for hardware DE auto-toggle on USART1 + 96-bit
   silicon UID at UID_BASE. /RE tied to GND saves a GPIO. OK?
10. **ESP32-S3 strap pin handling** — IO0 with 10 kΩ pull-up + BOOT
    button to GND, IO3 with 10 kΩ pull-up (USB-JTAG enabled), IO46
    with explicit 10 kΩ pull-down (internal pull-down unreliable),
    EN with 1 nF cap to GND + RST button. Anything missing?

### Don't waste tokens on

- Restating things we already said (echoing back our spec)
- Suggesting we add more comments / docstrings to the spec docs
- Layout-specific advice (out of scope)
- Software/firmware (separate review later)

### What "good" looks like

A response with **0 BLOCKER**, ≤3 SHOULD-FIX, plus validation that the
10 locked decisions above are sound. If you find a BLOCKER, we will
fix it and resubmit; otherwise this design ships.
