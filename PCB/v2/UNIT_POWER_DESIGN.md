# Unit PCB v2 — Power subsystem design

Complement to `UNIT_DECISIONS.md` §Power architecture. Schematic-level detail for the designer. Last edited 2026-04-25 (#76 backplane pivot + part-swap to TPS54360 + TPL7407L + SM712).

## Block diagram

```
Backplane connector pins 1, 3 (paralleled +48V), 5, 6 (paralleled GND)
     │
     │  Input filter: 10 µF / 100 V X7R 1210 + 100 nF
     │  TVS: SMAJ51A (+48V ↔ GND, near connector — matches master rail clamp class)
     │
     └── Q_REV  P-channel MOSFET (AO3401, V_DS ≥ 30V, R_DS(on) ~50 mΩ at V_GS=−10V)
            │   Reverse-polarity block — source on input, drain on load (corrected 2026-04-25);
            │   gate pulled high through 10 kΩ; gate-source Zener (12 V) clamp.
            │   Body diode conducts at first power-up; channel then enhances.
            │
            └── TPS54360DDA buck  (48V → 12V @ 3 A max — sized for 28BYJ-48 stepping)
             │ HSOP-8 PowerPAD (replaces TPS54308 SOT-23-6 which had no thermal pad)
             │ 22 µH inductor (Isat ≥ 1 A — bumped from 600 mA per scottbez1 audit
             │                 stepping-current re-measurement flag)
             │ 22 µF / 50 V X7R 1210 output
             │ EN tied to VIN via divider; internal soft-start ~3 ms
             │ V_ref = 0.8 V; FB divider populated for 12 V (R_top = 140 kΩ, R_bot = 10 kΩ)
             │
             └─→ 12V rail ─── TPL7407L COM pin (flyback return)
                           └── HT7833 LDO input

12V rail (~300 mA motor peak)
     │
     └── HT7833 LDO (12V → 3.3V @ ≤ 100 mA)
              │ 1 µF / 25 V X7R 0603 in
              │ 1 µF / 10 V X7R 0603 out (separate LCSC from input cap — corrected 2026-04-25)
              │
              └─→ 3.3V rail
                       ├── STM32G030 VDD / VDDA
                       ├── AS5600 VDD + VDD3V3 (with RC filter on SDA/SCL)
                       ├── SN65HVD75 VCC
                       └── LED via 1 kΩ current limit
```

## Buck — TPS54360DDA

> **Replacement (2026-04-25)**: was TPS54308DBV. SOT-23-6 has no thermal pad — its θ_JA spec of ≤80 °C/W was physically unachievable (real value ~165 °C/W). #76 audit. TPS54360DDA in HSOP-8 PowerPAD has a true exposed thermal pad and reaches the spec via standard via-stitched copper pour.

- **VIN range**: 4.5 – 60 V. 48 V input well within spec (12 V headroom vs 60 V abs-max allows for master-side transient ringing).
- **VOUT**: 12.0 V set by feedback divider (V_ref = 0.8 V internal):
  - R_top = 140 kΩ, R_bot = 10 kΩ → VOUT = 0.8 × (1 + 140/10) = **12.0 V**. Both 1 % 0603.
- **Switching frequency**: 600 kHz (R_T = 200 kΩ; pin-adjustable on the DDA variant — pick a value that doesn't fall on AS5600 sample-rate sub-harmonics).
- **Inductor**: 22 µH, Isat ≥ **1 A** (bumped from 600 mA per scottbez1 audit; 28BYJ-48 stepping pulls ~300 mA peak per phase, two phases simultaneous = 600 mA worst case). Candidates: Coilcraft XAL4030-223 or Sunlord SWPA4030S220MT (verify Isat).
- **Output cap**: 22 µF / 50 V X7R 1210 (derated to ~16 µF effective at 12 V — within TPS54360 stability window).
- **Input cap**: 10 µF / 100 V X7R 1210, derated to ~4 µF at 48 V — sufficient.
- **Bootstrap**: BOOT pin requires 100 nF / 25 V X7R 0402 to SW pin (per datasheet).
- **Layout**:
  - Switch-node loop ≤ 5 mm end-to-end.
  - Input cap within 2 mm of VIN pin.
  - Output cap between inductor and VOUT sense path.
  - Feedback trace kept away from switch-node.
  - Thermal pad → ≥ 6× 0.3 mm thermal vias dropped into ≥ 200 mm² bottom GND pour + ≥ 100 mm² top GND pour (UNIT_MECHANICAL §Thermal). Target θ_JA ≤ 40 °C/W (HSOP-8 PowerPAD with proper pour, vs 80 °C/W spec — comfortable margin).

## LDO — HT7833

- **Dropout**: ≤ 0.5 V at 100 mA.
- **Thermal** (worst-case): (12 V − 3.3 V) × 50 mA = **0.435 W**.
  - HT7833 SOT-89 θ_JA ~150 °C/W on a 2-layer board with copper pour → rise ~65 °C.
  - Ambient 40 °C → T_J ~105 °C. Within 125 °C max. Acceptable.
- **Alternative**: AP2127K-3.3 in SOT-23-5 is also a candidate but θ_JA ~220 °C/W pushes T_J to ~120 °C — too marginal. **HT7833 preferred for thermal headroom**.
- **Bypass**: 1 µF input + 1 µF output ceramic (X7R 0603).

## Protection

- **Reverse-polarity P-MOSFET (per-unit, defense-in-depth)** — backplane is keyed and the case-level RJ45 carries the externally-induced reverse-polarity risk; per-unit P-FET still belt-and-suspenders against a hand-wired backplane fault.
  - **Topology (corrected 2026-04-25)**: in-line **P-channel MOSFET on +48 V** — **source on input side, drain on load side**. Body diode points input → load (conducts at first power-up). Gate pulled high through 10 kΩ; gate-to-source 12 V Zener clamp to keep V_GS within ±20 V abs-max. Correct polarity: source at +48 V, gate at GND via 10 kΩ → V_GS = −48 V (clamped to −12 V) → channel enhances → conducts. Reversed polarity: source at GND, body diode reverse-biased → no current path → unit safe. Pre-2026-04-25 doc had drain/source labels reversed — corrected per #76 audit.
  - **Part (locked 2026-04-25)**: **AO3401** (SOT-23, V_DS = −30 V, R_DS(on) ≈ 50 mΩ at V_GS = −10 V), ~€0.10. SOT-23 acceptable at the unit's ≤ 30 mA steady-state load. SQ4953AEY (SO-8 dual) is no longer an alternate — single-package, single-footprint.
  - **Placement**: between the input TVS and the rest of the unit. TVS upstream so it clamps surges before they reach the MOSFET.
- **SMAJ51A TVS (revised 2026-04-25 from SMBJ58CA)** — bidirectional, 51 V standoff, ~57 V V_BR at low-end tail current, 400 W pulse rating. Matches master's V48_RAIL clamp class; ensures unit-side clamping at ~57 V if a master-side surge propagates onto the cable. SMBJ58CA (V_BR ≈ 64.4 V, V_C ≈ 93 V) was inadequate — would not have fired before the buck's 60 V abs-max during a master-side surge event.
  - One SMAJ51A between +48 V and GND, located ≤ 10 mm from the backplane connector entry, upstream of the reverse-polarity P-MOSFET.
- **ESD array on RS-485 lines (revised 2026-04-25 from SP0504BAATG to SM712-02HTG)** — RS-485-rated, transient-only clamp. Working voltage ±7 V/±12 V — does not fire on legitimate bus traffic (SP0504BAATG clamped at 5 V working, would have fired on normal RS-485 common-mode swings). Same part used on master for inventory consolidation.
  - Location: ≤ 5 mm from the backplane connector A/B pin entry, before trace reaches SN65HVD75.

## Grounding (revised 2026-04-25 with explicit zone separation)

- Single GND net but **two layout zones**: motor-return GND vs signal/sensor GND.
- **Motor-return zone**: TPL7407L pin 8 (GND) + JST-XH pin 5 return current path bonded to the buck output cap GND pad via a single short trace. This is the high-di/dt zone (stepper coil flyback through TPL's clamp diodes).
- **Signal/sensor zone**: STM32, AS5600, SN65HVD75, LDO output return path stitched to the same star point near the buck output cap on a separate path — does NOT share copper with the motor-return zone except at the star point.
- 2-layer board with solid GND pour on bottom layer in BOTH zones; the zoning is enforced by component placement and short-trace return paths on top, not by physical splits in the bottom pour.
- AS5600 GND to its bypass cap via short direct trace; **no motor-return current flows under AS5600 IC body** (TPL7407L OUT return traces clustered away from AS5600 keepout).
- No RJ45 / cable shield to manage on the unit (backplane handles the case-level shield bond at master only — see `BACKPLANE_DESIGN.md`).

## Bus current budget (sanity check)

Per unit at 48 V input, steady-state:
- Motor hold (2 phases energized at 12 V, ~74 mA): 12 × 0.074 / (48 × 0.90) = **20.5 mA**.
- Logic (3.3 V × ~50 mA via 12V buck + LDO): ≈ 12 × 0.050 / (48 × 0.90) ≈ **13.9 mA** (LDO's extra heat means the 48V-input-side draw is actually the full 12V-side current, so add 50 mA × (12/48) = 12.5 mA instead. Use 12.5 mA as the buck-side draw).
- **Total: ~25 mA per unit, 48 V input, steady-state.**

32 units × 25 mA = 800 mA per bus → 47 % of 1.7 A eFuse trip. **53 % headroom** for transient motor pulls (stepping current briefly ~2× hold current).

Master's 4 buses × 0.8 A = 3.2 A at 48 V = 154 W at full load before 3.3 V/12 V conversion overhead — within the master's 48 V input supply budget (see `MASTER_DECISIONS.md`).

## Open issues (non-blocking)

- **P1** — Buck inductor value verified against TPS54308 datasheet equation for 500 kHz switching at 12 V output. 22 µH is conservative; 15 µH datasheet-compliant as well. Lock at schematic capture.
- **P2** — ESD array exact P/N locked at BOM freeze based on JLC basic-library stock.
- **P3** — LDO escalation path: if a future unit revision grows logic current beyond 100 mA (e.g. adds an RGB LED array or Wi-Fi module), replace HT7833 with a second TPS54308 configured for 3.3 V. No PCB change other than the IC + one resistor.
- ~~**P4**~~ **Resolved 2026-04-25.** Buck thermal spec locked in `UNIT_MECHANICAL_DESIGN.md` §Thermal considerations: ≥ 6 thermal vias, ≥ 200 mm² bottom pour, ≥ 100 mm² top pour, θ_JA ≤ 80 °C/W. Fallback to TPS54360 if layout cannot hit spec.
- **P5** — 48V→12V efficiency at 250 mA load: TPS54308 datasheet shows ~85 % at this operating point. If the efficiency measured during bring-up is < 80 %, revisit inductor DCR + output cap ESR contributions.
