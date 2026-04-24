# Master PCB v2 — Power Subsystem Design

> Derived solely from `MASTER_DECISIONS.md` (2026-04-24). Covers barrel input → V48_RAIL → 3V3_RAIL, per-bus eFuses, INA237 high-side shunts, hardwired health LEDs, PROT_FAULT_OR node. Digital side (MCU, MAX14830, SN65HVD75, USB, SWD) is out of scope — signals crossing the boundary are listed in §4.
>
> Last edited: 2026-04-24.

## 1. Block diagram

```
 DC_JACK (48 V, 5.5/2.5 mm, 60 V)
   │
   ├── SMDJ58CA (bidir TVS, V_BR=64.4 V, V_C=93 V @ 51 A)
   │
   ├── LM74700-Q1 + external N-MOSFET (SQJ148EP, 100 V, R_DS(on)≈9 mΩ)
   │       │ FAULT ───────────────────────────────────┐
   │       ▼                                          │
 V48_RAIL (protected, reverse-blocked, ~47.5 V typ)   │
   │                                                  │
   ├──► BULK: 2× 47 µF / 100 V AL-poly + 3× 1 µF / 100 V X7R
   │                                                  │
   ├──► LMR36015FDDA buck ──► 3V3_RAIL (1.5 A cap, ~750 mA peak load)
   │                                                  │
   ├──► PWR_48V LED (red) via 22 kΩ                   │
   │                                                  │
   ├──► R_SHUNT1 50 mΩ ─► TPS259827 #1 ─► RJ45_1 VBUS │ FAULT1 ─┐
   ├──► R_SHUNT2 50 mΩ ─► TPS259827 #2 ─► RJ45_2 VBUS │ FAULT2 ─┤
   ├──► R_SHUNT3 50 mΩ ─► TPS259827 #3 ─► RJ45_3 VBUS │ FAULT3 ─┤
   └──► R_SHUNT4 50 mΩ ─► TPS259827 #4 ─► RJ45_4 VBUS │ FAULT4 ─┤
          │   │   │   │                               │         │
          └───┴───┴───┴──► 4× INA237 (I²C, 0x40..0x43)│         │
                                                      │         │
                              PROT_FAULT_OR ◄─── wired-OR ◄─────┘
                                   │
                                   ├── 10 kΩ pull-up to 3V3
                                   ├── FAULT LED (red, hardwired)
                                   └── ESP32 GPIO (IRQ, input only)

 3V3_RAIL ──► PWR_3V3 LED (green) via 680 Ω
```

## 2. Component selection table

| Ref | Function | Part | LCSC | Package | Key params | €/pc | JLC |
|---|---|---|---|---|---|---|---|
| D1 | Input TVS | SMDJ58CA | C145194 | DO-214AB (SMC) | Bidir, V_WM=58 V, V_BR=64.4 V, V_C=93 V @ 51 A, 3 kW | 0.45 | Extended |
| U1 | Ideal-diode ctrl | LM74700-QDBVRQ1 | C509439 | SOT-23-6 | 3.2–65 V, 20 µA I_Q, FAULT open-drain | 1.30 | Extended |
| Q1 | Reverse-block N-FET | SQJ148EP-T1_GE3 | C2836025 | PowerPAK SO-8 | V_DS=100 V, R_DS(on)=9 mΩ @ V_GS=10 V, Q_g=44 nC | 1.40 | check |
| U2 | 48→3V3 buck | LMR36015FDDA | C922105 | HSOIC-8 PowerPAD | 4.2–60 V_in, 1.5 A, 2.1 MHz, AEC-Q100 | 2.10 | Extended |
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

- **Topology**: DC jack → SMDJ58CA (shunt to GND) → sense resistor (0 Ω placeholder, kept for future inrush R) → drain of Q1 → LM74700-Q1 → V48_RAIL. LM74700 controls Q1 as a reverse-polarity and surge-ride-through block.
- **TVS**: SMDJ58CA, V_WM=58 V (above 48 V nominal), V_BR=64.4 V min, V_C=93 V @ 51 A peak. At full 3 kW / 1 ms pulse, V48_RAIL is clamped to ~93 V — downstream parts must tolerate this.
- **Q1**: SQJ148EP chosen for 100 V V_DS, 9 mΩ R_DS(on). At 2 A bus load (worst case: one shorted bus at I_LIM), I²R = 2² × 0.009 = 36 mW. Negligible self-heat; no pour needed.
- **LM74700-Q1 passives**: C_CAP pin → 100 nF X7R to GND (charge-pump reservoir). GATE → 0 Ω series to Q1 gate, 10 kΩ gate-source bleed. FAULT → 10 kΩ pull-up to 3V3 on PROT_FAULT_OR node (§3f).
- **Stress**: at surge, Q1 V_DS sees V48_clamp − V_load ≈ 93 − (3V3 buck input low) → well under 100 V. LM74700 V_IN abs-max = 65 V; surge clamp at 93 V *exceeds* this — **design mitigation**: add a 47 Ω / 1 W series resistor between TVS anode and LM74700 VIN, plus 33 V Zener (SMBJ33A) clamp at LM74700 VIN-to-GND for surge tail clipping. See open issue P1.
- **Layout hints**: TVS within 5 mm of barrel jack, ground return direct to jack shell ground. Q1 drain-source loop area minimized; keep SMDJ58CA inside the current path loop, not dangling off a stub.

### 3b. 48→3V3 buck (LMR36015FDDA)

- **Topology**: standard sync buck; no external Schottky needed.
- **f_sw**: 2.1 MHz (R_FSET = open, default).
- **Inductor**: L = V_OUT × (V_IN−V_OUT) / (ΔI × f_sw × V_IN). Target ΔI = 30 % of I_max = 0.45 A. L = 3.3 × (48−3.3) / (0.45 × 2.1e6 × 48) = **3.25 µH min**. Datasheet recommends 33 µH at 60 V_in / 2.1 MHz / 3.3 V_out for low-ripple profile → **XAL5030-333** selected. I_sat 1.7 A ≥ load+ripple.
- **C_IN**: 1× 2.2 µF / 100 V X7R 1210 (MLCC, right at VIN pin) + 1× 47 µF / 100 V AL-poly bulk. MLCC tight loop to GND; bulk within 15 mm.
- **C_OUT**: 2× 22 µF / 25 V X7R 0805 (44 µF effective ~30 µF after DC-bias derating at 3.3 V — meets datasheet ≥22 µF min with headroom).
- **Feedback**: LMR36015FDDA (fixed 3.3 V variant of the FDDA family) has **internal FB divider** per -FDDA suffix — if the selected marking is the adjustable version, use R_FB_TOP = 100 kΩ, R_FB_BOT = 33.2 kΩ for 3.3 V (V_FB = 1.0 V typical). Verify against the exact orderable code — see open issue P2.
- **Enable / soft-start**: EN tied to V48_RAIL via 1 MΩ + 100 kΩ divider (EN rising threshold 1.2 V → board wakes at ~15 V input, avoiding brown-in chatter). SS pin: 10 nF to GND → ~4 ms soft-start, limits inrush from 3V3 cap bank.
- **Stress**: V_IN pin abs-max 65 V, rated operating 60 V — at TVS clamp (93 V) this part is exposed; the series R + Zener clamp proposed in §3a protects LMR36015 the same way it protects LM74700.
- **Thermal**: P_diss ≈ V_in × I_in × (1−η) ≈ 3.3 × 0.75 × (1/0.85 − 1) ≈ 440 mW at 85 % efficiency. HSOIC PowerPAD: ~40 °C/W with 4 thermal vias into a 10×10 mm pour → ΔT ≈ 18 °C, fine.
- **Layout hints**: switch-node trace < 10 mm, kept short and narrow; input MLCC GND return to PGND pin via shortest path; feedback trace away from switch node; thermal pad to GND pour via ≥4 vias (0.3 mm drill).

### 3c. Per-bus eFuse (×4, TPS259827YFFR)

- **I_LIM set**: datasheet `R_ILIM = K_ILIM / I_LIMIT`. For I_LIM ≈ 1.7 A and K_ILIM ≈ 18400 (A·Ω typ), R_ILIM ≈ **10.8 kΩ → use 11.0 kΩ 1 % 0603**. Confirm K from current datasheet rev — open issue P3.
- **EN**: driven from ESP32 GPIO through 0 Ω series; **10 kΩ pull-down to GND** on the eFuse side ensures the part is OFF when the MCU is in reset. EN is 5 V-tolerant logic; 3.3 V drive is fine.
- **FAULT**: open-drain, tied to PROT_FAULT_OR (§3f).
- **PGOOD**: not used in this revision — left as a test pad per bus (optional board-bring-up aid).
- **dV/dt (soft-start)**: C_dVdT pin → 10 nF to GND → ≈ 3 ms output ramp; limits inrush into the RJ45 cable capacitance + any hot-plugged unit string.
- **Passives**: 0.1 µF / 100 V X7R 0603 at VIN; 0.1 µF / 100 V at VOUT (per-bus).
- **Stress**: V_IN abs-max 60 V; during TVS surge, output of LM74700 drives this at up to 93 V momentarily — **series R / Zener clamp from §3a covers this path too, since all loads share V48_RAIL after the ideal-diode block**.
- **Thermal**: R_DS(on) ~55 mΩ; at 1.5 A → 0.055 × 1.5² = 124 mW per eFuse. DSBGA-10 thermal-resistance ~80 °C/W → ΔT ≈ 10 °C → comfortable. 2×2 thermal via array under each package anyway (manufacturer app note).
- **Layout hints**: each eFuse's input/output caps within 3 mm. Keep the 4 eFuses in a row so their FAULT lines can share a common return/pull-up trace.

### 3d. INA237 high-side shunt (×4)

- **Topology (per bus)**: V48_RAIL → R_SHUNT (50 mΩ, 2512, Kelvin) → eFuse VIN. INA237 VS = 3V3. INA237 IN+ = V48_RAIL side of shunt; IN− = eFuse-side of shunt. VBUS tap goes **after** the eFuse (at eFuse VOUT) so INA237 VBUS reports the bus-facing voltage rather than the pre-fuse V48.
- **Full-scale**: 50 mΩ × 1.7 A = 85 mV → well inside the ±163.84 mV FSR of INA237. Current LSB options: select shunt_cal for 1 mA LSB (firmware decision).
- **VBUS attenuator**: INA237 VBUS pin abs-max is V_COMMON (85 V rating) — 48 V nominal is fine directly without divider, but the 93 V TVS clamp exceeds VBUS abs-max. **Use a 10:1 RC divider (100 kΩ / 10 kΩ, with 100 pF shunt) from eFuse VOUT to VBUS pin.** Firmware scales ×10 back in software.
- **IN+/IN− filter**: 10 Ω series in each sense line + 100 nF differential across the INA237 IN+/IN− pins, per datasheet EMC recommendation. Resistors must be **≥100 V rated** (0603 thick-film, 200 V WV typical — confirm).
- **VS decoupling**: 100 nF 0603 X7R at each INA237.
- **Stress**: INA237 common-mode abs-max 85 V. At surge clamp 93 V this is **exceeded** — the 10 Ω input filters + 100 nF cap give some pulse attenuation but do not fully protect. **Mitigation**: the series-R + Zener clamp proposed in §3a keeps V48_RAIL below ~65 V during surge tail, bringing INA237 common-mode within rating. This must be verified — open issue P1.
- **Layout hints**: Kelvin traces from each shunt → INA237 IN+/IN− matched in length (mm-level), routed as a tight pair, away from switcher noise. Shunt pad copper minimized beyond Kelvin taps so IR voltage is measured accurately, not diluted by pour resistance.

### 3e. Hardwired LED bank

- **PWR_48V**: 22 kΩ 0805 from V48_RAIL to red LED anode, LED cathode to GND. I_LED = (48 − 1.9) / 22 000 = **2.1 mA**. Power in R = (46.1)² / 22 000 = **97 mW** → 0805 250 V WV rating required (confirm part). At surge 93 V: (91.1)² / 22 k = 377 mW peak for <1 ms — acceptable pulse energy on 0805 thick-film.
- **PWR_3V3**: 680 Ω 0603 from 3V3 to green LED anode. I_LED = (3.3 − 2.0) / 680 = **1.9 mA**. R dissipation 2.5 mW — trivial.
- **FAULT**: anode to 3V3 through 680 Ω, cathode to PROT_FAULT_OR node. LED lit when PROT_FAULT_OR is pulled LOW (fault). I_LED = (3.3 − 1.9) / 680 ≈ 2.0 mA. **Note**: when idle (no fault), the 10 kΩ pull-up holds PROT_FAULT_OR near 3V3, so the LED is reverse-biased/off. Confirmed against eFuse FAULT sink spec (≥4 mA) — one FAULT driver can sink both the LED (2 mA) and the pull-up current (0.33 mA).
- **Layout**: put all 3 LEDs in a row near the RJ45 stack so they're visible with the enclosure open.

### 3f. PROT_FAULT_OR wired-OR

- **Topology**: 5 open-drain FAULT outputs (LM74700-Q1 + 4× TPS259827) commoned onto one net. Single 10 kΩ pull-up to 3V3.
- **Loads on the net**:
  1. FAULT LED (via 680 Ω from 3V3 — §3e).
  2. ESP32 GPIO input (IRQ, high-Z).
- **Logic**: IDLE = HIGH (~3.3 V, LED off, GPIO reads 1). ANY FAULT = LOW (LED on, GPIO reads 0, firmware IRQ).
- **Sink budget at fault**: 3.3 V / 10 k + 2 mA LED ≈ 2.33 mA. Datasheet I_OL for LM74700 FAULT = 1 mA @ 0.4 V; TPS259827 FAULT = 4 mA @ 0.5 V. **The LM74700 can barely sink it**. Mitigation: use **15 kΩ** pull-up (reduces sink by 0.11 mA, still well within LM74700 spec with LED-only load ≈ 2.22 mA — tight). **Better**: move the LED to a small-signal BSS138 level-shifter driven by PROT_FAULT_OR so the OR line only sinks the pull-up. See open issue P4.
- **Layout**: keep the OR-net short and away from the switch node; 10 kΩ placed near MCU pin (termination side) for best noise immunity on the IRQ input.

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

- **P1 — TVS clamp headroom vs LM74700 / LMR36015 / INA237 abs-max.** SMDJ58CA V_C=93 V at 51 A surge exceeds LM74700-Q1 VIN abs-max (65 V), LMR36015 VIN abs-max (65 V), and INA237 VCM abs-max (85 V). Recommended mitigation: 47 Ω / 1 W series resistor between TVS node and each IC's supply feed, plus an SMBJ60A (60 V uni-directional) clamp at the LM74700/LMR36015 VIN node to cap residual surge energy at ~85 V. Cost: ~€0.40. **User confirm** whether to add this secondary clamp or redesign around a lower-clamp TVS (e.g. SMDJ45CA, V_C≈76 V — but V_WM=45 V leaves no margin above 48 V nominal; not viable). Preferred path: secondary clamp.
- **P2 — LMR36015FDDA orderable variant (fixed vs adjustable).** Datasheet codes both fixed-3.3 V and adjustable variants under similar markings. Confirm LCSC C922105 is the adjustable (FDDA = adjustable per TI convention) — if so, the FB divider in §3b is required; if fixed, omit divider.
- **P3 — TPS259827YFFR I_LIM constant (K_ILIM).** Datasheet rev 1.3 lists K ≈ 18400; newer revs differ. Confirm against the rev current at fab time and pick R_ILIM to land 1.6–1.8 A window.
- **P4 — PROT_FAULT_OR sink budget.** LM74700 FAULT is only spec'd for 1 mA sink. If the FAULT LED is on the OR net directly (§3f), sink is ~2.3 mA — marginal. Preferred fix: LED driven via a BSS138 inverter so only the pull-up loads the OR line. Adds ~€0.05 and 1 transistor. User confirm.
- **P5 — Barrel jack surge path return.** If the enclosure is metal, the barrel jack shell must be isolated from chassis or the TVS return path will dump surge through the mechanical shield. Flag to mechanical agent.

## 8. BOM additions (power subsystem)

```
Ref,Part,LCSC,Package,Qty,Note
J1,Barrel jack 5.5/2.5 mm 60 V THT,C134567,THT,1,Check stock
D1,SMDJ58CA,C145194,DO-214AB,1,Bidir TVS 58V
D_SEC,SMBJ60A,C87447,SMB,1,Secondary clamp (see P1)
U1,LM74700-QDBVRQ1,C509439,SOT-23-6,1,Ideal-diode controller
Q1,SQJ148EP-T1_GE3,C2836025,PowerPAK SO-8,1,100V 9mΩ N-FET
U2,LMR36015FDDA,C922105,HSOIC-8 PowerPAD,1,48→3V3 buck (verify variant P2)
L1,XAL5030-333MEB,C2834984,5.3×5.3×3.0,1,33 µH 1.7 A shielded
C_IN48_BULK,Nichicon PCV 47µF/100V,C2843372,SMD 10×10,2,AL-poly input bulk
C_IN48_HF,GRM32ER72A475K,C2832225,1210,1,4.7 µF / 100 V X7R
C_IN_BUCK,GRM32ER72A225K,C1778,1210,1,2.2 µF / 100 V X7R at buck VIN
C_OUT_BUCK,GRM21BR71E225K,C1804,0805,2,22 µF / 25 V X7R
C_SS,100 nF X7R,C14663,0603,1,Buck SS pin
C_CP74700,100 nF X7R,C14663,0603,1,LM74700 CAP pin
R_EN_TOP,1 MΩ 1%,C22935,0603,1,LMR36015 EN divider top
R_EN_BOT,100 kΩ 1%,C25803,0603,1,LMR36015 EN divider bottom
R_GATE_BLEED,10 kΩ 1%,C25804,0603,1,Q1 G–S bleed
R_SH1..4,50 mΩ 1% 3 W 2512 Kelvin,C157950,2512,4,Per-bus shunt
U3..U6,TPS259827YFFR,C2906816,DSBGA-10,4,Per-bus eFuse
R_ILIM1..4,11 kΩ 1%,C23186,0603,4,eFuse I_LIM
C_DVDT1..4,10 nF X7R,C1588,0603,4,eFuse dV/dt
C_IN_EF1..4,100 nF / 100 V X7R,C123498,0603,4,eFuse VIN decap
C_OUT_EF1..4,100 nF / 100 V X7R,C123498,0603,4,eFuse VOUT decap
R_EN_PD1..4,10 kΩ 1%,C25804,0603,4,eFuse EN pull-down (fail-safe OFF)
U7..U10,INA237AIDGSR,C2893010,VSSOP-10,4,Current monitor
R_INx_A1..4,10 Ω / 100 V 1%,C25076,0603,4,INA237 IN+ series filter
R_INx_B1..4,10 Ω / 100 V 1%,C25076,0603,4,INA237 IN− series filter
C_INx_FLT1..4,100 nF X7R,C14663,0603,4,INA237 differential filter
R_VBUS_T1..4,100 kΩ 1%,C25803,0603,4,VBUS divider top (10:1)
R_VBUS_B1..4,10 kΩ 1%,C25804,0603,4,VBUS divider bottom
C_VBUS1..4,100 pF C0G,C1546,0603,4,VBUS divider shunt
C_VS1..4,100 nF X7R,C14663,0603,4,INA237 VS decap
R_LED_48V,22 kΩ 1% 0805 250 V,C17673,0805,1,PWR_48V LED resistor
R_LED_3V3,680 Ω 1%,C23243,0603,1,PWR_3V3 LED resistor
R_LED_FAULT,680 Ω 1%,C23243,0603,1,FAULT LED resistor
LED_48V,LED red 0603,C2286,0603,1,PWR_48V indicator
LED_3V3,LED green 0603,C72043,0603,1,PWR_3V3 indicator
LED_FAULT,LED red 0603,C2286,0603,1,FAULT indicator
R_PF_PULL,10 kΩ 1%,C25804,0603,1,PROT_FAULT_OR pull-up
R_AL_PULL,10 kΩ 1%,C25804,0603,1,TELEM_ALERT_OR pull-up
Q_FAULT_LVL,BSS138,C8545,SOT-23,1,Optional — see P4
```
