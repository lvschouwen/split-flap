# Master PCB v2 — Power Subsystem Design

> Derived solely from `MASTER_DECISIONS.md`. Covers barrel input → V48_RAIL → 3V3_RAIL, per-bus eFuses, INA237 high-side shunts, hardwired health LEDs, PROT_FAULT_OR node. Digital side (MCU, MAX14830, SN65HVD75, USB, SWD) is out of scope — signals crossing the boundary are listed in §4.
>
> Last edited: 2026-04-25 (post-review surge chain simplification + per-bus inrush fix; see issue #76).

## 1. Block diagram

```
 DC_JACK (48 V, 5.5/2.5 mm, 60 V)
   │
   ├── SMDJ58CA (bidir TVS, V_BR=64.4 V, V_C=93 V @ 51 A) — primary surge-facing clamp
   │
   ├── LM74700-Q1 ideal-diode controller (1 kΩ on GATE)
   │   + Q1 external N-MOSFET (SQJ148EP, 100 V, R_DS(on)≈9 mΩ)
   │       │ FAULT ───────────────────────────────────┐
   │       ▼                                          │
 V48_RAIL (reverse-blocked, ~47.5 V typ)              │
   │                                                  │
   ├── D_RAIL: SMAJ51A directly on V48_RAIL ──────────┤  (sole secondary clamp;
   │   (V_BR ~57 V tail; protects eFuse & INA237)     │   per TI SLVA936 — the
   │                                                  │   pre-2026-04-25 R_SERIES
   ├──► BULK: 2× 47 µF / 100 V AL-poly + 4.7 µF MLCC  │   + D_SEC chain is deleted)
   │                                                  │
   ├──► LMR36015 buck (FDDA = HSOIC-8 PowerPAD; FB divider for 3V3) ──► 3V3_RAIL (1.5 A cap, ~750 mA peak)
   │       └── C_BOOT 100 nF / 25 V X7R 0402 from CBOOT to SW (mandatory)
   │                                                  │
   ├──► PWR_48V LED (red) via 22 kΩ                   │
   │                                                  │
   ├──► R_SHUNT1 50 mΩ ─► TPS259827 #1 ─► RJ45_1 VBUS │ FAULT1 ─┐
   ├──► R_SHUNT2 50 mΩ ─► TPS259827 #2 ─► RJ45_2 VBUS │ FAULT2 ─┤
   ├──► R_SHUNT3 50 mΩ ─► TPS259827 #3 ─► RJ45_3 VBUS │ FAULT3 ─┤
   └──► R_SHUNT4 50 mΩ ─► TPS259827 #4 ─► RJ45_4 VBUS │ FAULT4 ─┤
          │   │   │   │   (each: C_dVdT = 100 nF for  │         │
          │   │   │   │    32-unit ~800 µF inrush     │         │
          │   │   │   │    against 1.7 A trip)        │         │
          └───┴───┴───┴──► 4× INA237 (I²C, 0x40..0x43)│         │
                          (VBUS divider 10 k / 1 k)   │         │
                                                      │         │
                              PROT_FAULT_OR ◄─── wired-OR ◄─────┘
                                   │
                                   ├── 10 kΩ pull-up to 3V3
                                   ├── BSS138 inverter ──► FAULT LED
                                   └── ESP32 GPIO (IRQ, input only)

 3V3_RAIL ──► PWR_3V3 LED (green) via 680 Ω
```

## 2. Component selection table

| Ref | Function | Part | LCSC | Package | Key params | €/pc | JLC |
|---|---|---|---|---|---|---|---|
| D1 | Input TVS | SMDJ58CA | C145194 | DO-214AB (SMC) | Bidir, V_WM=58 V, V_BR=64.4 V, V_C=93 V @ 51 A, 3 kW | 0.45 | Extended |
| U1 | Ideal-diode ctrl | LM74700-QDBVRQ1 | C509439 | SOT-23-6 | 3.2–65 V, 20 µA I_Q, FAULT open-drain | 1.30 | Extended |
| Q1 | Reverse-block N-FET | SQJ148EP-T1_GE3 | C2836025 | PowerPAK SO-8 | V_DS=100 V, R_DS(on)=9 mΩ @ V_GS=10 V, Q_g=44 nC | 1.40 | check |
| R_GATE_LM | LM74700 GATE series | 1 kΩ 1% 0603 | C22929 | 0603 | TI SLUA975 stability vs Q_g ringing | 0.01 | Basic |
| D_RAIL | V48_RAIL clamp | SMAJ51A | C8678 | SMA | Sole secondary clamp post-LM74700 (P1 closed 2026-04-25) | 0.20 | Basic |
| U2 | 48→3V3 buck | LMR36015AFDDA | C922105 | HSOIC-8 PowerPAD | 4.2–60 V_in, 1.5 A, 2.1 MHz, adjustable FB | 2.10 | Extended |
| C_BOOT | LMR36015 bootstrap | 100 nF / 25 V X7R 0402 | C1525 | 0402 | CBOOT to SW, ≤2 mm placement | 0.01 | Basic |
| L1 | Buck inductor | XAL5030-333MEB (33 µH, 1.7 A_sat, 245 mΩ) | C2834984 | 5.3×5.3×3.0 mm shielded | I_sat ≥ 1.7 A, DCR ≤ 250 mΩ | 0.90 | Extended |
| C_IN48 | V48 bulk (AL-poly) | 47 µF / 100 V Nichicon PCV | C2843372 | 10×10 SMD | 100 V, 105 °C, ESR ~30 mΩ | 0.75 | check |
| C_IN48x | V48 HF decap | GRM32ER72A475K (4.7 µF / 100 V X7R) | C1778 / C2832225 | 1210 | 100 V rating on unfiltered rail | 0.35 | Extended |
| C_OUT3V3 | 3V3 output cap | GRM188R71A225K (2.2 µF / 25 V X7R) ×2 | C1804 | 0603 | Low ESR | 0.03 | Basic |
| U3..U6 | eFuse (×4) | TPS259827YFFR | C2906816 | DSBGA-10 | 4.2–60 V, I_LIM adj, EN/FAULT/PGOOD | 2.50 | Extended |
| U7..U10 | Current monitor (×4) | INA237AIDGSR | C2893010 | VSSOP-10 | ±163.84 mV FSR, 16-bit, ALERT, I²C | 1.80 | Extended |
| R_SH1..4 | Shunt (×4) | CSR2512-50-1% (50 mΩ, 3 W, 2512) | C157950 | 2512 Kelvin | 50 mΩ ±1%, 3 W | 0.25 | Extended |
| LED1 | PWR_48V | 0603 red, V_F≈1.9 V, I_F=2 mA | C2286 | 0603 | — | 0.02 | Basic |
| LED2 | PWR_3V3 | 0603 green, V_F≈2.0 V, I_F=2 mA | C72043 | 0603 | — | 0.02 | Basic |
| LED3 | FAULT | 0603 red, V_F≈1.9 V, I_F=2 mA | C2286 | 0603 | — | 0.02 | Basic |
| R_LED_48V | 22 kΩ / 0805 / ≥0.125 W | 22 kΩ 1% 0805 (250 V WV) | C17673 | 0805 | Drops ~46 V, 96 mW | 0.01 | Basic |
| R_LED_3V3 | 680 Ω 1% 0603 | — | C23243 | 0603 | 2 mA @ 3.3 V | 0.01 | Basic |
| R_PULL_PF | 10 kΩ 0603 (PROT_FAULT_OR) | — | C25804 | 0603 | pull-up to 3V3 | 0.01 | Basic |

All parts that sit on V48_RAIL unfiltered are ≥100 V rated except the LM74700-Q1 (65 V abs-max) — **called out in §7**.

## 3. Sub-circuits

### 3a. Input protection (barrel → V48_RAIL)

> **Architecture simplified 2026-04-25 (issue #76 audit).** The earlier R_SERIES + D_SEC secondary-clamp chain has been **deleted**. Reason: 47 Ω in series with the main rail dissipates 47 × 4.5² ≈ 950 W steady at 4.5 A bus current — an architectural error that no part choice could fix. The replacement design relies on LM74700-Q1's native overvoltage protection per TI app note SLVA936 plus a single SMAJ51A (`D_RAIL`) clamping V48_RAIL post-Q1.

- **Topology**: DC jack → SMDJ58CA (shunt to GND) → drain of Q1 (SQJ148EP) → LM74700-Q1 controller → V48_RAIL → D_RAIL (SMAJ51A to GND). No series element in the main current path.
- **TVS**: SMDJ58CA, V_WM=58 V (above 48 V nominal), V_BR=64.4 V min, V_C=93 V @ 51 A peak. At full 3 kW / 1 ms pulse the input node clamps at ~93 V; LM74700's overvoltage protection pulls Q1's GATE low, isolating V48_RAIL from the surge tail. After Q1 turns off, V48_RAIL is held by D_RAIL (SMAJ51A) at ~57 V worst-case tail current — within LMR36015 / TPS259827 / INA237 ratings.
- **Q1**: SQJ148EP chosen for 100 V V_DS, 9 mΩ R_DS(on). At 2 A bus load (worst case: one shorted bus at I_LIM), I²R = 2² × 0.009 = 36 mW. Negligible self-heat; no pour needed.
- **LM74700-Q1 passives**: C_CAP pin → 100 nF X7R to GND (charge-pump reservoir). **GATE → 1 kΩ series resistor (R_GATE_LM)** to Q1 gate per TI SLUA975 (stability vs Q1's 44 nC Q_g ringing); was 0 Ω pre-2026-04-25. 10 kΩ gate-source bleed. FAULT → 10 kΩ pull-up to 3V3 on PROT_FAULT_OR node (§3f).
- **D_RAIL clamp (only secondary clamp)**: SMAJ51A on V48_RAIL within 10 mm of Q1's source pad. V_BR ≈ 56.7 V min, V_C ≈ 82 V at full surge current, drops back to ~51–57 V at low surge-tail current. Catches the residual surge tail that flows forward through Q1's body during the SMDJ58CA clamp event. 400 W pulse rating handles the worst-case tail energy (≤ 30 J).
- **Layout hints**: SMDJ58CA within 5 mm of barrel jack, ground return direct to jack shell ground (do not dump 51 A surge through the main GND pour). Q1 drain-source loop area minimised; keep SMDJ58CA inside the current path loop, not dangling off a stub. D_RAIL within 10 mm of Q1's source pad.

### 3b. 48→3V3 buck (LMR36015FDDA)

- **Topology**: standard sync buck; no external Schottky needed.
- **f_sw**: 2.1 MHz (R_FSET = open, default).
- **Inductor**: L = V_OUT × (V_IN−V_OUT) / (ΔI × f_sw × V_IN). Target ΔI = 30 % of I_max = 0.45 A. L = 3.3 × (48−3.3) / (0.45 × 2.1e6 × 48) = **3.25 µH min**. Datasheet recommends 33 µH at 60 V_in / 2.1 MHz / 3.3 V_out for low-ripple profile → **XAL5030-333** selected. I_sat 1.7 A ≥ load+ripple.
- **C_IN**: 1× 2.2 µF / 100 V X7R 1210 (MLCC, right at VIN pin) + 1× 47 µF / 100 V AL-poly bulk. MLCC tight loop to GND; bulk within 15 mm.
- **C_OUT**: 2× 22 µF / 25 V X7R 0805 (44 µF effective ~30 µF after DC-bias derating at 3.3 V — meets datasheet ≥22 µF min with headroom).
- **Feedback (P2 closed 2026-04-25)**: LMR36015's `FDDA` suffix is the **package code (HSOIC-8 PowerPAD)**, not a fixed/adjustable indicator. The base part is adjustable via FB pin. Populate FB divider unconditionally: **R_FB_TOP = 100 kΩ, R_FB_BOT = 33.2 kΩ** for 3.3 V (V_FB = 1.0 V typical). Confirm against TI orderable code at fab time.
- **Bootstrap (mandatory)**: **C_BOOT = 100 nF / 25 V X7R 0402** between CBOOT pin and SW pin, placed within 2 mm of the IC. Without this, the high-side gate driver does not switch and the part bricks. Was missing pre-2026-04-25.
- **Enable / soft-start**: EN tied to V48_RAIL via 1 MΩ + 100 kΩ divider (EN rising threshold 1.2 V → board wakes at ~15 V input, avoiding brown-in chatter). SS pin: 10 nF to GND → ~4 ms soft-start, limits inrush from 3V3 cap bank.
- **Stress**: V_IN pin abs-max 65 V — protected by D_RAIL SMAJ51A on V48_RAIL (§3a). Light-load behaviour (FPWM vs auto-PFM) varies at high V_IN/V_OUT ratios per datasheet Figure 8-3; verify ripple at minimum load against the 22 µF C_OUT.
- **Thermal**: P_diss ≈ V_in × I_in × (1−η) ≈ 3.3 × 0.75 × (1/0.85 − 1) ≈ 440 mW at 85 % efficiency. HSOIC PowerPAD: ~40 °C/W with 4 thermal vias into a 10×10 mm pour → ΔT ≈ 18 °C, fine.
- **Layout hints**: switch-node trace < 10 mm, kept short and narrow; input MLCC GND return to PGND pin via shortest path; feedback trace away from switch node; thermal pad to GND pour via ≥4 vias (0.3 mm drill).

### 3c. Per-bus eFuse (×4, TPS259827YFFR)

- **I_LIM set**: datasheet `R_ILIM = K_ILIM / I_LIMIT`. For I_LIM ≈ 1.7 A and K_ILIM ≈ 18400 (A·Ω typ), R_ILIM ≈ **10.8 kΩ → use 11.0 kΩ 1 % 0603**. Confirm K from current datasheet rev — open issue P3.
- **EN**: driven from ESP32 GPIO through 0 Ω series; **10 kΩ pull-down to GND** on the eFuse side ensures the part is OFF when the MCU is in reset. EN is 5 V-tolerant logic; 3.3 V drive is fine.
- **FAULT**: open-drain, tied to PROT_FAULT_OR (§3f).
- **PGOOD**: not used in this revision — left as a test pad per bus (optional board-bring-up aid).
- **dV/dt (soft-start)**: **C_dVdT pin → 100 nF to GND** (was 10 nF pre-2026-04-25). Sized for full 32-unit bus inrush against 1.7 A trip. Aggregate downstream cap = 32 × ~25 µF effective = ~800 µF. Inrush = C × dV/dt = 800 µF × (48 V / 28 ms) ≈ **1.4 A** — below 1.7 A trip with margin. The pre-2026-04-25 10 nF gave ~12.8 A peak inrush, well above trip — the master could not bring up a populated chain.
- **Passives**: 0.1 µF / 100 V X7R 0603 at VIN; 0.1 µF / 100 V at VOUT (per-bus).
- **Per-bus enable sequencer (firmware)**: at boot, master enables each EFUSE_EN_n serially, waits for PGOOD assertion, samples INA237 current after 50 ms; if I_settle > 200 mA, declare INRUSH_NOT_SETTLED and disable that bus (open-fault for service). Pattern adopted from scottbez1 Chainlink Base supervisor.
- **Stress**: V_IN abs-max 60 V (transient 65 V ≤ 1 ms). V48_RAIL is clamped by D_RAIL SMAJ51A (§3a) at ~57 V tail current — within the 65 V transient rating. No additional per-bus protection needed.
- **Thermal**: R_DS(on) ~55 mΩ; at 1.5 A → 0.055 × 1.5² = 124 mW per eFuse. DSBGA-10 thermal-resistance ~80 °C/W → ΔT ≈ 10 °C → comfortable. 2×2 thermal via array under each package anyway (manufacturer app note).
- **Layout hints**: each eFuse's input/output caps within 3 mm. Keep the 4 eFuses in a row so their FAULT lines can share a common return/pull-up trace.

### 3d. INA237 high-side shunt (×4)

- **Topology (per bus)**: V48_RAIL → R_SHUNT (50 mΩ, 2512, Kelvin) → eFuse VIN. INA237 VS = 3V3. INA237 IN+ = V48_RAIL side of shunt; IN− = eFuse-side of shunt. VBUS tap goes **after** the eFuse (at eFuse VOUT) so INA237 VBUS reports the bus-facing voltage rather than the pre-fuse V48.
- **Full-scale**: 50 mΩ × 1.7 A = 85 mV → well inside the ±163.84 mV FSR of INA237. Current LSB options: select shunt_cal for 1 mA LSB (firmware decision).
- **VBUS attenuator (revised 2026-04-25)**: 10:1 RC divider, but **10 kΩ / 1 kΩ + 100 pF shunt** (was 100 kΩ / 10 kΩ pre-2026-04-25; high source impedance destroyed INA237's 16-bit accuracy via input-bias current and temperature drift). 8 mW dissipation per bus at 48 V — acceptable. Firmware scales ×11 back in software.
- **IN+/IN− filter**: 10 Ω series in each sense line + 100 nF differential across the INA237 IN+/IN− pins, per datasheet EMC recommendation. Resistors must be **≥100 V rated** (0603 thick-film, 200 V WV typical — confirm).
- **VS decoupling**: 100 nF 0603 X7R at each INA237.
- **Stress**: INA237 common-mode abs-max 85 V. With D_RAIL SMAJ51A on V48_RAIL (§3a), the rail is clamped to ~57 V during surge tail, well under the 85 V CM rating. The 10 Ω + 100 nF input filters on each IN+/IN− line further attenuate any residual fast edge. No separate protection on this subcircuit beyond what D_RAIL provides.
- **Layout hints**: Kelvin traces from each shunt → INA237 IN+/IN− matched in length (mm-level), routed as a tight pair, away from switcher noise. Shunt pad copper minimized beyond Kelvin taps so IR voltage is measured accurately, not diluted by pour resistance.

### 3e. Hardwired LED bank

- **PWR_48V**: 22 kΩ 0805 from V48_RAIL to red LED anode, LED cathode to GND. I_LED = (48 − 1.9) / 22 000 = **2.1 mA**. Power in R = (46.1)² / 22 000 = **97 mW** → 0805 250 V WV rating required (confirm part). At surge 93 V: (91.1)² / 22 k = 377 mW peak for <1 ms — acceptable pulse energy on 0805 thick-film.
- **PWR_3V3**: 680 Ω 0603 from 3V3 to green LED anode. I_LED = (3.3 − 2.0) / 680 = **1.9 mA**. R dissipation 2.5 mW — trivial.
- **FAULT (revised 2026-04-25, P4 closed)**: PROT_FAULT_OR is active-low (open-drain wired-OR). To off-load the LED current from the OR line, drive the LED via a **single SOT-23 PNP transistor (e.g. MMBT3906) configured as a high-side switch**: `3V3 → MMBT3906 emitter; MMBT3906 collector → 680 Ω → LED anode → LED cathode → GND; MMBT3906 base → 10 kΩ → PROT_FAULT_OR`. Idle: PROT_FAULT_OR pulled HIGH by 10 kΩ → base sees 3V3 → BJT off → LED off. Fault: PROT_FAULT_OR pulled LOW → base current ~0.27 mA flows from emitter through 10 kΩ → BJT saturates → LED lit. Total load on the OR line = 10 kΩ pull-up + 10 kΩ base resistor in parallel = ~5 kΩ → 0.66 mA sink at fault — still within LM74700's 1 mA FAULT I_OL spec. Adds one MMBT3906 (~€0.04) + one 10 kΩ. Cleaner than the earlier BSS138-inverter sketch.
- **Layout**: put all 3 LEDs in a row near the RJ45 stack so they're visible with the enclosure open.

### 3f. PROT_FAULT_OR wired-OR

- **Topology**: 5 open-drain FAULT outputs (LM74700-Q1 + 4× TPS259827) commoned onto one net. Single 10 kΩ pull-up to 3V3.
- **Loads on the net (revised 2026-04-25)**:
  1. ESP32 GPIO input (IRQ, high-Z).
  2. MMBT3906 PNP base via 10 kΩ (drives FAULT LED — §3e). Adds ~0.33 mA at fault.
- **Logic**: IDLE = HIGH (~3.3 V, GPIO reads 1, BJT off, LED off). ANY FAULT = LOW (GPIO reads 0, BJT saturates, LED on, firmware IRQ).
- **Sink budget at fault (revised)**: 3.3 V / 10 kΩ pull-up + 3.3 V / 10 kΩ base resistor = **0.66 mA** total. LM74700 FAULT I_OL = 1 mA → 33 % headroom. TPS259827 FAULT I_OL = 4 mA → 6× headroom. P4 closed.
- **Layout**: keep the OR-net short and away from the switch node; 10 kΩ pull-up placed near MCU pin (termination side) for best noise immunity on the IRQ input. MMBT3906 + base resistor placed near the LED, not near the OR pull-up.

## 4. Signal handoff list (power side → digital side)

| Net | Direction | Electrical | MCU pin needed | Pull-up/down | Owner of pull |
|---|---|---|---|---|---|
| PROT_FAULT_OR | Power → MCU (in) | Open-drain, 3V3 logic, idle HIGH, 10 kΩ pull-up, <5 µs edges | Input, IRQ-capable, any GPIO | 10 kΩ to 3V3 | **Power side** (this doc) |
| EFUSE_EN_1..4 | MCU → Power (out) | Push-pull 3V3, driven HIGH to enable bus | Output, any GPIO | 10 kΩ pull-down to GND (fail-safe OFF) | **Power side** (this doc) |
| TELEM_ALERT_OR | Power → MCU (in) | Open-drain, 3V3 logic, 4× INA237 ALERT wired-OR, 10 kΩ pull-up | Input, IRQ-capable | 10 kΩ to 3V3 | **Power side** (this doc) |
| INA237_SDA / _SCL | Bidir I²C | 400 kHz, 3V3 | I²C master pins | 2.2 kΩ to 3V3 on each line | **Digital side** (not in this doc) |
| FAULT_LED | (hardwired, informational) | — | none | — | Power side |

## 5. Bulk cap + inrush analysis

- **Input bulk**: 2× 47 µF AL-poly (100 V) + 1× 4.7 µF 100 V X7R MLCC in parallel = ~100 µF effective.
- **Hot-plug worst case**: barrel plug inserted into a 48 V live supply with the board cold (caps at 0 V). Inrush peak current through an ideal short: I_peak = V / ESR_eff. ESR_AL-poly ≈ 30 mΩ, ESR_MLCC ≈ 5 mΩ, combined parallel ESR ~4 mΩ. Unlimited I_peak would be 48 / 0.004 = 12 kA — but source impedance of a 48 V brick (2 A, 3 m lead) is ≥200 mΩ typ, so real peak ≈ 48 / 0.2 = **240 A for a few µs** → still enough to arc the connector.
- **Mitigation**: LM74700-Q1's current-limited turn-on (slow GATE charge via internal 100 µA charge pump) gates the inrush. GATE-to-source Miller plateau time with Q_g=44 nC and I_gate_typ=100 µA → slew ≈ 44 nC / 100 µA = **440 µs** → inrush current limited to C × dV/dt = 100 µF × (48 V / 440 µs) = **10.9 A peak**. Acceptable on a 2 A supply with a capable brick; graceful brown-in on a marginal brick.
- **Supply sizing recommendation for users**: 48 V / 5 A brick recommended for a fully populated master (128 units × ~30 mA mech avg = 3.8 A, plus per-bus overhead + master = ~4.5 A steady). Board won't exceed 4× 1.7 A = 6.8 A if all buses hit eFuse limit simultaneously (fault scenario).
- **Output bulk on 3V3**: 44 µF effective; handles the 500 mA WiFi TX pulse with <50 mV sag at LMR36015's 2.1 MHz response bandwidth.

## 6. Thermal table (worst-case, ambient 40 °C, still air)

| Part | P_diss worst-case | θ_JA | ΔT | Mitigation |
|---|---|---|---|---|
| Q1 (SQJ148EP) | 2² × 0.009 = 36 mW typ; 6.8² × 0.009 = 416 mW fault | 40 °C/W | 17 °C | Drain pad copper pour ≥ 100 mm² |
| LMR36015FDDA | ~440 mW @ 750 mA | 40 °C/W PowerPAD | 18 °C | Thermal pad → 4× 0.3 mm vias into GND pour ≥ 100 mm² |
| TPS259827 ×4 | 124 mW @ 1.5 A / ea | 80 °C/W DSBGA | 10 °C | 2×2 thermal-via array under each pkg |
| R_SHUNT ×4 | 50 mΩ × 1.7² = 144 mW @ I_LIM | 2512 handles 1 W | negligible | No mitigation needed; keep pads away from INA237 thermal |
| INA237 ×4 | <5 mW (analog) | 170 °C/W VSSOP | <1 °C | None |
| LM74700-Q1 | <1 mW (control only) | — | — | None |
| SMDJ58CA | 0 W steady; 3 kW / 1 ms pulse | per datasheet | — | GND pour on cathode pad |
| R_LED_48V (22 kΩ) | 97 mW steady | 0805 rated 125 mW | fine | Use 250 V WV 0805; confirm derating at 60 °C |

No part sits above 50 % of its rated θ_JA-limited temperature at full steady-state load. Copper-pour callouts are the only thermal mitigations needed.

## 7. Open issues

- ~~**P1**~~ **Resolved 2026-04-25 (issue #76 review).** Surge-clamp chain **simplified**: R_SERIES + D_SEC chain deleted (R_SERIES dissipated 950 W steady — architectural error). Replaced by reliance on LM74700-Q1 native overvoltage protection per TI SLVA936 + single SMAJ51A (D_RAIL) on V48_RAIL. Net BOM saving: ~€0.60.
- ~~**P2**~~ **Closed 2026-04-25.** `FDDA` is the package code (HSOIC-8 PowerPAD), not fixed/adjustable. Base part is adjustable; FB divider is mandatory. C_BOOT 100 nF added (was missing).
- **P3 — TPS259827YFFR I_LIM constant (K_ILIM).** Datasheet rev 1.3 lists K ≈ 18400; newer revs differ. Confirm against the rev current at fab time and pick R_ILIM to land 1.6–1.8 A window. (Non-blocking; component value determined at fab.)
- ~~**P4**~~ **Closed 2026-04-25.** FAULT LED driven via MMBT3906 PNP high-side switch with 10 kΩ base resistor (§3e/§3f). PROT_FAULT_OR sink at fault = 0.66 mA — within LM74700's 1 mA spec.
- **P5 — Barrel jack surge path return.** If the enclosure is metal, the barrel jack shell must be isolated from chassis or the TVS return path will dump surge through the mechanical shield. Flag to mechanical agent. Default desktop project-box class enclosure assumed (plastic) per MASTER_DECISIONS — non-blocking.
- ~~**P6**~~ **(new) Closed 2026-04-25.** Per-bus C_dVdT bumped from 10 nF → 100 nF for 32-unit bus inrush against 1.7 A trip.

## 8. BOM additions (power subsystem)

> **Note (2026-04-25):** This block is informational — the canonical BOM lives in `MASTER_BOM.csv` in the JLC-native schema. The list below is grouped by subcircuit for review. Changes vs. pre-2026-04-25:
> - **Removed**: `RTH1` (R_SERIES NTC) and `D_SEC` (SMAJ51A on IC V_IN feed) — surge chain simplified per #76 audit.
> - **Added**: `C_BOOT` (LMR36015 bootstrap), `R_GATE_LM` (LM74700 GATE series), `Q_FAULT_PNP` (MMBT3906 FAULT LED driver), `R_FAULT_BASE` (BJT base R), bumped `C_DVDT` 10 nF → 100 nF, changed `R_VBUS_T` 100 kΩ → 10 kΩ and `R_VBUS_B` 10 kΩ → 1 kΩ.

```
Ref,Part,LCSC,Package,Qty,Note
J1,Barrel jack 5.5/2.5 mm 60 V THT,check,THT,1,Check stock + footprint at BOM-finalize
D1,SMDJ58CA,C145194,DO-214AB,1,Bidir TVS 58V (primary surge clamp)
D_RAIL,SMAJ51A,C8678,SMA,1,Sole secondary clamp on V48_RAIL post-Q1 (§3a)
U1,LM74700-QDBVRQ1,C509439,SOT-23-6,1,Ideal-diode controller
Q1,SQJ148EP-T1_GE3,C2836025,PowerPAK SO-8,1,100V 9mΩ N-FET
R_GATE_LM,1 kΩ 1%,C22929,0603,1,LM74700 GATE series for Q_g stability (TI SLUA975)
U2,LMR36015AFDDA,C922105,HSOIC-8 PowerPAD,1,48→3V3 buck (adjustable; FB divider populated)
C_BOOT,100 nF 25V X7R,C1525,0402,1,LMR36015 CBOOT-to-SW (mandatory)
L1,XAL5030-333MEB,C2834984,5.3×5.3×3.0,1,33 µH 1.7 A shielded
C_IN48_BULK,Nichicon PCV 47µF/100V,C2843372,SMD 10×10,2,AL-poly input bulk
C_IN48_HF,GRM32ER72A475K,C2832225,1210,1,4.7 µF / 100 V X7R
C_IN_BUCK,GRM32ER72A225K,C1778,1210,1,2.2 µF / 100 V X7R at buck VIN
C_OUT_BUCK,GRM21BR71E225K,C1804,0805,2,22 µF / 25 V X7R
C_SS,100 nF X7R,C14663,0603,1,Buck SS pin
C_CP74700,100 nF X7R,C14663,0603,1,LM74700 CAP pin
R_EN_TOP,1 MΩ 1%,C22935,0603,1,LMR36015 EN divider top
R_EN_BOT,100 kΩ 1%,C25803,0603,1,LMR36015 EN divider bottom
R_FB_TOP,100 kΩ 1%,C25803,0603,1,LMR36015 FB divider top (3V3)
R_FB_BOT,33.2 kΩ 1%,check,0603,1,LMR36015 FB divider bottom (3V3)
R_GATE_BLEED,10 kΩ 1%,C25804,0603,1,Q1 G–S bleed
R_SH1..4,50 mΩ 1% 3 W 2512 Kelvin,C157950,2512,4,Per-bus shunt
U3..U6,TPS259827YFFR,C2906816,DSBGA-10,4,Per-bus eFuse (primary; alternate WQFN-12 footprint also placed)
R_ILIM1..4,11 kΩ 1%,C23186,0603,4,eFuse I_LIM
C_DVDT1..4,100 nF X7R,C14663,0603,4,eFuse dV/dt — sized for 32-unit bus inrush (was 10 nF)
C_IN_EF1..4,100 nF / 100 V X7R,C123498,0603,4,eFuse VIN decap
C_OUT_EF1..4,100 nF / 100 V X7R,C123498,0603,4,eFuse VOUT decap
R_EN_PD1..4,10 kΩ 1%,C25804,0603,4,eFuse EN pull-down (fail-safe OFF)
C_EN_RC1..4,100 nF X7R,C14663,0603,4,eFuse EN RC filter (τ ≈ 1 ms)
U_POR,TPS3839L33DBVR,C70604,SOT-23-5,1,3V3 POR supervisor — open-drain /RESET
R_POR_PU,10 kΩ 1%,C25804,0603,1,/RESET pull-up
D_POR1..4,BAT54 small-signal Schottky,C2480,SOT-23,4,Anode at /RESET, cathode at EFUSE_EN_n (polarity corrected 2026-04-25) — pulls EN low when 3V3 unhealthy
U7..U10,INA237AIDGSR,check,VSSOP-10,4,Current monitor — LCSC pending ChatGPT pass (#75); choose between C2893010 / C2897185
R_INx_A1..4,10 Ω / 100 V 1%,C25076,0603,4,INA237 IN+ series filter
R_INx_B1..4,10 Ω / 100 V 1%,C25076,0603,4,INA237 IN− series filter
C_INx_FLT1..4,100 nF X7R,C14663,0603,4,INA237 differential filter
R_VBUS_T1..4,10 kΩ 1%,C25804,0603,4,VBUS divider top (revised 2026-04-25 from 100 kΩ)
R_VBUS_B1..4,1 kΩ 1%,C21190,0603,4,VBUS divider bottom (revised 2026-04-25 from 10 kΩ)
C_VBUS1..4,100 pF C0G,C1546,0603,4,VBUS divider shunt
C_VS1..4,100 nF X7R,C14663,0603,4,INA237 VS decap
R_LED_48V,22 kΩ 1% 0805 250 V,C17673,0805,1,PWR_48V LED resistor
R_LED_3V3,680 Ω 1%,C23243,0603,1,PWR_3V3 LED resistor
R_LED_FAULT,680 Ω 1%,C23243,0603,1,FAULT LED collector resistor
LED_48V,LED red 0603,C2286,0603,1,PWR_48V indicator
LED_3V3,LED green 0603,C72043,0603,1,PWR_3V3 indicator
LED_FAULT,LED red 0603,C2286,0603,1,FAULT indicator
Q_FAULT_PNP,MMBT3906 PNP,C8616,SOT-23,1,FAULT LED high-side switch — gates LED off the OR line (P4 closed)
R_FAULT_BASE,10 kΩ 1%,C25804,0603,1,MMBT3906 base resistor on PROT_FAULT_OR
R_PF_PULL,10 kΩ 1%,C25804,0603,1,PROT_FAULT_OR pull-up
R_AL_PULL,10 kΩ 1%,C25804,0603,1,TELEM_ALERT_OR pull-up
```
