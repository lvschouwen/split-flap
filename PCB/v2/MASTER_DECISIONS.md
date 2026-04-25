# Master PCB v2 — locked design decisions

> **Single source of truth for the master PCB.** Design agents bind to this file ONLY. No other file in this repo is authoritative for the v2 master design.
>
> Last edited: 2026-04-25 (post-review pivot to backplane architecture; see issue #76).

## System shape

- **Single MCU**: ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB Octal PSRAM), soldered SMD.
- **No second MCU / radio coprocessor on the master board.** S3's native Wi-Fi + BLE covers connectivity needs; no Zigbee/Thread on-board.
- **No decorative LED bar.** Health indicators only.
- **4 RJ45 ports on master**, each driving an RS-485 chain of cases. **One case = 16 units on a backplane** (see `BACKPLANE_DESIGN.md`). Max 2 cases per chain (per the bus inrush budget at C_dVdT = 100 nF), so 4 × 2 × 16 = **128 units max** across the master.
- **Architecture pivot (2026-04-25):** units are no longer daisy-chained jack-to-jack via CAT6. Each case carries a backplane that distributes the bus to its 16 unit-slots; only the case has external RJ45s. Master ↔ backplane and case-to-case use shielded CAT5e/6 patch cables. Unit-side termination, IN/OUT polarity, per-unit RJ45s are obsolete.
- **Address discovery: UID-based.** Each unit's STM32G030 96-bit factory UID is the identity. Master broadcasts a 1-Wire-style binary-tree search over RS-485, persists the resulting UID→short-address map in NVS (warm boot ~100 ms; cold first-ever boot 5–20 s for 32 units). DIP switches and wake-pin schemes are explicitly rejected. RJ45 pin 1 is `NC reserved` on both master and case.
- **Cabling standard**: shielded CAT5e or CAT6 (S/FTP or F/UTP), straight-through, T568B. RJ45 shielded connectors, no magnetics. **Max 32 m total per chain, max 3 m per hop, ≤7 V worst-case IR drop** at full bus current.

## RJ45 pinout (passive PoE-compatible — 802.3at Mode B convention)

This board follows the **IEEE 802.3at Mode B pinout convention** for power delivery, so it uses standard PoE-style pin assignments. It is NOT a true PoE PSE — there is no detection/classification handshake. A real 802.3af/at/bt switch will refuse to energize the cable (no PD signature present), which is safe behavior. Power is supplied by the master itself, always on (within eFuse limits).

| Pin | T568B wire color | Net | Notes |
|---|---|---|---|
| 1 | white-orange | NC (reserved) | Standard Mode B leaves pair 1/2 for Ethernet data — not used in this design |
| 2 | orange | NC (reserved) | " |
| 3 | white-green | RS-485 A | Green pair (3/6) is a true twisted pair in CAT6 — suitable for differential |
| 4 | blue | +48V | Blue pair (4/5) = Mode B V+ (paralleled for ~3 A conductor capacity) |
| 5 | white-blue | +48V | paralleled with pin 4 |
| 6 | green | RS-485 B | paired with pin 3 |
| 7 | white-brown | GND | Brown pair (7/8) = Mode B V− (paralleled) |
| 8 | brown | GND | paralleled with pin 7 |

**Cable requirement**: straight-through only (pin N → pin N). Crossover cables will short +48V to GND and damage both master and the first unit. All cables shipped/recommended with the system must be straight-through CAT5e/CAT6 patch cables.

**Current capacity over CAT6 (24 AWG)**: each conductor ~1.5 A; paralleled pair ~3 A. eFuse trip at 1.7 A per bus leaves ~75% headroom on the power pairs.
- **Max 128 units per master** (4 buses × 32 units).

## Power architecture

| Item | Decision |
|---|---|
| Input | 48 V DC barrel jack, 5.5 / 2.5 mm, 60 V rated |
| Reverse-polarity & ideal-diode | LM74700-Q1 ideal-diode controller + external N-MOSFET (Q1 = SQJ148EP, 100 V / 9 mΩ). FAULT pin → ESP32 GPIO via PROT_FAULT_OR. **1 kΩ series on GATE** (per TI SLUA975) for stability against Q1 Q_g ringing. |
| Input TVS | SMDJ58CA bidirectional (3 kW / 1 ms) at the barrel jack. The earlier R_SERIES + D_SEC secondary-clamp chain is **deleted (2026-04-25)** — its 47 Ω in series with the main rail dissipated ~950 W steady at 4.5 A, an architectural error. Replaced by reliance on LM74700-Q1's native overvoltage-protection behaviour per TI app note SLVA936. |
| V48_RAIL clamp | **Single SMAJ51A on V48_RAIL** (post-Q1 source), within 10 mm of Q1, sinking the surge-tail current that flows forward through Q1's body during a SMDJ58CA clamp event. Holds V48_RAIL ≤ ~57 V at low-end tail current — within LMR36015 V_IN abs-max (65 V), TPS259827 V_IN abs-max (60 V transient 65 V), and INA237 V_CM (85 V). |
| FAULT LED isolation | **BSS138 N-MOSFET inverter** drives FAULT LED from PROT_FAULT_OR, so the wired-OR line sinks only the 10 kΩ pull-up (~0.33 mA) — stays within LM74700's 1 mA FAULT I_OL spec. Locked 2026-04-25 (was open issue P4). |
| Rail architecture | **Single buck 48V → 3V3** via **LMR36015 (adjustable, FDDA = HSOIC-8 PowerPAD)** (60 V sync buck, 1.5 A, 2.1 MHz). FB divider populated for 3.3 V (P2 resolved: FDDA suffix is package code, base part is adjustable). **C_BOOT = 100 nF / 25 V X7R 0402** mandatory from CBOOT to SW pin within 2 mm. No 5 V rail. Every IC (ESP32-S3, MAX14830, SN65HVD75, INA237) is 3V3-native. USB-VBUS isolated from board rails. |
| Per-bus protection | **60 V-class eFuse / hot-swap per bus (×4)**. **Candidate: TPS26600PWPR (LCSC C544399, HTSSOP-16-EP)**, 4.2–60 V industrial eFuse with adjustable current limit (~1.7 A target), ENABLE + FAULT, controlled dV/dt inrush. **REVIEW_PASS2 BLOCKER (open):** the pre-pass-2 selection (TPS259827YFFR DSBGA-10 + TPS25981QWRPVRQ1 WQFN-12 fallback) was electrically invalid — TPS25982/TPS25981 families are 24 V/30 V abs-max class, **not valid on the 48 V rail**. The voltage-class error is the real blocker; DSBGA manufacturability was a secondary concern. Switching to TPS26600 requires: schematic rework (different pin layout, different protection-net topology), footprint change (HTSSOP-16-EP vs DSBGA-10/WQFN-12), current-limit network respec (different K_ILIM constant), and inrush network respec (different C_dVdT timing). NOT drop-in. **C_dVdT per bus** target remains the same goal (~28 ms ramp / ~1.4 A inrush vs ~1.7 A trip on a 32-unit bus with ~800 µF aggregate input cap) but the cap value must be re-derived from the TPS26600 datasheet. |
| Per-bus current telemetry | INA237AIDGSR per bus (×4), 50 mΩ high-side shunt, I²C. Addresses 0x40–0x43. **VBUS divider = 10 kΩ / 1 kΩ** (was 100 kΩ / 10 kΩ pre-2026-04-25; bumped to preserve 16-bit accuracy from INA237 input bias). 8 mW dissipation per bus at 48 V — acceptable. |
| Per-bus enable sequencing | At master boot, firmware sequences each bus enable serially: assert EFUSE_EN_n, wait for PGOOD, read INA237 settling current (≤ 200 mA after 50 ms = healthy). Refuse to leave bus enabled if INRUSH_NOT_SETTLED. Pattern adopted from scottbez1 Chainlink Base supervisor (#76 audit). |

**Cost note (flagged by user):** per-bus eFuse is ~€10 total across 4 buses. Accepted for safety. **Revisit candidate** if a later cost-trim pass is needed — fallback would be polyfuse + INA237-ALERT-driven software cutoff.

## Inter-chip communication

- **RS-485 UART expansion**: **1× MAX14830** (4 UARTs over SPI). All 4 bus UARTs live on this chip, NOT on the S3 native UARTs.
  - Shared SPI bus to S3: MOSI / MISO / SCK / CS / IRQ = 5 ESP32 GPIOs
  - **VEXT pin tied to 3V3** (I/O level reference) with its own 100 nF decoupling. Locked 2026-04-25.
- **RS-485 transceivers**: 4× SN65HVD75D (3.3 V, half-duplex), one per bus. DE/RE driven by MAX14830 per-channel DE pin (hardware-assisted, no firmware per-byte toggling).
- **RS-485 wire format (locked 2026-04-25):**
  - **Baud: 500 kbaud 8N1** (matches unit-side SN65HVD75 capability and 32 m chain SI margin).
  - **Framing: COBS(payload || CRC16-BE) 0x00**, reusing `firmware/lib/common/` from the v1→v2 firmware split. 4-byte header `[ver][type][addr_hi][addr_lo]`.
  - **Failsafe bias: 1 kΩ each leg** (master side only; unit/backplane do NOT bias). Idle differential ~280 mV — comfortably above SN65HVD75 200 mV failsafe threshold. Bias resistance bumped from 390 Ω (pre-2026-04-25) to reduce bus loading and idle current to 1.6 mA per bus.
  - **Termination: 120 Ω at master and at chain-end backplane** (PCBA stuffing variant on backplane).
  - **Message-type design includes broadcast + batched-update frames from day one** (post-#76 audit): `BROADCAST_SET_POSITION`, `BATCH_UPDATE` (start..end address range, single payload). Enables future sync animation across all units without a protocol rev.
  - **Loopback integrity frame**: master can send a special address-N loopback frame; the addressed unit echoes back. Allows end-to-end chain validation at boot. Pattern adopted from scottbez1's SR loopback bits.
- **Address discovery (locked 2026-04-25): UID-based binary-tree search over RS-485.** Each STM32G030 unit's 96-bit factory UID is its identity. Master broadcasts prefix-match queries; collisions show up as CRC fails and master narrows the prefix. Once enumerated, master assigns a 1-byte short address per unit and persists the UID→short-address map in NVS (also: UID→physical-slot map for calibration continuity across re-flashes). Cold first-ever boot ~5–20 s for 32 units; warm boot ~100 ms. RJ45 pin 1 is `NC reserved` on both master and case-side backplane — there is no wake-pin scheme.
- **Telemetry**: 4× INA237 on I²C bus (internal to the master — not exposed off-board). Addresses 0x40–0x43.
- **eFuse control**: 4× eFuse ENABLE pins driven by **direct ESP32-S3 GPIOs** (one per bus, firmware sequences bus-on at boot, waits for PGOOD + INA237 inrush settle, refuses if INRUSH_NOT_SETTLED). Each EN line is gated by a **TPS3839L33 3V3 POR supervisor** via 4× BAT54 Schottky diodes plus a 100 nF RC filter — eFuses are held OFF during 48 V brown-in until 3V3 is healthy AND firmware drives the GPIO. See `DIGITAL_DESIGN.md` §3j. **BAT54 polarity (corrected 2026-04-25): anode at /RESET, cathode at EFUSE_EN_n** (was reversed in pre-audit revision).
- **Fault aggregation**:
  - **PROT_FAULT_OR** — wired-OR of LM74700-Q1 FAULT + 4× eFuse FAULT → 1 ESP32 GPIO (IRQ). Drives FAULT LED via BSS138 inverter (so OR line only sinks the pull-up). Firmware reads individual status via INA237 voltage readback correlation + eFuse state knowledge.
  - **TELEM_ALERT_OR** — wired-OR of 4× INA237 ALERT → 1 ESP32 GPIO (IRQ). Firmware polls all 4 INA237s over I²C to identify which fired.
- **USB**: USB-C connector wired to ESP32-S3 native USB (IO19/IO20) for flash + debug + CDC. **Note for layout: IO19/IO20 are also strap pins for USB_JTAG vs UART download mode** — verify boot mode is unaffected by USB host presence at module reset.
- **SWD**: 1.27 mm 2×5 Cortex-debug header (JTAG on ESP32-S3).
- **No expansion header.** The master has no off-board connector for daughterboards. Future Zigbee/Thread/radio support would require a board rev or soldering wires to test pads on the native UART1/UART2 pins.

## LED set (8 total)

| # | Label | Driven by | Color | Visible without firmware |
|---|---|---|---|---|
| 1 | PWR_48V | Hardwired on 48 V via resistor | Red | ✅ |
| 2 | PWR_3V3 | Hardwired on 3V3 via resistor | Green | ✅ |
| 3 | FAULT | Hardwired on PROT_FAULT_OR node (OR of LM74700 FAULT + 4× eFuse FAULTs) | Red | ✅ |
| 4 | HEARTBEAT | ESP32-S3 GPIO. Encodes firmware liveness + WiFi status via blink pattern: off = dead; 2 Hz fast = booting/connecting; 1 Hz steady = running + WiFi up; 0.5 Hz slow = running + WiFi down/AP mode. | Blue | ❌ |
| 5 | BUS_ACT_1 | MAX14830 GPIO tied to channel-1 TX activity | Green | Partial (TX-time) |
| 6 | BUS_ACT_2 | MAX14830 GPIO tied to channel-2 TX activity | Green | Partial |
| 7 | BUS_ACT_3 | MAX14830 GPIO tied to channel-3 TX activity | Green | Partial |
| 8 | BUS_ACT_4 | MAX14830 GPIO tied to channel-4 TX activity | Green | Partial |

All 0603 SMD, fixed colors (no RGB). No dedicated WIFI LED — status encoded into HEARTBEAT blink pattern.

## Connectors

| Connector | Part class | Count |
|---|---|---|
| Power input | Barrel jack 5.5/2.5 mm, 60 V rated | 1 |
| Bus ports | Shielded THT RJ45, no magnetics | 4 |
| USB | USB-C receptacle | 1 |
| SWD | 1.27 mm 2×5 Cortex-debug header | 1 |
| Buttons | Tactile 6×6 SMD (RESET, BOOT) | 2 |

No expansion header / off-board I²C exposure / daughterboard socket. Master is a closed-function controller.

## GPIO budget (ESP32-S3-WROOM-1-N16R8)

### Available GPIOs on the N16R8 module

- Exposed IO pins on N16R8: IO0–IO21 + IO38–IO48 = 33 physical pins (IO33–IO37 consumed internally by Octal PSRAM).
- Minus hardware-fixed assignments:
  - IO19, IO20 → USB D+/D− (native USB, mandatory)
  - IO39, IO40, IO41, IO42 → JTAG/SWD (mandatory)
  - IO43, IO44 → UART0 console (policy-reserved; not remapped)
  - IO3, IO45, IO46 → strap pins, no copper escape
  - IO48 → glitch-prone, policy-avoided
- **General-purpose GPIOs available: 21.**

### Demand

| Function | Pins |
|---|---|
| SPI to MAX14830 (MOSI, MISO, SCK, CS, IRQ) | 5 |
| I²C bus (SDA, SCL) for INA237 bank | 2 |
| eFuse ENABLE × 4 (direct, firmware-sequenced bus-on) | 4 |
| PROT_FAULT_OR (wired-OR of LM74700 FAULT + 4× eFuse FAULT) | 1 |
| TELEM_ALERT_OR (wired-OR of 4× INA237 ALERT) | 1 |
| HEARTBEAT LED (on S3, not on MAX14830 — independent of downstream chip health) | 1 |
| **Total assigned** | **14** |
| **Spare** | **7** |

Spare pool (7 pins) reserved with 10 kΩ pull-ups + test pads for future signals. No new function gets assigned without a justification commit here.

### GPIO allocation policy (locked)

1. **Single owner per GPIO** unless explicitly a shared bus (SPI, I²C).
2. **UART0 (IO43/IO44) reserved** for boot/recovery/console. Not remapped at runtime.
3. **Strap pins IO3, IO45, IO46 no-route** beyond the module pad.
4. **IO48 unassigned** (policy avoid; acceptable use limited to non-critical test pad).
5. **INA237 ALERT lines wire-OR'd** onto a single TELEM_ALERT_OR pin (not 4 separate pins). Firmware identifies which fired via I²C poll of each INA237.
6. **eFuse FAULTs + LM74700 FAULT wire-OR'd** onto a single PROT_FAULT_OR pin. FAULT LED driven via BSS138 inverter (does NOT load the OR line directly).
7. **Pin numbers locked (2026-04-25)** in `DIGITAL_DESIGN.md` §2. Allocation: SPI to MAX14830 on IO10–14, I²C INA237 bank on IO8/9, eFuse EN on IO4–7, fault aggregation on IO15/16, HEARTBEAT on IO21. Spare pool = 7 pins (IO1, 2, 17, 18, 38, 47 + slack).

## Mechanical decisions (now locked)

- **Board dimensions: 130 × 100 mm** (full derivation in `MECHANICAL_DESIGN.md` §1).
- **Layer count: 4** (1.6 mm FR-4, ENIG finish, 1 oz outer / 0.5 oz inner). **V48 distribution lives on L1 only** — inner layers carry signal/GND only, not V48 (locked 2026-04-25 to keep the 0.5 oz inner thermally safe at 6.8 A fault).
- **Antenna keep-out: 18 × 15 mm** copper-free zone at the top edge, all 4 layers. Module does **not** overhang — kept fully on-board for shipping durability.
- **Mounting: 4× M3 corner holes**, positions (6, 94), (124, 94), (6, 22), (124, 22) measured from the bottom-left of the 130 × 100 board (locked 2026-04-25 — moved inboard from 5/95 to clear the 21 mm RJ45 keep-out and to give 2 mm minimum edge clearance on exclusion rings). Isolated pads, 0 Ω star-ground DNP per corner.
- **Enclosure**: custom user-fabricated, 130 × 100 mm PCB sized for desktop project boxes (Hammond 1455N family fits). Designed-to-fit approach: enclosure follows PCB, not the other way round.

## Prototype / production target

- **Prototype run: 5 boards**, JLC PCBA assembly path, user-fabricated enclosure (project-box class, e.g. Hammond 1455N).
- **Not production-ready.** Parts marked `CHECK` in the BOM are acceptable for the prototype run; LCSC re-verification is delegated to the ChatGPT BOM pass (#75).
- **Schematic capture path**: KiCad preferred to enable the **KiBot + kikit production pipeline** (CI-driven JLCPCB output, LCSC alt-part fields per component). Adopted post-#76 audit from scottbez1 — solves the "BOM numbers known-wrong" failure mode mechanically rather than editorially.

## Design artifacts (in this directory)

- `MASTER_DECISIONS.md` (this file) — source of truth; agents bind to this only.
- `POWER_DESIGN.md` — power subsystem schematic-level detail.
- `DIGITAL_DESIGN.md` — digital subsystem + complete ESP32-S3 pin allocation.
- `MECHANICAL_DESIGN.md` — placement brief, stack-up, zones, diff-pair rules.
- `MASTER_BOM.csv` — JLC-native BOM (Designator, Comment, Footprint, LCSC Part #, MPN, Manufacturer, Qty, JLC_Tier, Populate, EstEUR, DatasheetURL, Notes, BomType).
- `MASTER_BRINGUP.md` — first-power sequence + per-test-point readings + JLC PCBA verification steps.

## Still deferred (non-blocking, resolvable at schematic capture or fab-time)

Most pre-2026-04-25 open items are now resolved by the post-review revision. Remaining:
- **P3 — TPS259827YFFR I_LIM constant (K_ILIM).** Datasheet rev varies; pin R_ILIM at fab-time against the rev current then.
- **M3 closed (enclosure)** — designed-to-fit project-box class.
- **M5 closed (shield bond)** — master solid bond, unit/backplane float.
- **M6 closed (RJ45 pin-1 indicator)** — promoted to silkscreen must-have.
- **M9 closed (surge-clamp chain)** — simplified to single SMAJ51A on V48_RAIL after R_SERIES deletion.

## BOM / sourcing gates from external review (pass-2 update)

Bound on every part decision below. All notes apply to the master BOM (`MASTER_BOM.csv`) and propagate to schematic-capture and PCBA freeze.

### Hard blocker — eFuse voltage class (pass-2)

**The pre-pass-2 active eFuse path (TPS259827YFFR + TPS25981QWRPVRQ1) is electrically invalid on a 48 V bus.** Both belong to families rated 24 V/30 V abs-max class — they would instantly exceed abs-max on V48 inrush, let alone steady-state. This is a real "this won't work" finding, not a sourcing issue.

- **Candidate replacement: `TPS26600PWPR` (LCSC `C544399`, HTSSOP-16-EP)** — 4.2–60 V industrial eFuse / hot-swap, adjustable I_LIM, ENABLE + FAULT, programmable dV/dt inrush. Correct voltage class for V48.
- **Not drop-in.** Requires schematic rework (TPS26600 pin layout differs from TPS259827), footprint change (HTSSOP-16-EP vs DSBGA-10/WQFN-12), current-limit network respec (K_ILIM constant + R_ILIM target), and inrush network respec (C_dVdT timing math from the TPS26600 datasheet).
- **The DSBGA-10 0.4 mm manufacturability gate is now moot** — the new candidate is HTSSOP-16-EP, a standard JLC PCBA package.
- If a JLC-stocked 60 V eFuse with simpler integration exists and is found during the BOM-quote pass, swap to it (document why); but do **not** keep the TPS25982/TPS25981 path as the active 48 V solution.

### Other unresolved sourcing items (pass-2)

- **`MAX14830ETJ+` (U12) — major BOM/sourcing risk, unresolved.** No clean JLC-stocked ETJ+ path. `MAX14830ETM+T (C2653202)` is package-mismatched; do **not** substitute. Three open options: (a) JLC manual-feeder/global-source ETJ+ (extra PCBA cost); (b) replace with two dual-UART SPI bridges after sourcing review; (c) reopen architecture to a 2-bus ESP32-S3-native UART master (per the `REVIEW_STRONG_OPINION` below). **Architectural decision required before tape-out.**
- **`LMR36015AFDDA` (U2) — candidate substitute only.** Pass-2 found `LMR36015FSC3RNXRQ1 (C1850344)` in VQFN-12 (fixed-output 3.3 V). NOT footprint-compatible with HSOIC-8. Adopting it requires schematic + footprint change + deletion of `R_FB_TOP/R_FB_BOT` (fixed variant has internal divider). Decision gate is open.
- **`TPS3839L33DBVR` (U_POR) — candidate substitute only.** Pass-2 found `TLV803SDBZR (C132016)` SOT-23-3 active-low reset supervisor. Verify output topology (open-drain vs push-pull), reset delay, threshold, and BAT54 OR-gating compatibility before lock. Pinout/package differ.
- **`INA237AIDGSR` (U7–U10) — RESOLVED pass-2.** `LCSC C2864837` is the verified live JLC part. 85 V common-mode class is suitable for the 48 V monitoring scheme + existing 10 k/1 k VBUS divider.
- **`SQJ148EP-T1_GE3` (Q1) — verify-before-order.** Pass-2 found `LCSC C727381` that appears to resolve to the Vishay part, but page metadata must be reconciled against the datasheet before commit. Do **not** substitute a generic 100 V N-MOSFET without LM74700 ideal-diode SOA + Q_g review.
- **`J1` barrel jack, `J4–J7` RJ45, `L2–L5` CM choke** — `MANUAL_LOCK_FOOTPRINT` / `GLOBAL_SOURCE` markers in the BOM. Lock exact MPN by mechanical footprint and current rating before layout. Manual/consign acceptable for these mechanical/SI items; generic footprints are not.

### Standing rules

- **C-numbers are not trusted unless marked `REVIEW_VERIFIED` or `REVIEW_PASS2_VERIFIED`** in the BOM Notes column. Treat any other LCSC entry as advisory only.
- **If exact parts cannot be sourced, do not silently substitute package variants** — every package change must be re-validated against pinout, footprint, thermal pad, and PCBA process at minimum.

## LAYOUT_CONSTRAINT: master pre-layout checklist (pass-2 sharpened)

Layout engineer must clear all of these before accepting routing scope. Pass-2 added the concrete handoff items previously missing.

### Stackup, copper, impedance (pre-routing)

- **Exact JLC stackup name** required before routing — not "4-layer 1.6 mm" alone. Pick the named JLC stackup at quote time (e.g. `JLC04161H-7628`) and lock the dielectric thicknesses + Dk values it implies. The 1 oz outer / 0.5 oz inner spec follows from this choice.
- **Copper weight per layer** locked: outer 1 oz (35 µm), inner 0.5 oz (17 µm). Adjust V48 trace widths if the JLC quote forces a different stackup.
- **Impedance targets must include actual trace width / spacing** once the stackup is chosen — RS-485 A/B = 120 Ω differential, USB D+/D− = 90 Ω differential. Run the JLC impedance calculator for the chosen stackup and write the geometry into the layout brief, not a generic "120 Ω".

### Differential + sensitive nets

- **USB D+/D−** routed as 90 Ω differential pair, length-matched, over solid GND reference, with USBLC6-2SC6 in-line at the connector.
- **RS-485 A/B** routed as 120 Ω differential pair, length-matched, on L1 over solid L2 GND, with the Würth 744232601 (or substitute) CM choke and SM712 ESD placed close to the RJ45 connector. **Termination + bias resistors close to the MAX14830/PHY side or the RJ45 side as the SI analysis dictates** — locked before fab.
- **ESP32-S3 antenna keep-out** (18 × 15 mm, all 4 layers, no copper / no traces / no vias) copied **exactly** onto the mechanical/silkscreen layer and onto the enclosure interior.
- **RJ45 shield/ESD return strategy** documented on the layout: master is the sole shield-bond point system-wide; downstream RJ45 shields float; ESD return path explicitly identified.

### High-voltage, fiducials, panelization

- **48 V high-current path**: width per layer, copper weight, thermal vias, and clearance rules documented for the L1 V48 distribution islands. **≥ 0.4 mm pre-clamp / ≥ 0.2 mm post-clamp creepage.** No V48 on inner layers.
- **Fiducial count and placement**: 3 fiducials minimum (two diagonal corners + asymmetric third), 1 mm copper, 3 mm solder-mask opening, on top side. Add bottom-side fiducials (matching pattern) only if the bottom side has SMD parts requiring PCBA placement.
- **Panel rails / tooling holes**: 5 mm rail on long edges with 2× 3.2 mm tooling holes per rail. Mouse-bite or V-score policy: prefer V-score for rectangular outline (cleaner edge, kikit-friendly). Document the V-score line in the mech layer.

### Drill, clearance, silkscreen, soldermask (DRC class table)

| Class | 48 V net | RS-485 / USB | Logic | General |
|---|---|---|---|---|
| Trace-to-trace clearance | ≥ 0.40 mm pre-clamp / ≥ 0.20 mm post-clamp | ≥ 0.15 mm | ≥ 0.15 mm | ≥ 0.15 mm |
| Trace-to-edge clearance | ≥ 0.50 mm | ≥ 0.30 mm | ≥ 0.30 mm | ≥ 0.30 mm |
| Min drill | 0.30 mm | 0.30 mm | 0.20 mm | 0.20 mm |
| Min annular ring | 0.15 mm | 0.10 mm | 0.10 mm | 0.10 mm |
| Drill-to-copper (other net) | ≥ 0.30 mm | ≥ 0.20 mm | ≥ 0.20 mm | ≥ 0.20 mm |
| Silkscreen-to-copper (pad edge) | ≥ 0.10 mm | ≥ 0.10 mm | ≥ 0.10 mm | ≥ 0.10 mm |
| Soldermask sliver (between pads) | ≥ 0.10 mm | ≥ 0.10 mm | ≥ 0.10 mm | ≥ 0.10 mm |
| Edge-cuts to copper (any layer) | ≥ 0.50 mm | ≥ 0.30 mm | ≥ 0.30 mm | ≥ 0.30 mm |

### Test points, fiducials, signals

- **Test points** (1 mm SMD pads, edge-accessible where possible) on: V48_IN, V48_RAIL, each eFuse VOUT, each EFUSE_EN_n, each FAULT_n, INA237 I²C SDA/SCL, MAX14830 SPI MOSI/MISO/SCK/CS, MAX14830 IRQ + RESET, each RS-485 TX/RX/DE, 3V3, multiple GND.

### Open architectural decision before routing

- **U12 strategy must be finalised before tape-out**: keep MAX14830 with manual-feeder, swap to two dual-UART bridges, or drop to a 2-bus ESP32-S3-native master. The choice changes the SPI net topology, eFuse count, INA237 count, and overall PCB area. **Layout cannot start until this is locked.**

## REVIEW_STRONG_OPINION: 4-bus master architecture

The 4-bus MAX14830 architecture is technically workable but expensive and sourcing-risky. If 128 units is not a real near-term requirement, a 2-bus ESP32-S3-native UART master is cheaper and easier (no MAX14830, fewer eFuses, fewer INA237s, smaller PCB). Architecture remains locked unless owner reopens scope.

## REVIEW_SAFETY: passive 48 V over RJ45 labelling

Passive 48 V over RJ45 must be labelled as **NOT Ethernet/PoE**. Required:

- Silkscreen at every RJ45: **"PASSIVE 48V — NOT ETHERNET"**.
- Coloured/keyed cables preferred over generic CAT5e/CAT6 patch cords (e.g. yellow or red jackets) to discourage installation into a shared patch panel.
- Avoid any patch-panel installation language in user-facing docs. The system uses RJ45 connectors but is **not** a structured-cabling product.
