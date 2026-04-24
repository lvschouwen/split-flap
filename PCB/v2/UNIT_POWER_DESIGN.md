# Unit PCB v2 — Power subsystem design

Complement to `UNIT_DECISIONS.md` §Power architecture. Schematic-level detail for the designer.

## Block diagram

```
RJ45 pair 4/5/7/8  (+48V, GND)
     │
     │  Input filter: 10 µF / 100 V X7R 1210 + 100 nF
     │  TVS: SMBJ58CA (+48V ↔ GND, near jack)
     │
     └── TPS54308DBV buck  (48V → 12V @ 500 mA max)
             │ 22 µH inductor (Isat ≥ 600 mA)
             │ 22 µF / 25 V X7R 0805 output
             │ EN tied to VIN (always-on) or pulled up via 100 kΩ
             │ internal soft-start ~2 ms
             │
             └─→ 12V rail ─── ULN2003 COM pin (flyback return)
                           └── HT7833 LDO input

12V rail (~250 mA motor peak)
     │
     └── HT7833 LDO (12V → 3.3V @ ≤ 100 mA)
              │ 1 µF / 25 V X7R 0603 in
              │ 1 µF / 10 V X7R 0603 out
              │
              └─→ 3.3V rail
                       ├── STM32G030 VDD / VDDA
                       ├── AS5600 VDD + VDD3V3
                       ├── SN65HVD75 VCC
                       ├── 10 kΩ pull-ups (WAKE)
                       └── LED via 1 kΩ current limit
```

## Buck — TPS54308DBV

- **VIN range**: 4.5 – 60 V. 48 V input well within spec (12 V headroom vs 60 V abs-max allows for master-side transient ringing).
- **VOUT**: 12.0 V set by feedback divider (FB = 0.8 V internal ref):
  - R_top = 140 kΩ, R_bot = 10 kΩ → VOUT = 0.8 × (1 + 140/10) = **12.0 V**. Both 1 % 0603.
- **Switching frequency**: 500 kHz (internal, not pin-adjustable on DBV variant).
- **Inductor**: 22 µH, Isat ≥ 600 mA (50 % headroom over max load). Candidates: Coilcraft XAL4020-223 or Sunlord SWPA4030S220MT.
- **Output cap**: 22 µF / 25 V X7R 0805 (derated to ~11 µF effective at 12 V — within TPS54308 stability window).
- **Input cap**: 10 µF / 100 V X7R 1210, derated to ~4 µF at 48 V — sufficient.
- **Layout**:
  - Switch-node loop ≤ 5 mm end-to-end.
  - Input cap within 2 mm of VIN pin.
  - Output cap between inductor and VOUT sense path.
  - Feedback trace kept away from switch-node.
  - Thermal vias under IC's ground pad (DBV has exposed GND pin — use a copper pour + 4–8 vias to bottom GND plane).

## LDO — HT7833

- **Dropout**: ≤ 0.5 V at 100 mA.
- **Thermal** (worst-case): (12 V − 3.3 V) × 50 mA = **0.435 W**.
  - HT7833 SOT-89 θ_JA ~150 °C/W on a 2-layer board with copper pour → rise ~65 °C.
  - Ambient 40 °C → T_J ~105 °C. Within 125 °C max. Acceptable.
- **Alternative**: AP2127K-3.3 in SOT-23-5 is also a candidate but θ_JA ~220 °C/W pushes T_J to ~120 °C — too marginal. **HT7833 preferred for thermal headroom**.
- **Bypass**: 1 µF input + 1 µF output ceramic (X7R 0603).

## Protection

- **SMBJ58CA** — bidirectional, 58 V standoff, 93 V max clamp, 600 W pulse rating.
  - One SMBJ58CA between +48 V and GND, located ≤ 10 mm from the RJ45 power-pin entry.
  - A single shared TVS at the board midpoint between IN and OUT jacks is acceptable (low-impedance 48V plane between jacks).
- **ESD array on RJ45 data + wake pins** — see `UNIT_DECISIONS.md` §Protection.
  - Preferred: **SP0504BAATG** 4-channel (covers A, B, WAKE_IN, WAKE_OUT in one SOT-23-6). ~€0.20.
  - Fallback: ESD9B5.0 2-ch (A, B only) + ESD9B3.3 for WAKE.
  - Location: ≤ 5 mm from each RJ45 jack's pin entry, **before** trace reaches SN65HVD75 or MCU.

## Grounding

- Single GND net. 2-layer board with solid GND pour on bottom layer.
- Star point near buck output cap.
- AS5600 GND to its bypass cap via short direct trace; **avoid high-current motor return currents flowing under AS5600 IC body** (routes via bottom plane but keep ULN2003's OUT return traces clustered away from AS5600 keepout).
- Chassis-to-signal-GND: RJ45 shield tied to signal GND via 1 MΩ + 10 nF RC snubber (optional; standard practice for unshielded installs). If enclosures are plastic and cables are short, direct bond or no bond — evaluate at bring-up.

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
- **P4** — Buck thermal vias count pending layout review. Target pad temp rise ≤ 50 °C at max load.
- **P5** — 48V→12V efficiency at 250 mA load: TPS54308 datasheet shows ~85 % at this operating point. If the efficiency measured during bring-up is < 80 %, revisit inductor DCR + output cap ESR contributions.
