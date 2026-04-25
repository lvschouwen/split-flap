# Split-Flap PCB — v1 → v2 Changelog & Review Request

**Prepared for ChatGPT external review, 2026-04-24.**
**Artifacts bundled alongside this doc:** `pcb_v1.zip`, `pcb_v2.zip`.

---

## 0. How to read this package

- `pcb_v1.zip` — the **as-manufactured 2021–2022 design** (EasyEDA JSON source, gerbers, old BOM, old pick-and-place). This is the physical board currently in use.
- `pcb_v2.zip` — the **new clean-slate redesign**, as of 2026-04-24. Markdown design docs + BOM CSVs. **No schematic capture yet** — next workstream is JLC EasyEDA entry or freelance layout engineer handoff.
- This changelog — walks you through the architectural delta and summarises the open review questions.
- `MASTER_DECISIONS.md` and `UNIT_DECISIONS.md` inside the v2 zip are **the sources of truth**; every other doc (`*_DESIGN.md`, `*_BOM.csv`) is derivative and must agree with the decisions files.

**Target prototype run:** 5× master boards + 10× unit boards via JLC PCBA. User fabricates the 3D-printed enclosure themselves.

---

## 1. System at a glance

A split-flap display: one master board (WiFi / web UI / clock logic) driving N unit boards (one per flap), each unit steps a 28BYJ-48 stepper to show a character. v1 was built around an ESP-01 driving Arduino Nanos over I²C from a 5 V supply in a star topology. v2 replaces the bus, the power distribution, the microcontrollers, and the position-sensing strategy.

---

## 2. Major architectural changes

### 2.1 Communication bus
**v1:** I²C @ 400 kHz, star topology. Master ESP-01 drives up to 16 Arduino Nano units, each addressed by a 4-bit DIP switch on the unit.
**v2:** RS-485 @ 500 kbaud, half-duplex, daisy-chained over CAT5e/CAT6. Master has **4 independent buses** driven by a MAX14830 SPI-UART expander, each supporting up to 32 units. Max system capacity: 128 units.
**Rationale:** I²C with many stubs is capacitance-limited and unreliable at scale; 500 kbaud RS-485 over twisted pair handles long runs cleanly and natively supports daisy-chain. Four independent buses parallelise the screen-update path.
**New risks:** cable polarity sensitivity (straight-through mandatory — crossover shorts 48 V to GND); no PoE-PSE handshake, so a standard PoE switch will refuse to energise the cable (safe), but user-error interconnection could still damage a real PSE.

### 2.2 Power distribution
**v1:** 5 V barrel jack, 5 V distributed star; per-unit L7805 LDO; 12 V for stepper came from a separate off-PCB module.
**v2:** **48 V over CAT6 pairs 4/5 + 7/8** (IEEE 802.3at Mode-B convention pinout, passive — no PoE signature). Each unit has a local TPS54308 48→12 V buck + HT7833 12→3.3 V LDO. Master has its own LM74700 ideal diode + LMR36015FDDA sync buck to derive 3.3 V, plus per-bus TPS259827 eFuse and INA237 telemetry for fault isolation.
**Rationale:** at 48 V the I²R losses on 20–30 m CAT6 runs are ~1/100 of what they'd be at 5 V; eliminates the separate 12 V motor rail; centralises power in one cable.
**New risks:** single-supply common-mode failure (loss of 48 V = whole system down — accepted trade); inrush on hot-plug briefly drives the TVS (SMDJ58CA) clamp rail to ~93 V, mitigated by a secondary SMBJ60A clamp protecting the master's LMR36015 + INA237.

### 2.3 Master MCU
**v1:** ESP8266 (ESP-01, 160 MHz single-core, ~42 KB RAM, I²C master, limited GPIO).
**v2:** ESP32-S3-WROOM-1-N16R8 (240 MHz dual-core, 16 MB flash + 8 MB PSRAM, native USB-C, SPI to MAX14830). Pin budget: 14 assigned / 21 available / 7 spare.
**Rationale:** ESP8266 cannot run 4 independent UART buses + WiFi + web UI deterministically. S3's second core isolates WiFi; PSRAM absorbs telemetry buffering.

### 2.4 Unit MCU
**v1:** ATmega328P-AU (Arduino Nano, 16 MHz, 32 KB flash / 2 KB RAM, I²C slave, address from DIP switch).
**v2:** STM32G030F6P6 (Cortex-M0+, 64 MHz, 32 KB flash / 8 KB RAM, TSSOP-20). RS-485 UART with hardware DE control (no software RTS bit-banging). I²C master talks to the on-PCB AS5600 encoder.
**Rationale:** ATmega328P can't simultaneously run I²C slave + stepper control + sensor I/O without saturating the core at full step rate; STM32G030 has headroom and is cheaper at volume.
**New risks:** vendor lock-in to ST; no hobbyist-friendly bootloader out of the box — v2 uses SWD for factory first-flash, then RS-485 OTA.

### 2.5 Position sensing / homing
**v1:** KY-003 hall-effect switch on the flap carrier, one edge per revolution. Firmware homes by sweeping the motor up to 8 revolutions until the hall pin goes LOW — **takes 10–20 s per unit at boot**.
**v2:** **AS5600 absolute magnetic encoder** mounted on-PCB, reads absolute angle of a diametral magnet glued to the 28BYJ-48 output shaft. Known position instantly at power-on; closed-loop step-loss detection during operation.
**Rationale:** eliminates the homing sweep (32-unit commissioning drops from ~5 hr to ~2 min); closed-loop gives real-time stall detection.
**New risks:** magnet gluing procedure has to hit 0.5–3 mm gap window — mechanical bracket must enforce this; I²C pull-ups now mandatory on every unit.

### 2.6 Addressing / enumeration
**v1:** 4-bit DIP switch per unit → fixed I²C address at boot (DIP `0000` → `0x01`, etc.). Manual, collision-prone if two units set the same, no runtime reconfig.
**v2:** **Auto-enumeration** via a single-ended wake signal reused on RJ45 pair 1/2 (NC in the passive-PoE Mode-B pinout). Master drives WAKE HIGH; first un-addressed unit claims an address, drives its downstream WAKE_OUT HIGH, cascading down the chain. EEPROM-persisted after first assignment.
**Rationale:** removes the installer error-surface and the per-unit DIP-switch BOM cost; enables hot-plug.
**New risks:** WAKE drive style (push-pull vs open-drain) **not yet locked on the master side** — flagged as open-issue U7 in `UNIT_DECISIONS.md`. Unit currently defaults to push-pull with a 10 kΩ pull-down on receive.

### 2.7 OTA firmware update
**v1:** I²C bootloader (vendored twiboot) — master pushes unit firmware byte-wise over I²C from a PROGMEM hex. Full-system OTA takes ~2 hours for 32 units.
**v2:** **RS-485 bootloader** — firmware-level, application-provided (no ROM bootloader). 60× faster in principle; designed but not yet implemented. Factory first-flash is via SWD pogo-pin pads.
**New risks:** if firmware is corrupted mid-OTA a unit is bricked until SWD recovery — bootloader protocol (packet format, rollback/ping-pong banks) **not yet specified at packet level** (see §4 open issues).

### 2.8 Termination strategy
**v1:** manual soldered 120 Ω on bus ends during assembly.
**v2:** Every unit ships with a 2-pin 2.54 mm header and a populated 120 Ω resistor in series. Installer fits a shorting jumper only on the chain-end unit. Master provides its own end of the bus: 120 Ω termination + 2× 390 Ω fail-safe bias (A→3V3, B→GND).

### 2.9 Stepper motor
**v1:** 28BYJ-48 **5 V** variant.
**v2:** 28BYJ-48 **12 V** variant, unchanged mechanics, different winding. Lower per-phase current (~150 mA → ~70 mA) reduces bus current budget. ULN2003A driver unchanged. Mirrors scottbez1's v2 split-flap choice.

---

## 3. What intentionally did NOT change

- **Flap count per unit (45)** — same letter/symbol/blank alphabet as v1. No mechanical redesign of the drum.
- **Motor family (28BYJ-48)** — proven and cheap. NEMA-17 or servo were considered and ruled out (~3× cost, added driver complexity).
- **ULN2003A Darlington array** — still the right unipolar driver for 28BYJ-48; scottbez1 uses the equivalent MIC5842 (same topology).
- **Master WiFi module family (Espressif)** — ESP-01 → ESP32-S3 keeps the same ecosystem and PCB-integrated antenna; no external RF design burden.

---

## 4. Open issues flagged by internal review

These are the ones you (ChatGPT) should weigh in on. They are **not** yet addressed in v2 docs.

### 4.1 Critical — needs resolution before schematic capture

1. **BOM LCSC part numbers are systemically wrong. Tracked as issue [#75](https://github.com/lvschouwen/split-flap/issues/75). PLEASE DO A FULL BOM VERIFICATION PASS — see §5.**

2. **WAKE signal drive style not documented on master side.** Unit docs specify push-pull with a 10 kΩ pull-down on receive; master docs are silent on whether it's push-pull or open-drain, and whether there's a pull-up on the master side. A mismatch here causes either contention (two push-pull drivers) or a floating input (open-drain without pull-up). Needs a one-paragraph clarification in `MASTER_DECISIONS.md`.

3. **RJ45 jack P/N mismatch between master and unit BOMs.** Master BOM lists the RJHSE-5380 family; unit BOM lists RJHSE-5081 "or equiv" with a "CHECK STOCK" flag. They are electrically compatible but should be a single P/N across both boards for sourcing and cable compatibility.

4. **OTA-over-RS485 protocol is not specified at packet level.** `UNIT_DIGITAL_DESIGN.md` describes the bootloader entry opcode but not: packet format, CRC/auth, MTU, NACK/retry, rollback behaviour, dual-bank A/B layout. A corrupted mid-flash unit today deadlocks the downstream chain (WAKE_OUT stuck LOW).

5. **Cable length & signal integrity at 500 kbaud over CAT6 not validated.** No analysis, no SPICE, no field measurement. 32 units × ~2–3 m hops = ~100 m total. With SN65HVD75 slew rate and master-side-only bias, is the last unit's receive margin still clean? Design docs give no answer.

### 4.2 Medium — should be addressed before fab order

6. **Boot-time eFuse sequencing + watchdog.** Master's 4× TPS259827 EN pins are direct ESP32-S3 GPIOs. If firmware crashes during GPIO init, all EN pins stay LOW, system is locked. No documented boot state machine, no hardware watchdog forcing EN HIGH on firmware hang.

7. **Creepage / clearance for 48 V path not called out** for whoever does the freelance layout. 48 V is below IEC 61010 Class-2 thresholds (≤ 60 V) so it's not a strict safety requirement, but IPC-2221 Class-1 asks for ≥ 0.4 mm on unprotected 48 V nets. Not currently specified.

8. **Unit LDO thermal margin is tight.** `UNIT_POWER_DESIGN.md` puts T_J at ~105 °C at 40 °C ambient (only 20 °C from the 125 °C limit) and assumes θ_JA ~150 °C/W. In a sealed enclosure or at 50 °C ambient the HT7833 will throttle. The 3D-printed enclosure design needs to guarantee convection around the LDO.

9. **CE/FCC compliance plan absent.** 48 V + 500 kbaud switching + WiFi on the master = a product likely needing EMC assessment if ever sold. No compliance path documented anywhere.

10. **Product safety label for the 48 V inlet.** Looks like a regular RJ45 — real PoE PSEs will refuse it (safe), but user-error interconnection into a shared patch panel is conceivable. Nothing in the docs today warns the installer.

### 4.3 Low — v2.1 material

11. **Bring-up / test-fixture procedure not written.** A freelance assembler or field tech receiving a board has no documented: boot sequence, expected LED states, minimal bench test.
12. **No spares / EOL mitigation plan** (ESP32-S3 module, AS5600, STM32G030).
13. **Single status LED per unit** — blink-code semantics only exist in user notes, not in any v2 doc.

---

## 5. BOM status — **please verify**

**Skip to this if you're short on time — it's the highest-impact review task.**

Both `PCB/v2/MASTER_BOM.csv` (~50 lines) and `PCB/v2/UNIT_BOM.csv` (~25 lines) were populated with LCSC `Cxxxxxx` part numbers that do **not** match the MPN listed next to them. A Claude-driven spot check against https://jlcpcb.com/parts and https://www.lcsc.com found that out of 18 active ICs spot-checked, **only 2 resolve to the correct part**. Examples confirmed wrong:

| BOM line | MPN as written | Cxxxxxx as written | What LCSC actually returns |
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

The **MPN column is authoritative**, not the LCSC column. Please, as part of your review:

1. For every active IC line in both BOMs, look up the correct live LCSC / JLC part number by searching the MPN on https://jlcpcb.com/parts.
2. Note JLC Basic vs Extended, current stock, and unit price at **qty 5 (master run)** and **qty 10 (unit run)**.
3. If JLC doesn't carry a given MPN at all, suggest a drop-in substitute — ideally something in JLC's Basic library to cut the extended-part assembly fee.
4. Re-sum the BOM against the headline cost claims (€85–100/master, €9.80/unit). Flag if those are off by more than 15 %.
5. Passives (0603 R/C, bulk ceramic) can be treated as "JLC Basic equivalent, pick at PCBA quote time" — no need to hunt individual Cxxxxxx for them.

Hand the result back as a corrected BOM excerpt (MPN + correct Cxxxxxx + JLC stock + price + Basic/Extended) that can be pasted directly into the CSVs.

---

## 6. Cost / complexity delta summary

| Axis | v1 | v2 | Delta |
|---|---|---|---|
| **Master unique ICs** | ~5 | ~15 | +3× (4 buses + telemetry) |
| **Master BOM cost** (est.) | ~€6 | ~€57 claimed (UNVERIFIED — see §5) | +€51 |
| **Master PCB** | 2-layer, ~100×100 mm | 4-layer ENIG, 130×100 mm | +1 stackup |
| **Unit BOM cost** (est.) | ~€5 | ~€9.80 claimed | +€4.80 |
| **Unit PCB** | 1–2 layer, 50×30 mm | 2-layer, 65×35 mm | +30 % area |
| **Commissioning time / 32 units** | ~5 hr (manual DIP + homing sweeps) | ~2 min (auto-enum + AS5600) | **~150× faster** |
| **System capacity** | 16 units | 128 units | +8× |
| **OTA time / 32 units** | ~2 hr (I²C twiboot) | ~16 s (RS-485, projected) | **~450× faster** |
| **Cable** | JST-XH 4-pin custom | RJ45 + CAT5e/6 commodity | simpler install, cheaper per metre |

---

## 7. What I'd like you (ChatGPT) to return

Please structure your review as:

### A. BOM verification — **highest priority**
Corrected LCSC table per §5. If you can only do one thing, do this.

### B. Architecture sanity check
Take the v2 zip. For each of the 9 major changes in §2, tell me:
- does the chosen part / topology actually solve the claimed problem?
- is there a cheaper / simpler / more robust alternative I should consider at this stage (before schematic capture)?
- any new failure mode the docs haven't flagged?

### C. Open-issue triage
For each of the 13 open items in §4, give me your view: is it critical, deferrable, or over-thinking? Are there items NOT on my list that should be?

### D. Layout-readiness verdict
Is this package enough for a freelance PCB layout engineer, or are there missing pieces (component placement constraints, stackup, controlled-impedance callouts, test-point plan) that a layout engineer would bounce back asking for?

### E. Things you'd do differently
If this were your project, what would you change and why? Keep this section brutal and specific — I'd rather hear "the 4-bus architecture is overkill for a home-display-scale split-flap, drop it to 2 and save €20" than polite hedging.

---

## 8. Housekeeping

- **I am not asking you to redesign the system.** The architectural decisions in §2 are locked. Feedback on them is welcome but only for clear "here's why this won't work" cases.
- **I am asking you to validate and sharpen.** BOM correctness, failure-mode completeness, open-issue triage, layout-readiness.
- **Git repo for context:** https://github.com/lvschouwen/split-flap — branch `pcb-v2-rs485-48v`. Parent meta-issue #73, BOM correction sub-issue #75.
- **Firmware context (if helpful):** v1 firmware (ESP8266 + Arduino Nano) is frozen at tag `v-esp8266-final`. v2 firmware development is a separate workstream (`firmware/MasterS3/` — in progress). Unit-side v2 firmware doesn't exist yet.

Thanks.

---

## Appendix — Prompt to paste into ChatGPT alongside the two zips

> I'm handing you two zips and a changelog for external review of a PCB v2 redesign of a split-flap display project.
>
> - `pcb_v1.zip` — the original 2021 / 2022 design: EasyEDA JSON source, gerbers, old BOM.
> - `pcb_v2.zip` — the new clean-slate redesign, markdown docs + BOM CSVs. No schematic capture yet.
> - `CHANGELOG_V1_TO_V2.md` — walks you through every delta, flags open questions, tells you what I want back.
>
> **Start by reading the changelog**, especially §5 (BOM status) and §7 (what I'd like you to return). The v2 zip's `MASTER_DECISIONS.md` and `UNIT_DECISIONS.md` are the sources of truth; treat other v2 docs as derivative.
>
> **Highest-priority task: BOM verification.** Most LCSC `Cxxxxxx` part numbers in both BOMs are wrong (they resolve to unrelated components — limit switches, photodiodes, film caps — not the MPN listed next to them). The MPN column is authoritative. For every active IC, please look up the correct live LCSC / JLC part number on https://jlcpcb.com/parts and return a corrected BOM excerpt. Passives can be left as "JLC Basic equivalent, pick at PCBA quote time."
>
> **Then** do architecture sanity-check, open-issue triage, layout-readiness verdict, and "what you'd do differently" per §7.
>
> Architectural decisions in §2 of the changelog are locked — please push back on them only if you're confident they won't work, not for stylistic preferences. The goal is to validate and sharpen, not re-scope.
>
> Keep the "what you'd do differently" section brutal and specific. I'd rather hear a strong opinion than polite hedging.

---

# Appendix B — 2026-04-25 update (post-review architecture pivot + electrical fixes)

The original 2026-04-24 review bundle (this document above) went out for **two parallel external audits**:

1. **#76 — 7-agent multi-perspective review** of the v2 brief: PCB design (master + unit), product/handoff readiness (master + unit), RS-485 + 48 V link, BOM/sourcing, cross-board consistency. Surfaced ~80 findings ranging from typos to fundamental architectural breaks.
2. **scottbez1 audit** — 3-agent comparison against the mature open-source [splitflap project by Scott Bezek](https://github.com/scottbez1/splitflap) (108-module display, ~10 years of iteration). Validated the v2 architecture is right for product-scale operation but flagged tactical wins to steal.

The bundled `pcb_v2.zip` has been regenerated to reflect the post-review state. The system architecture has changed in one major way (backplane introduced), three major part swaps were made, and ~50 individual fixes were applied. **The decisions docs and BOMs in v2.zip are the new sources of truth** — read those, not the pre-2026-04-25 narrative in §1–§7 above (which is preserved for historical context but is now superseded where it contradicts the new docs).

## B.1 Architectural change — backplane PCB introduced

**Pre-2026-04-25**: each unit had 2× RJ45 jacks for unit-to-unit daisy-chaining within a case (16 modules per case = 32 RJ45 jacks + 15 patch cables per case).

**Post-2026-04-25**: a new **backplane PCB** lives in each case, distributing V48 + RS-485 to 16 unit-slots via a 2×3 box-header per slot. The backplane is split into 4 segments × 320 mm each (a single 1280 mm board exceeds JLC's standard fab limit). Each unit drops its 2× RJ45s and gains a single 2×3 male pin header that mates with the backplane socket. Only 2 case-level RJ45s remain (chain in / chain out, on the outer-segment ends).

Net per 32-unit display: ~€150 saved (smaller unit PCBs + no internal patch cables); dramatically cleaner install; better signal integrity (no 16 jack-stubs per case). Cost: one new PCB design (the backplane).

The system is now a **3-PCB system**: master + backplane (×N) + unit (×16N). New design-doc set in `pcb_v2.zip`: `BACKPLANE_DECISIONS.md`, `BACKPLANE_DESIGN.md`, `BACKPLANE_MECHANICAL.md`, `BACKPLANE_BOM.csv`, `BACKPLANE_BRINGUP.md`.

## B.2 Critical electrical fixes (master)

- **R_SERIES + D_SEC surge chain deleted.** R_SERIES (47 Ω in series with the main rail) dissipated 950 W steady at 4.5 A — an architectural error. Replaced by reliance on LM74700-Q1 native overvoltage protection per TI app note SLVA936, plus a single SMAJ51A (D_RAIL) clamping V48_RAIL post-Q1.
- **C_BOOT 100 nF added** on LMR36015 (was missing — would brick high-side gate driver).
- **Per-bus C_dVdT bumped 10 nF → 100 nF.** At 32-unit bus inrush (~800 µF aggregate input cap), the original 10 nF gave ~12.8 A peak inrush against a 1.7 A trip — master could not bring up a populated chain.
- **INA237 VBUS divider 100 k/10 k → 10 k/1 k.** High source impedance destroyed the INA237's 16-bit accuracy.
- **FAULT LED moved to MMBT3906 PNP high-side switch.** Prior topology pulled 2.3 mA on PROT_FAULT_OR; LM74700 FAULT can only sink 1 mA.
- **RS-485 failsafe bias 390 Ω → 1 kΩ each leg.** Reduces idle current 17 mA → 6.4 mA across 4 buses.
- **BAT54 polarity corrected** on the EFUSE_EN POR-gating (was reversed).
- **LM74700 GATE 1 kΩ series resistor added** per TI SLUA975 (Q_g stability).
- **Master mounting holes moved** to (6,94)/(124,94)/(6,22)/(124,22) — pre-existing positions had 0.75 mm edge clearance and bottom holes collided with the 21 mm RJ45 keep-out.

## B.3 Critical electrical fixes (unit)

- **Buck swap: TPS54308DBV → TPS54360DDA.** TPS54308 is SOT-23-6 with no thermal pad; the prior "≤ 80 °C/W" thermal spec was physically unachievable. TPS54360DDA is HSOP-8 PowerPAD with a real exposed pad.
- **MCU package swap: STM32G030F6P6 (TSSOP-20) → STM32G030K6T6 (LQFP-32).** Pre-2026-04-25 pin map referenced PA12 / PB0 / PB1 — none of which are bonded on TSSOP-20.
- **Stepper driver swap: ULN2003A → TPL7407L** (per scottbez1 audit). Drop-in MOSFET array, ~0.5 W less heat per unit at 300 mA stepping.
- **ESD swap: SP0504BAATG → SM712-02HTG.** SP0504 clamps at 5 V working voltage and would fire on legitimate RS-485 traffic.
- **TVS swap: SMBJ58CA → SMAJ51A.** Matches master's V48_RAIL clamp class.
- **Reverse-polarity P-FET drain/source labels corrected** (was reversed pre-2026-04-25).
- **Inductor Isat ≥ 600 mA → ≥ 1 A** (per scottbez1 audit; 28BYJ-48 stepping pulls ~300 mA peak per phase).
- **AS5600 I²C RC filter added** (100 Ω + 100 pF per line) to reject buck-switching coupling.
- **TPL7407L IN5–IN7 hard-tied to GND** (was via 10 kΩ; false-turn-on risk).
- **STM32 NRST conditioning added** (100 nF + 10 kΩ).
- **SWD pads sized correctly**: 1.5 mm pads on 2.54 mm pitch (was 2 mm pads on 1.5 mm spacing — too tight).
- **C_LDO_OUT distinct LCSC from C_LDO_IN** (was C15849 dupe).

## B.4 Discovery / addressing change

Pre-2026-04-25 used a single-ended **wake signal on RJ45 pin 1** to serialise enumeration. Cross-consistency review (#76) caught that the master had no WAKE_OUT GPIO, no per-port driver, and no parts on the BOM — units would arrive correctly fabricated but the system would never enumerate.

**Replaced by master-driven UID-based binary-tree search over RS-485** (firmware-only). Each unit's STM32G030 96-bit factory UID is its identity. Master broadcasts prefix-match queries; matching units echo their UID; collisions resolved by narrowing the prefix. Master persists the UID→address map in NVS. Cold first-ever boot ~5–20 s for 32 units; warm boot ~100 ms. RJ45 pin 1 is `NC reserved` on both master and case-side. No DIP switches, no wake pin.

## B.5 Protocol additions (broadcast frames + loopback integrity)

Per scottbez1 audit, RS-485 at 500 kbaud cannot match Scott's SPI shift-register sub-millisecond 108-module update latency. Solution baked into the v2 protocol from day one: `BROADCAST_SET_POSITION`, `BATCH_UPDATE` (single payload across address range), and a `LOOPBACK` integrity message. Wire format remains COBS(payload || CRC16-BE) 0x00.

## B.6 BOM schema migration (all three boards)

Pre-2026-04-25 master BOM had `#`-prefixed comment lines that broke JLC's BOM uploader, different column schema between master + unit BOMs, no DNI/Populate column, no JLC tier annotation, mixed grouped designators with `..` shorthand.

All three v2 BOMs now use a unified **JLC-native schema**:
`Designator, Comment, Footprint, LCSC Part #, MPN, Manufacturer, Qty, JLC_Tier, Populate, EstEUR, DatasheetURL, Notes, BomType`

`Populate`: `Y` / `DNP` / `END_OF_CHAIN` / `ALT_<group>`.
`BomType`: `onboard` / `offboard` / `fab` / `hardware`.

## B.7 Production pipeline + standalone test mode

Adopted from scottbez1's mature workflow: **KiBot + kikit panelize.json + LCSC alt-part fields per part** when the freelancer produces KiCad sources. Solves the "BOM LCSC numbers known-wrong" failure mode mechanically.

**Standalone unit test mode** (also from scottbez1): when the SWD pogo-jig connects without an active SWD master, unit firmware exposes a text protocol on PA13/PA14 repurposed as UART RX/TX. Commands: `home`, `step <N>`, `angle?`, `id?`. Allows single-unit bench bring-up without master/backplane. Documented in `UNIT_BRINGUP.md`.

## B.8 What's still open

- LCSC re-verification across all `CHECK` BOM rows — delegated to ChatGPT BOM pass per #75. **Unchanged from pre-2026-04-25.**
- DSBGA-10 0.4 mm pitch hard pre-fab gate at JLC — confirm at fab-time.
- TPS259827 K_ILIM constant verification at fab-time (datasheet rev varies).
- Stepping current real-world measurement on first prototype unit.
- Mech freelancer hand-off via STEP exchange for AS5600 origin, mounting holes, backplane brackets.
- Master enclosure design (designed-to-fit project-box class).

## B.9 What's no longer open

The 2026-04-24 review bundle's Master open issues P1, P2, P4, M3, M5, M6, M9 are all now resolved. Unit open issues U1, U2, U4, U5, U7, U8, U9, U10 are resolved. See per-board DECISIONS docs for the closure rationale.
