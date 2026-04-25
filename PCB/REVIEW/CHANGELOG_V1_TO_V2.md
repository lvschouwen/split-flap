# Split-Flap PCB — v1 → v2 Changelog & Review Request

**Prepared for ChatGPT external review, 2026-04-25 (post-pivot architecture freeze).**
**Artifacts bundled alongside this doc:** `pcb_v1.zip`, `pcb_v2.zip`.

---

## 0. How to read this package

- `pcb_v1.zip` — the **as-manufactured 2021–2022 design** (EasyEDA JSON source, gerbers, old BOM, old pick-and-place). Currently-deployed hardware.
- `pcb_v2.zip` — the **clean-slate redesign**, post-2026-04-25 architecture freeze. **3-PCB system**: master + backplane (×N) + unit (×16·N). Markdown design briefs + BOM CSVs. **No schematic capture yet** — next workstream is JLC EasyEDA Pro entry or freelance layout-engineer hand-off.
- This changelog — walks the architectural delta and summarises the open review questions.

**`pcb_v2.zip` layout** (flattened to 8 files for ChatGPT's upload limit; the in-repo split is 19 per-section files at `PCB/v2/` on the `pcb-v2-rs485-48v` branch):

| File | Contains |
|---|---|
| `README.md` | Bundle entry point + system overview + scope |
| `CONNECTOR_PINOUTS.md` | Single canonical cross-board pinout contract |
| `MASTER.md` | Master PCB: DECISIONS (source of truth) + POWER + DIGITAL + MECHANICAL + BRINGUP, concatenated with `# Section N — <ORIGINAL>.md` headers |
| `MASTER_BOM.csv` | JLC-native BOM, master |
| `BACKPLANE.md` | Backplane PCB: DECISIONS + DESIGN + MECHANICAL + BRINGUP, concatenated |
| `BACKPLANE_BOM.csv` | JLC-native BOM, backplane segment |
| `UNIT.md` | Unit PCB: DECISIONS + POWER + DIGITAL + MECHANICAL + BRINGUP, concatenated |
| `UNIT_BOM.csv` | JLC-native BOM, unit |

**Sources of truth inside each per-board mega-doc** are the `Section 1 — *_DECISIONS.md` blocks; later sections (DESIGN / POWER / DIGITAL / MECHANICAL / BRINGUP) are derivative and must agree with the decisions section.

**Target prototype run:** 5× master boards + 5 case-sets of 4× backplane segments + 80× unit boards via JLC PCBA. User fabricates the 3D-printed enclosure parts.

---

## 1. System at a glance

A split-flap display: one master controller drives N enclosure cases; each case is a passive backplane PCB with 16 unit-slots; each slot holds a unit PCB driving one 28BYJ-48 stepper to show a character. v1 was an ESP-01 driving up to 16 Arduino Nanos over 5 V I²C in a star topology. v2 replaces every layer except the 45-flap drum mechanics and the 28BYJ-48 motor family.

Topology:

```
   +----------+
   |          |   shielded CAT5e/6 (T568B straight-through)
   |  MASTER  |---<---- RJ45 (V48 + RS-485 A/B + GND) ---->----+
   |          |                                                |
   +----------+                                                v
        |                                            +-------------------+
        |  4× independent buses                      |  Backplane case   |  16 unit-slots
        |  (≤2 cases per bus, ≤32 units per bus)     |  4× 320 mm segs   |  per case
        |                                            +-------------------+
        |                                                     |
        |                                            chain-out RJ45
        |                                                     v
        |                                            +-------------------+
        |                                            |  next case        |
        |                                            +-------------------+
```

Max system capacity: 4 buses × 2 cases × 16 units = **128 units**.

---

## 2. Architectural deltas (v1 → v2)

### 2.1 System partitioning *(NEW class of board in v2)*
**v1:** 2 PCB classes — master + unit. Units daisy-chained inside a case via 2× RJ45 each + ~15 internal patch cables per 16-unit case.
**v2:** **3 PCB classes** — master + backplane (×N) + unit (×16·N). The backplane is a new passive distribution PCB inside each case: **4 segments × 320 mm = 1280 mm strip**, 16 unit-slots per case via 2×3 box headers. Per-slot resettable polyfuse (nanoSMDC020F-2, 0.2 A hold). Only 2 case-level RJ45s per case (chain-in on segment-1, chain-out on segment-4); units have no RJ45 jacks at all.
**Why:** dropped 32 RJ45 jacks + 15 patch cables per 16-unit case (~€150 saved per 32-unit display, dramatically cleaner install, better SI without 16 jack-stubs per case).
**New risks:** segment-to-segment 6-pin board-to-board mating reliability; 1280 mm ≥ JLC's 400 mm standard fab limit forces segmentation; 320 mm V48 + RS-485 trace per segment with ≤5 mm stub per slot needs careful layout; no schematic capture yet to validate segment-to-segment SI.

### 2.2 Communication bus
**v1:** I²C @ 400 kHz, star topology, max ~16 units, 4-bit DIP-switch addressing.
**v2:** **RS-485 @ 500 kbaud, half-duplex**, 4 independent buses on master via **MAX14830ETJ+** SPI-UART bridge. Each bus daisy-chained over **shielded CAT5e/6**. Max 32 m total per chain, 3 m per hop. Per-bus transceiver: **SN65HVD75DR** (3.3 V, 500 kbaud).
**Termination:** 120 Ω 1% at master end; chain-end backplane has a `R_TERM` 120 Ω footprint populated only on the final case (PCBA stuffing variant).
**Failsafe bias:** 1 kΩ A→3V3 + 1 kΩ B→GND at master only (bumped from original 390 Ω post-pivot to cut 4-bus idle from 17 mA → 6.4 mA total).
**Wire format:** COBS(payload ‖ CRC16-BE(payload)) 0x00; 4-byte header `[ver][type][seq_hi][seq_lo]`. Canonical implementation in `firmware/lib/common/`.
**Cable risk:** crossover wiring shorts V48 to GND — T568B straight-through is mandatory; silkscreen warns at every RJ45.

### 2.3 Power distribution
**v1:** 5 V barrel jack, 5 V star, per-unit L7805 LDO, separate 12 V brick for stepper.
**v2:** **48 V over CAT6 pairs 4/5 + 7/8** (IEEE 802.3at Mode-B convention pinout, **passive — no PoE signature**). One inlet, one cable type, end-to-end.
- **Master:** SMDJ58CA primary TVS → LM74700-Q1 ideal-diode + SQJ148EP N-FET (1 kΩ GATE per TI SLUA975) → SMAJ51A secondary clamp → bulk → LMR36015AFDDA sync buck (with C_BOOT 100 nF) for 3V3; per-bus TPS259827YFFR eFuse + INA237AIDGSR current monitor; TPS3839L33 supervisor + 4× BAT54 hold the eFuses OFF until 3V3 is healthy.
- **Backplane:** fully passive — 2 mm V48 trace, per-slot polyfuse, 22 µF/100 V bulk per segment.
- **Unit:** AO3401 P-FET reverse-block → SMAJ51A clamp → **TPS54360DDA** 48→12 V buck (HSOP-8 PowerPAD, 22 µH Isat ≥1 A) → HT7833 LDO 12→3V3.

**Why:** at 48 V the I²R loss on 20–30 m CAT6 is ~1/100 of 5 V; eliminates the separate 12 V motor rail; centralises power in one cable.
**Risk:** common-mode failure (loss of 48 V = whole system down — accepted trade); 32-unit hot-plug aggregate input cap is ~800 µF and inrush must not trip the 1.7 A per-bus eFuse (post-pivot fix: dV/dt cap 10 nF → 100 nF). Cable polarity = catastrophic if reversed (T568B straight-through mandatory).

### 2.4 Master MCU
**v1:** ESP8266 (ESP-01, 160 MHz single-core, ~42 KB RAM, I²C master).
**v2:** **ESP32-S3-WROOM-1-N16R8** (240 MHz dual-core, 16 MB flash + 8 MB PSRAM, native USB-C, SPI to MAX14830). Pin budget: 14 assigned / 21 available / 7 spare.
**Why:** ESP-01 cannot run 4 UART buses + WiFi + web UI deterministically; second core isolates WiFi, PSRAM absorbs telemetry buffering.

### 2.5 Unit MCU
**v1:** ATmega328P-AU (Arduino Nano, 16 MHz, 32K/2K, I²C slave, address from DIP).
**v2:** **STM32G030K6T6** (Cortex-M0+, 64 MHz, 32K/8K, **LQFP-32**). RS-485 USART1 with hardware DE on PA12 (AF1). I²C master to on-PCB AS5600 encoder.
**Note:** the original 2026-04-24 v2 brief specced STM32G030F6P6 in TSSOP-20; cross-consistency review caught that PA12, PB0, and PB1 (used pins) aren't bonded on TSSOP-20 — escalated to LQFP-32 post-pivot.

### 2.6 Position sensing / homing
**v1:** KY-003 hall switch, one edge per revolution, 10–20 s homing sweep at boot per unit.
**v2:** **AS5600** 12-bit absolute magnetic encoder at I²C 0x36, mounted on-PCB, reads diametral magnet glued to motor shaft (0.5–3 mm gap, 1.5 mm target). Closed-loop step-loss detection during operation. Per-unit I²C RC filter (100 Ω + 100 pF on each line) added post-pivot to reject buck-switching coupling.
**Why:** kills the ~5 hr commissioning of 32 units; closed-loop catches stalls in real time.

### 2.7 Stepper driver
**v1:** ULN2003A bipolar Darlington array.
**v2:** **TPL7407L** MOSFET array (drop-in pinout, ~0.5 W less heat per unit at 300 mA stepping). Recommended by the scottbez1 audit as the modern equivalent.

### 2.8 Stepper motor
**v1:** 28BYJ-48 **5 V** variant (~150 mA per phase).
**v2:** 28BYJ-48 **12 V** variant (~70 mA per phase). Same mechanics. Mirrors scottbez1's choice and reduces total bus current budget.

### 2.9 Addressing / enumeration
**v1:** 4-bit DIP per unit → fixed I²C address (manual, collision-prone, no runtime reconfig).
**v2:** **Master-driven UID-based binary-tree search over RS-485** (firmware-only). Each unit's STM32G030 96-bit factory UID is its identity. Master broadcasts prefix-match queries; matching units echo their UID; collisions resolved by narrowing the prefix. UID→short-address map persists in NVS. Cold first-ever boot ~5–20 s for 32 units; warm boot ~100 ms.
**Note:** the 2026-04-24 v2 brief used a single-ended wake pin on RJ45 pin 1. Cross-consistency review caught that the master had no wake driver, no per-port driver circuitry, and no parts on the BOM — units would arrive correctly fabricated but the system would never enumerate. The wake-pin scheme was deleted; **RJ45 pin 1 is now `NC reserved`** system-wide.

### 2.10 OTA firmware update
**v1:** vendored twiboot I²C bootloader; master pushes unit firmware byte-wise from a PROGMEM hex; ~2 hr per 32 units.
**v2:** **RS-485 application-provided bootloader** with broadcast frames + batch-update frames + loopback integrity (baked into protocol from day one per scottbez1 audit). Designed but not yet implemented; ~16 s per 32 units projected. Factory first-flash via SWD pogo pads.
**Risk:** if firmware is corrupted mid-OTA, a unit is bricked until SWD recovery — but unlike the wake-pin scheme this no longer deadlocks the rest of the chain (UID-discovery skips dead nodes). Packet format / dual-bank A/B / rollback semantics still **not specified at packet level** (see §4).

### 2.11 Termination strategy
**v1:** manually soldered 120 Ω at bus ends.
**v2:** Master always populates 120 Ω + 1 kΩ bias (each leg). Backplane segment-4 has an `R_TERM` 120 Ω 0805 footprint **DNP by default**; PCBA stuffing variant populates only the chain-end case. Unit boards no longer carry termination (moved to backplane).

---

## 3. What intentionally did NOT change

- **Flap count per unit (45)** — same letter/symbol/blank alphabet as v1. No drum redesign.
- **Motor family (28BYJ-48)** — proven and cheap. NEMA-17 / servo were ruled out (~3× cost, added driver complexity). Voltage variant changed (5 V → 12 V) but mechanics identical.
- **Master WiFi family (Espressif)** — ESP-01 → ESP32-S3 keeps the ecosystem and PCB-integrated antenna; no external RF burden.

---

## 4. Open issues (post-pivot, current — reclassified by external review)

These are the items still open as of the 2026-04-25 freeze, with severities reclassified after the external review pass. Items resolved during the pivot are listed in §4.4 for ChatGPT context.

### 4.1 Critical — must address before schematic capture or layout release

1. **Master per-bus eFuse voltage-rating blocker (pass-2).** The selected `TPS259827YFFR` family + `TPS25981QWRPVRQ1` fallback are **electrically invalid on the 48 V bus** — both belong to TPS25982/TPS25981 families rated 24 V/30 V abs-max class, not 60 V. Pass-2 review caught this as a real "this won't work" issue, not a sourcing detail. **Active master eFuse design must move to a ≥60 V eFuse / hot-swap solution.** Candidate: `TPS26600PWPR` (LCSC `C544399`, HTSSOP-16-EP, 4.2–60 V industrial eFuse). Not drop-in: schematic + footprint + current-limit + inrush-network rework required. The DSBGA-10 manufacturability concern is now moot (HTSSOP-16-EP is a standard JLC PCBA package). **This blocks schematic capture and layout release.** **Severity: Critical (new in pass-2).**

2. **BOM LCSC numbers known unreliable. Tracked as issue [#75](https://github.com/lvschouwen/split-flap/issues/75). PLEASE DO A FULL BOM VERIFICATION PASS — see §5.** Pass-2 closed several `CHECK` rows (`U_INA237 → C2864837`, `D2 SM712 → C172881`, `U_POR → TLV803SDBZR C132016` candidate, `Q1 → C727381` verify-before-order); others remain unresolved. Several rows are now `MANUAL_FEEDER` / `MANUAL_LOCK_FOOTPRINT` / `GLOBAL_SOURCE` rather than CHECK — explicit non-JLC-Basic statuses, not pending verification. **Severity: Critical (unchanged).**

3. **MAX14830ETJ+ sourcing risk (master U12).** No clean JLC-stocked ETJ+ path. `MAX14830ETM+T` (LCSC `C2653202`) is package-mismatched and must **not** be substituted blindly. Three open options: (a) JLC manual-feeder/global-source ETJ+; (b) replace with two dual-UART SPI bridges after sourcing review; (c) reopen scope to a 2-bus ESP32-S3-native UART master. **Architectural decision required before tape-out.** Layout cannot start until this is locked. **Severity: Critical pre-tape-out (new in pass-2).**

4. **OTA-over-RS485 protocol not specified at packet level.** `UNIT_DIGITAL_DESIGN.md` describes the bootloader entry opcode but not packet format, CRC/auth, MTU, NACK/retry, rollback behaviour, dual-bank A/B layout. A corrupted mid-flash unit no longer deadlocks the chain (UID discovery skips it) but still bricks itself until SWD recovery. **Severity: Critical before field use** — not necessarily blocking schematic capture **if SWD recovery is robust on the prototype run**, but blocks field deployment.

5. **Cable length & signal integrity at 500 kbaud over CAT5e/6 not validated.** No SPICE, no field measurement. Worst case: 2 cases × 32 m chain ≈ 64 m, plus 16 backplane stubs per case (≤5 mm each). With SN65HVD75 slew rate, master-side-only 1 kΩ failsafe bias, and the new 16-stub backplane segments, is the last unit's RX margin still clean? **Severity: Critical before layout release.**

6. **Boot-time eFuse sequencing — hardware watchdog still missing.** TPS3839L33 supervisor (now `TLV803SDBZR` candidate per pass-2) + 4× BAT54 hold the eFuses OFF until 3V3 is healthy. But if firmware crashes during init, all 4 EN pins stay LOW and the system is locked off. No documented boot state machine, no IWDG forcing EN HIGH on firmware hang. **Severity: Critical.**

7. **48 V inlet labelling and human-factor warning.** Promoted from Medium to Critical safety/human-factor item by pass-1 external review. Real PoE PSEs refuse the cable (safe), but user-error interconnection into a shared patch panel is conceivable. Required mitigations: silkscreen `"PASSIVE 48V — NOT ETHERNET"` at every RJ45 (master + backplane); coloured/keyed cables (yellow/red jackets) preferred over generic CAT5e/CAT6 patch cords; user-facing docs must avoid any "structured cabling" or "patch panel" framing. **Severity: Critical safety.**

8. **Mechanical freelancer hand-off / AS5600 datum specification (pass-2 sharpened).** Promoted from Low to Critical for unit layout by pass-1; pass-2 sharpened the gate to **eleven explicit dimensional values** (motor shaft XY datum, AS5600 IC center coord + tolerance, magnet ⌀ + thickness + diametral magnetization spec, target / min / max air gap, allowed radial offset, allowed axial tilt, board mounting tolerance, motor harness clearance envelope). The unit is **not layout-ready** until all eleven are explicit numbers in a single dimensioned drawing or STEP file. Backplane bracket geometry (segment-joint mechanical reliability per `BACKPLANE_DECISIONS.md`) is the parallel gate for backplane layout. **Severity: Critical for unit + backplane layout.**

9. **Factory programming and test-fixture flow for 80-unit prototype run.** New issue from pass-1 external review. Production of 80 units requires a documented flow: SWD pogo jig (mechanical fixture + electrical), firmware flashing automation, UID capture (per-unit logging of STM32G030 96-bit UID + assigned address + version), per-unit pass/fail acceptance test (home + step + angle? + I²C-to-AS5600 + RS-485 loopback). Without this, the 80-unit run is bench-flashed one at a time with manual logbook entries — fine for 10, infeasible for 80. **Severity: Critical for the 80-unit prototype phase.**

### 4.2 Medium — should be addressed before fab order

10. **Creepage / clearance for 48 V path** for the freelance layout engineer. 48 V is below IEC 61010 Class-2 thresholds (≤60 V) so not a strict safety requirement, but IPC-2221 Class-1 wants ≥0.4 mm on unprotected 48 V nets. Master, backplane, and unit DECISIONS docs now carry explicit DRC class tables (pass-2) — `≥0.4 mm pre-clamp / ≥0.2 mm post-clamp`. **Severity: Medium — specified now.**

11. **CE/FCC compliance plan absent.** 48 V + 500 kbaud switching + WiFi master = a product likely needing EMC assessment if ever sold. **Severity: deferrable for personal prototype; Critical before sale.**

12. **Stepping current real-world measurement** on first prototype unit. Buck inductor Isat ≥ 1 A and per-slot polyfuse 0.2 A hold/0.4 A trip both assume ≤300 mA peak. If first prototype shows ≥600 mA peak per phase, both must be re-spec'd (firmware current-limit, larger inductor, larger polyfuse). **Severity: Medium; must happen on first prototype before ordering the 80-unit run.**

13. **RJ45 jack P/N consistency** between master and backplane. Both now `MANUAL_LOCK_FOOTPRINT` per pass-2 BOM update. **Severity: Medium but must be locked before footprint/layout** — generic 8P8C footprint is not acceptable for either board.

14. **U2 master buck — open footprint decision.** Pass-2 candidate substitute `LMR36015FSC3RNXRQ1 C1850344` is fixed-output VQFN-12, not the original adjustable HSOIC-8. Decision gate is open: keep adjustable / FB divider / HSOIC-8 (and re-search JLC for a cleaner LMR36015 variant), or commit to the fixed-output candidate (which deletes `R_FB_TOP/R_FB_BOT`). **Severity: Medium — must be locked before schematic capture.**

15. **U_POR master supervisor — open behaviour-verification.** Pass-2 candidate `TLV803SDBZR C132016` (SOT-23-3) replaces `TPS3839L33DBVR` (SOT-23-5). Output topology, reset delay, threshold, and BAT54 OR-gating compatibility must be verified before lock. **Severity: Medium — verify before schematic capture.**

### 4.3 Low — v2.1 material

14. **No spares / EOL mitigation plan** for ESP32-S3 module, AS5600, STM32G030, or MAX14830.
15. **Single status LED per unit** — blink-code semantics in user notes only, not in the v2 design docs.
16. **Backplane per-slot debug LEDs** — adds ~€0.30/slot, would cut bring-up debug time ~50 %; deferred. (DNP footprints recommended at zero cost.)

### 4.4 Closed during the pivot — for ChatGPT context

Listed only so you don't re-flag them. All resolved between 2026-04-24 and 2026-04-25 across two parallel external audits (a 7-agent multi-perspective review and a scottbez1-architecture comparison):

**Master:**
- R_SERIES + D_SEC chain dissipating 950 W steady — **deleted**; replaced with single SMAJ51A post-LM74700 per TI SLVA936.
- C_BOOT 100 nF on LMR36015 — **added** (was missing; would have bricked the high-side gate driver).
- Per-bus dV/dt 10 nF → **100 nF** (32-unit inrush 12.8 A peak vs. 1.7 A trip).
- INA237 VBUS divider 100 k/10 k → **10 k/1 k** (high source impedance destroyed 16-bit accuracy).
- FAULT LED moved to **MMBT3906 PNP** high-side switch (LM74700 FAULT can only sink 1 mA).
- RS-485 failsafe bias **390 Ω → 1 kΩ** each leg.
- BAT54 polarity **corrected** on EFUSE_EN POR-gating.
- LM74700 GATE **1 kΩ series** added per TI SLUA975.
- Master mounting holes moved to (6, 94)/(124, 94)/(6, 22)/(124, 22) — clears 21 mm RJ45 keep-out.

**Unit:**
- Buck swap **TPS54308DBV (SOT-23-6, no thermal pad, ≤80 °C/W unachievable) → TPS54360DDA** (HSOP-8 PowerPAD).
- MCU swap **STM32G030F6P6 TSSOP-20 → STM32G030K6T6 LQFP-32** (TSSOP-20 didn't bond PA12/PB0/PB1).
- Stepper driver **ULN2003A → TPL7407L** (~0.5 W less heat per unit).
- ESD swap **SP0504BAATG → SM712-02HTG** (5 V working voltage would fire on legitimate RS-485 traffic).
- TVS swap **SMBJ58CA → SMAJ51A** (matches master V48_RAIL clamp class).
- Reverse-polarity P-FET drain/source labels **corrected** (was reversed).
- Inductor **Isat ≥ 600 mA → ≥ 1 A** (28BYJ-48 stepping pulls ~300 mA peak per phase).
- AS5600 I²C **RC filter** (100 Ω + 100 pF) added.
- TPL7407L IN5–IN7 **hard-tied to GND** (was via 10 kΩ; false-turn-on risk).
- STM32 NRST **conditioning** (100 nF + 10 kΩ) added.
- SWD pads **resized**: 1.5 mm pads on 2.54 mm pitch (was 2 mm on 1.5 mm — too tight).
- Unit C_LDO_OUT **distinct LCSC** from C_LDO_IN (was a duplicated LCSC #).

**System-level:**
- **Backplane PCB introduced** as new third class (this changelog's biggest delta).
- **Wake-pin scheme deleted** in favour of UID-based discovery (master had no wake driver — system would never have enumerated).
- **BOM schema unified** to JLC-native across all three BOMs (Designator, Comment, Footprint, LCSC Part #, MPN, Manufacturer, Qty, JLC_Tier, Populate, EstEUR, DatasheetURL, Notes, BomType). Solves the JLC-uploader-rejecting-`#`-comments breakage and the column-mismatch breakage.
- **Broadcast / batch-update / loopback frames** baked into the RS-485 protocol from day one (RS-485 at 500 kbaud cannot match scottbez1's SPI-shift-register sub-millisecond 108-module update without these).
- **Standalone unit test mode** — when SWD pogo-jig connects without active SWD master, firmware repurposes PA13/PA14 as UART for `home`, `step <N>`, `angle?`, `id?` commands. Allows single-unit bench bring-up with no master/backplane.
- **Bring-up procedures written** — `MASTER_BRINGUP.md`, `BACKPLANE_BRINGUP.md`, `UNIT_BRINGUP.md` (closed pre-pivot P11).

---

## 5. BOM status — please verify *(highest priority)*

**Skip to here if you're short on time — it's the highest-impact review task.**

Pre-pivot, ~88 % of the LCSC `Cxxxxxx` part numbers in the v2 BOMs resolved to unrelated components (limit switches instead of LM74700-Q1, photodiodes instead of CM chokes, film caps instead of INA237s, etc.). Examples preserved here so you can sanity-check the failure mode:

| BOM line | MPN as written | LCSC as written | What LCSC actually returned |
|---|---|---|---|
| U1 master | LM74700-Q1 | C509439 | Hroparts K9-1267QW limit switch |
| U3–U6 master | TPS259827YFFR eFuse | C2906816 | empty / delisted |
| U7–U10 master | INA237AIDGSR | C2893010 | STE 100 nF film cap, 250 V |
| U2 master | LMR36015FDDA | C922105 | empty / HGSEMI MIC29302 LDO |
| U12 master | MAX14830ETJ+ | C2683133 | empty / MAX3221EUE+ |
| Q1 master | SQJ148EP N-FET | C2836025 | MATSUKI ME50N06A |
| L2–L5 master | 744232601 CM choke | C191126 | Vishay VEMD5510C photodiode |
| J4–J7 master | RJHSE-5380 RJ45 | C150877 | Skyworks RFX1010 RF IC |
| U1 unit | STM32G030F6P6 | C2888044 | empty page |
| U3 unit | AS5600 encoder | C78989 | KYX 24 MHz crystal |
| U4 unit | SN65HVD75DR | C143422 | empty page |
| U5 unit | TPS54308DBV | C123604 | Silergy SY8113BADC (different pinout + spec) |
| U2 unit | ULN2003ADR | C79209 | empty page |

Post-pivot, all three BOMs (`MASTER_BOM.csv`, `BACKPLANE_BOM.csv`, `UNIT_BOM.csv`) have been **schema-migrated** to a unified JLC-native column set:

`Designator, Comment, Footprint, LCSC Part #, MPN, Manufacturer, Qty, JLC_Tier, Populate, EstEUR, DatasheetURL, Notes, BomType`

`Populate` ∈ {`Y`, `DNP`, `END_OF_CHAIN`, `ALT_<group>`}; `BomType` ∈ {`onboard`, `offboard`, `fab`, `hardware`}. Several lines remain unresolved — but pass-2 split them into explicit categories rather than leaving them all as `CHECK`:

| `LCSC Part #` value | Meaning | Action |
|---|---|---|
| `Cxxxxxx` + `REVIEW_VERIFIED` / `REVIEW_PASS2_VERIFIED` note | live JLC part confirmed | safe to upload |
| `Cxxxxxx` + `REVIEW_PASS2_VERIFY_BEFORE_ORDER` | candidate, page metadata not reconciled | verify against datasheet before commit |
| `Cxxxxxx` + `REVIEW_PASS2_SUBSTITUTE` | candidate substitute, **not drop-in** | schematic + footprint change required |
| `MANUAL_FEEDER` | no JLC-stocked path | global-source / manual-feeder at PCBA quote |
| `MANUAL_LOCK_FOOTPRINT` | mechanical/connector item, lock by footprint | finalize before layout |
| `GLOBAL_SOURCE` | SI-critical part, not on JLC | global-source at PCBA quote |
| `QUOTE_TIME` | passive, picked at PCBA quote with constraints | acceptable as-is for the brief |
| `DELETE` | row to be removed at next BOM revision | architectural change (e.g. eFuse swap) |
| `CHECK` | still pending verification | should be empty after pass-2 — see §4.1 #1 |

**The MPN column is authoritative**, not the LCSC column. As part of your review, please:

1. For every active IC line in master + backplane + unit BOMs, look up the correct live LCSC / JLC part number by searching the MPN on https://jlcpcb.com/parts.
2. Note JLC Basic vs. Extended, current stock, and unit price at **qty 5 (master)**, **qty ~20 (backplane segment, 4 per case × 5 cases)**, and **qty ~80 (unit)**.
3. If JLC doesn't carry the MPN at all, suggest a drop-in substitute — ideally something in JLC's Basic library to cut the extended-part assembly fee.
4. Re-sum each BOM against the headline cost claims (~€80/master, ~€7.50/backplane segment, ~€8.90/unit). Flag if those are off by more than 15 %.
5. Passives (0603 R/C, bulk ceramic) can be treated as "JLC Basic equivalent, pick at PCBA quote time" — no need to hunt individual `Cxxxxxx`.

Hand the result back as a corrected BOM excerpt (MPN + correct `Cxxxxxx` + JLC stock + qty-priced + Basic/Extended) that can be pasted directly into the CSVs.

---

## 6. Cost / complexity delta summary

> **REVIEW_COST_WARNING:** current cost estimates are directional until the JLC BOM uploader accepts exact C-numbers/packages. Master cost is especially sensitive to MAX14830 availability, eFuse package choice, and current-monitor sourcing. Treat €80/master, €7.50/backplane segment, and €8.90/unit as unverified planning numbers.
>
> **REVIEW_PASS2_COST_WARNING:** pass-2 resolves several remaining `CHECK` rows (master `INA237 → C2864837`, unit `D2 SM712 → C172881`) but also surfaced a **master eFuse voltage-rating blocker** that forces a part-family change (`TPS259827YFFR` → `TPS26600PWPR C544399`, ~€3.20/each vs €2.50/each at qty 4 — adds ~€2.80 across the four buses, plus rework cost). The master estimate is **not closed** until: (a) the 60 V eFuse replacement is finalised at schematic capture, and (b) the MAX14830 sourcing / manual-feeder path is decided. Backplane and unit cost estimates remain plausible planning numbers.

| Axis | v1 | v2 (post-pivot) | Delta |
|---|---|---|---|
| **PCB classes** | 2 (master, unit) | 3 (master, backplane, unit) | +1 class |
| **Master unique ICs** | ~5 | ~17 | +3.4× |
| **Master cost** (qty 5, fab + PCBA + ICs + passives) | ~€6 | ~€80 *(UNVERIFIED — see §5)* | +€74 |
| **Master PCB** | 2-layer | 4-layer ENIG, 130 × 100 mm | +1 stack-up + ENIG |
| **Backplane cost** (per case = 4 segments) | n/a | ~€30 (€7.50/segment) | new |
| **Backplane PCB** | n/a | 2-layer HASL, 320 × 35 mm × 4 segments | new |
| **Unit cost** (qty 10) | ~€5 | ~€8.90 (excl. €1.50 motor + €0.30 magnet off-board) | +€3.90 |
| **Unit PCB** | 1–2 layer | 2-layer HASL, 75 × 35 mm | +50 % area |
| **Patch cables / 32-unit display** | 1 inlet + ~30 internal | 1 inlet + 1 inter-case | −30 cables |
| **Commissioning / 32 units** | ~5 hr (DIP + homing sweep) | ~10 s (UID + AS5600) | ~1800× faster |
| **System capacity** | 16 | 128 | 8× |
| **OTA / 32 units** | ~2 hr (twiboot) | ~16 s (projected) | ~450× faster |
| **Cable** | JST-XH 4-pin custom | shielded CAT5e/6 commodity | simpler, cheaper/m |

**Total displayed-system cost at 32 units** (1 master + 2 cases × 4 segments + 32 units):
≈ €80 + (2 × €30) + (32 × €8.90) ≈ **~€425** in PCBs+ICs (BOM unverified — see §5).

---

## 7. What I'd like you (ChatGPT) to return

Please structure your review as:

### A. BOM verification — **highest priority**
Corrected LCSC table per §5, across master + backplane + unit. If you can do only one thing, do this.

### B. Architecture sanity check
For each delta in §2, tell me:
- does the chosen part / topology actually solve the claimed problem?
- is there a cheaper / simpler / more robust alternative I should consider before schematic capture?
- any new failure mode the docs haven't flagged?

Particular interest:
- **The 3-PCB partition** — is the 4×320 mm backplane segmentation correct, or should the case be one full-length flex board, or one stretched panel split differently? Are 6-pin board-to-board headers the right inter-segment connector, or should it be a flex / FFC / wire harness?
- **The 4-bus master architecture** — is this overkill for a ≤128-unit home-display-scale system? Would 2 buses be enough?
- **48 V over CAT6 with no PoE signature** — accepted trade or genuine field-failure waiting to happen?

### C. Open-issue triage
For each item in §4.1–4.3, give me your view: critical, deferrable, or over-thinking? Anything NOT on the list that should be?

### D. Layout-readiness verdict
Is this package enough for a freelance PCB layout engineer, or are there missing pieces (placement constraints, stackup, controlled-impedance callouts, test-point plan, fiducials, panelization) that the engineer would bounce back asking for? **Per-board:**
- **Master** — 130 × 100 mm, 4-layer ENIG, ESP32-S3 antenna keep-out, controlled-impedance RS-485 + USB.
- **Backplane** — 320 × 35 mm × 4 segments, 2-layer HASL, mostly-passive, 16 box-headers per segment.
- **Unit** — 75 × 35 mm, 2-layer HASL, AS5600 origin reference, motor connector + 2×3 box-header.

### E. Things you'd do differently
Brutal and specific. "The 4-bus architecture is overkill at home-display scale, drop to 2 and save €20" beats polite hedging.

---

## 8. Housekeeping

- **Architectural decisions in §2 are locked** post-pivot. Push back only for clear "this won't work" cases, not stylistic preferences.
- **I am asking you to validate and sharpen.** BOM correctness, failure-mode completeness, open-issue triage, layout-readiness verdict.
- **Git repo:** https://github.com/lvschouwen/split-flap — branch `pcb-v2-rs485-48v`. Parent meta-issue **#73**; BOM correction sub-issue **#75**; pivot tracker **#76**.
- **Firmware context** (skip if reviewing hardware only):
  - v1 firmware (ESP8266 + Arduino Nano) frozen at tag `v-esp8266-final`.
  - v2 firmware: `firmware/MasterS3/` (ESP32-S3, in progress) + future `firmware/UnitG030/` (STM32G030, not yet started). RS-485 protocol canonical implementation in `firmware/lib/common/`.

Thanks.

---

## Appendix — Prompt to paste into ChatGPT alongside the two zips

> I'm handing you two zips and a changelog for external review of a PCB v2 redesign of a split-flap display project.
>
> - `pcb_v1.zip` — original 2021 / 2022 design: EasyEDA JSON source, gerbers, old BOM. Currently-deployed hardware.
> - `pcb_v2.zip` — clean-slate redesign at the 2026-04-25 architecture freeze. **3-PCB system**: master + backplane (×N) + unit (×16·N). 8 files: `README.md`, `CONNECTOR_PINOUTS.md`, three per-board mega-docs (`MASTER.md`, `BACKPLANE.md`, `UNIT.md`), and three BOM CSVs. **No schematic capture yet** — next workstream is JLC EasyEDA Pro entry or freelance layout engineer hand-off.
> - `CHANGELOG_V1_TO_V2.md` — walks every delta, flags open questions, tells you what I want back.
>
> **Start by reading the changelog**, especially §5 (BOM status) and §7 (what I'd like you to return). Inside the v2 zip, the per-board mega-docs are concatenations of the original section files; in each one the **`Section 1 — *_DECISIONS.md` block is the source of truth** — every later section (DESIGN / POWER / DIGITAL / MECHANICAL / BRINGUP) is derivative and must agree with the decisions section.
>
> **Highest-priority task: BOM verification.** Many LCSC `Cxxxxxx` part numbers in the original BOMs were wrong (resolving to limit switches, photodiodes, film caps — not the MPN listed beside them). Post-pivot all three BOMs have been schema-migrated to a JLC-native column set, but most active-IC lines still need live re-verification. The MPN column is authoritative. For every active IC across master + backplane + unit, please look up the correct live LCSC / JLC part on https://jlcpcb.com/parts and return a corrected BOM excerpt (note JLC Basic vs. Extended, stock, qty 5 / qty ~20 / qty ~80 pricing, and substitutes if JLC doesn't carry the MPN). Passives can be left as "JLC Basic equivalent, pick at PCBA quote time."
>
> **Then** do architecture sanity-check, open-issue triage, layout-readiness verdict, and "what you'd do differently" per §7.
>
> Architectural decisions in §2 of the changelog are locked — please push back only if you're confident they won't work, not for stylistic preferences. The goal is to validate and sharpen, not re-scope.
>
> Keep the "what you'd do differently" section brutal and specific. Strong opinions over polite hedging.
