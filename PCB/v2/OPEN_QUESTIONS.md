# v2 open questions

Design decisions still pending on the v2 PCB. Each entry states the context, the concrete options, and a recommendation as a starting point (not a commitment). Resolving these unblocks schematic capture, BOM freeze, and layout handoff.

Organized roughly in the order they gate downstream work. Recommendations are the author's; the user decides.

---

## Q1 — S3 ↔ H2 coprocessor link: UART or SPI?

**Context.** The master has 3 UART peripherals (UART0/1/2). With UART0 policy-reserved for boot/recovery, two RS-485 buses already claim UART1 and UART2, leaving no UART for the S3↔H2 link.

**Options.**
- **A. SPI for S3↔H2** (S3 master, H2 slave). Uses 4 pins (SCK/MOSI/MISO/CS). Deterministic framing, SPI-DMA gives good H2 OTA throughput. Cost: firmware port on both sides — current COBS-over-UART framing invalidated.
- **B. External dual-UART I²C bridge** (e.g. SC16IS752) hosts the two RS-485 buses; H2 link stays on UART. Cost: ~$2 BOM, one extra IC, 2 additional I²C slaves, 2 IRQ lines consumed.
- **C. Drop to a single RS-485 bus.** Bus B removed. Cost: halves capacity (128 → 64 units per master).

**Recommendation.** A. Cleanest electrically, no extra silicon, firmware cost is real but contained.

**Blocks.** Final GPIO allocation, S3 and H2 firmware protocol, SCHEMATIC §A.4–A.5.

---

## Q2 — IO1 / IO2: TLC control or reserve for ADC?

**Context.** IO1 and IO2 are ADC1-capable. Assigning them to TLC5947 XLAT/BLANK consumes the cleanest analog-capable pins on the module.

**Options.**
- **A. Use for TLC_XLAT / TLC_BLANK.** Simplest; no future ADC headroom.
- **B. Reserve for future analog** — e.g. monitor master's own 48 V input via divider.
- **C. Add a master-input INA237_M on I²C1.** Covers the monitoring need without consuming GPIOs.

**Recommendation.** C if master-input telemetry is valuable; otherwise A.

**Blocks.** Final GPIO allocation, optional INA237_M in BOM.

---

## Q3 — IO48 disposition

**Context.** Policy: avoid for critical signals. Acceptable uses: non-critical status LED, test pad, unassigned.

**Options.**
- **A. Unassigned** — cleanest default.
- **B. Non-critical heartbeat LED** — visual "S3 alive" indicator during bring-up.

**Recommendation.** B. One LED + resistor; disproportionate bring-up value.

**Blocks.** BOM (one LED), layout.

---

## Q4 — Q_RP reverse-polarity topology

**Context.** Second-pass review: source and drain of Q_RP (AO4407) are swapped in SCHEMATIC §A.1. Body diode points the wrong way and V_GS sits at 0 V in steady state — board as drawn is non-functional.

**Options.**
- **A. Fix the discrete topology** — swap S/D, pull gate to GND via 100 kΩ (not to source), keep Zener S-G clamp.
- **B. Replace with an ideal-diode controller + external FET** — e.g. LM74700-Q1, LM5050-1. Monotonic turn-on, fault pin exposed, no Zener dependency.

**Recommendation.** B. Clean redesign is the right moment to adopt the ideal-diode IC; easier to review, harder to get wrong at layout.

**Blocks.** SCHEMATIC §A.1, BOM.

---

## Q5 — TLC5947 supply rail (3V3 or 5V)

**Context.** At VCC = 5 V, TLC5947 V_IH = 0.7 × 5 = 3.5 V. ESP32-S3 drives 3.3 V typ — non-compliant.

**Options.**
- **A. Run TLC5947 on 3V3.** Datasheet allows 3.0–5.5 V VCC. No level-shifter needed. Verify LED Vf headroom (green Vf ≈ 2 V leaves 1.3 V for the constant-current sink — works).
- **B. Keep 5 V and add a level translator** (74LVC4T1774 or 74LVC2T45 × 2) on SCK/SIN/XLAT/BLANK.

**Recommendation.** A. Couples with Q6.

**Blocks.** SCHEMATIC §A.10, BOM.

---

## Q6 — Master power architecture: cascaded or parallel?

**Context.** Current: 48V → 5V → 3V3 (cascaded). Proposed: two parallel bucks from 48V for 5V_LED and 3V3_LOGIC, decoupling SK6812/TLC current transients from logic.

**Options.**
- **A. Parallel 48→5V + 48→3V3.** Decouples LED load. Slight efficiency gain. Cost: one extra buck IC + passives.
- **B. Keep cascaded.** Lower BOM; LED transients couple into 3V3 via shared 5V.

**Recommendation.** A, contingent on Q5. If TLC moves to 3V3, the 5V rail carries only SK6812 and can be sized small (~1 A buck).

**Blocks.** SCHEMATIC §A.2–A.3, BOM, layout.

---

## Q7 — Per-bus overcurrent isolation on master

**Context.** Current: single F_VIN polyfuse on master input. A Bus A short takes down Bus B via the shared fuse. No firmware-controlled cutoff.

**Options.**
- **A. Per-bus eFuse** (TPS25947A, TPS259631 class). Firmware cutoff via INA237 ALERT. Hard cutoff within µs.
- **B. Per-bus polyfuse** (1812 at ~1.8 A hold). Passive, cheap, slow.
- **C. Per-bus NMOS load switch + INA237-driven hard disconnect.**

**Recommendation.** A. 48 V / 2 A distributed system warrants active protection; INA237 ALERT infrastructure is already there.

**Blocks.** SCHEMATIC §A.6–A.9, BOM.

---

## Q8 — Unit hot-plug inrush limiter

**Context.** Hot-plugging a unit pulls tens of amps through RJ45 contacts for microseconds to charge ~13 µF of local-tap capacitance. RJ45 is not rated for hot-mate at 48 V / high inrush.

**Options.**
- **A. Hot-swap controller** (LTC4364, LTC4231) — active soft-start + protection.
- **B. Discrete NMOS with slow gate RC** (~200–500 µs ramp) — cheap, works, no telemetry.
- **C. NTC thermistor** — simplest, adds 0.5–1 W steady-state dissipation.

**Recommendation.** B for first pass (unit BOM is cost-sensitive at 128× quantity); A if field hot-plug is expected to be routine.

**Blocks.** Unit SCHEMATIC §B.2, BOM.

---

## Q9 — RS-485 fail-safe bias value

**Context.** With master 680 Ω / 680 Ω and both ends terminated (60 Ω parallel across A–B), idle differential is 139 mV — below the TIA-485-A 200 mV margin recommendation.

**Options.**
- **A. 390 Ω / 390 Ω** → 235 mV idle.
- **B. 330 Ω / 330 Ω** → 272 mV idle.

**Recommendation.** A. Adequate margin at ~4 mA extra quiescent per bus.

**Blocks.** SCHEMATIC §A.6–A.7, BOM.

---

## Q10 — Cable shield grounding strategy

**Context.** Current: every node (master + all units) bonds shield to GND via 10 nF. Shield is DC-floating everywhere — no static drain path.

**Options.**
- **A. Solid bond at master only; 10 nF at all units.** Shield has a defined DC reference; HF path preserved at every node.
- **B. 1 MΩ bleeder in parallel with 10 nF at every node.** DC path for static, HF path through the cap.

**Recommendation.** A.

**Blocks.** SCHEMATIC §A.9 and §B.1, assembly.

---

## Q11 — Common-mode choke on RS-485 ports

**Context.** For CE/FCC radiated emissions compliance, a CMC in series with each A/B pair is standard practice on RS-485.

**Options.**
- **A. Add CMC** (e.g. Würth 744232601, 100 Ω @ 100 MHz) on every SN65HVD75 port. 3 instances: 2 on master + 1 on unit (shared for J_IN / J_OUT passthrough).
- **B. Skip, compliance-test first, add in rev if failed.**

**Recommendation.** A. Cheap, no latency impact at 250 kbps, removes EMC respin risk.

**Blocks.** Master SCHEMATIC §A.6–A.7, Unit §B.6, BOM.

---

## Q12 — Input TVS class upgrade

**Context.** SMBJ58CA (600 W / 1 ms) is adequate for ESD, borderline for IEC 61000-4-5 surge class on a 48 V distributed bus.

**Options.**
- **A. Upgrade master input to SMDJ58CA** (3 kW / 1 ms). Keep SMBJ58CA on unit local tap.
- **B. Stay with SMBJ, rely on polyfuse to open on sustained clamp.**

**Recommendation.** A. Master sees the worst-case surge (longest cable, most exposure).

**Blocks.** BOM.

---

## Q13 — RS-485 isolation (installation-dependent)

**Context.** SN65HVD75 common-mode window: −7 V to +12 V. Single-enclosure deployment: no isolation needed. Distributed / multi-structure: common-mode can exceed the window.

**Options.**
- **A. No isolation** — assumes single-enclosure deployment.
- **B. Isolated transceivers** (ISO1410 or ADM2582E) — supports distributed deployment.

**Recommendation.** A for the first prototype. Revisit if a multi-enclosure deployment target emerges.

**Blocks.** SCHEMATIC, BOM (if B).

---

## Q14 — SWD header on unit: 4-pin or 5-pin?

**Context.** Current JP_SWD is 4-pin (3V3/GND/SWDIO/SWCLK). No NRST on the header; only on TP_NRST.

**Options.**
- **A. 5-pin (add NRST).** Supports connect-under-reset for factory fixtures and recovery from nBOOT_SEL corruption.
- **B. Keep 4-pin, rely on TP_NRST.**

**Recommendation.** A. Marginal cost, significant bring-up and factory robustness.

**Blocks.** Unit SCHEMATIC §B.5, BOM.

---

## Q15 — Unit board layer count and size

**Context.** Current target: 30 × 50 mm, 2 layers. 2 layers routes but crowds the ADC divider next to the LMR16006 switch node.

**Options.**
- **A. 2 layers at 35 × 55 mm.** ~$0.2/unit more from fab; gives layout breathing room.
- **B. 4 layers at 30 × 50 mm.** ~30 % fab cost uplift; better EMI containment and analog/digital isolation.

**Recommendation.** A. At 128× quantities the size bump is the cheapest way to buy routing margin.

**Blocks.** Layout handoff.

---

## Q16 — Master board layer count and size

**Context.** Current target: 120 × 80 mm, 4 layers. 4 layers is correct; 80 mm height is tight for placement zones (PSU / buses / LED bar / USB).

**Options.**
- **A. 120 × 80 mm, 4 layers** — as-is. Risks cramped placement near ESP32 antenna keep-outs.
- **B. 130 × 90 mm, 4 layers.** Dedicated zone per functional group, no crossings.

**Recommendation.** B. Negligible cost difference, large placement benefit.

**Blocks.** Layout handoff.

---

## Q17 — Unit unconditional power LED

**Context.** LED_STATUS is firmware-driven (PB8). If firmware fails to boot, there is no visual indication of power at the unit.

**Options.**
- **A. Add a hardwired power LED** — 2 mA red LED + 2.2 kΩ R on 5V_UNIT or LOCAL_48V. Lights without firmware.
- **B. Skip, rely on LED_STATUS.**

**Recommendation.** A. Trivial cost, significant bring-up value.

**Blocks.** Unit SCHEMATIC §B.11, BOM, layout.

---

## Q18 — Unit stepper hardware failsafe

**Context.** Firmware contract says coils de-energise at rest. If firmware hangs with coils energised, ULN2003 + motor dissipation becomes a reliability concern.

**Options.**
- **A. 100 kΩ pulldowns on STEPPER_A..D** at the MCU-to-ULN2003 input pins. Hung / reset MCU with Hi-Z GPIOs → coils off.
- **B. Watchdog on ULN2003 enable path** (discrete NMOS + MCU-toggled gate).
- **C. Rely on firmware contract only.**

**Recommendation.** A. Four resistors, very cheap hardware backstop to a firmware promise.

**Blocks.** Unit SCHEMATIC §B.8, BOM.

---

## Status summary

| Block | Gated on |
|---|---|
| Final GPIO allocation (rewrite PINOUT.md from scratch) | Q1, Q2, Q3 |
| Master schematic respin | Q4, Q5, Q6, Q7, Q9, Q10, Q11, Q12 |
| Unit schematic respin | Q8, Q9, Q10, Q11, Q14, Q17, Q18 |
| Layout handoff | Q15, Q16 (independent) |
| Deferrable | Q13 (installation-dependent) |

When a question is resolved, move its content into the appropriate schematic / policy document and delete the entry here with a commit message referencing the decision.
