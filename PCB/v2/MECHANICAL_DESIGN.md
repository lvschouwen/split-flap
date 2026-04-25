# Master PCB v2 — Mechanical / Placement Brief

> **Scope:** board dimensions, layer count, stack-up, placement zones, edge-connector layout, antenna keep-out, mounting holes, copper-pour strategy, differential-pair rules, silkscreen requirements. This is a **placement brief** for a layout engineer (JLCPCB EasyEDA service or a freelancer), not a finished layout.
>
> **Binding references (only):** `MASTER_DECISIONS.md`, `POWER_DESIGN.md`, `DIGITAL_DESIGN.md`. Last edited 2026-04-25 (post-review revisions; see issue #76).

---

## 1. Proposed board dimensions + layer count

**Headline: 130 mm × 100 mm, 4-layer, 1.6 mm FR-4, ENIG, 1 oz outer / 0.5 oz inner.**

### Area math (component footprint budget)

| Block | Approx footprint (mm²) |
|---|---|
| 4× RJ45 shielded THT (16 × 21 mm each) | 1344 |
| ESP32-S3-WROOM-1 module (18 × 25.5) | 459 |
| Per-bus cluster overhead × 4 (shunt + eFuse + INA237 + SN65HVD75 + CMC + TVS + passives, ~200 mm² ea.) | 800 |
| Input power zone (jack + TVS + LM74700 + Q1 + secondary clamp + buck IC + inductor + bulk caps + MLCCs) | 700 |
| MAX14830 + crystal + load caps + decoupling | 120 |
| USB-C receptacle + ESD + ferrite + bulk | 150 |
| SWD header + debounce RCs | 60 |
| Buttons (2× 6×6) | 72 |
| LEDs (8×) + series resistors | 40 |
| Misc decoupling / pull-up / wire-OR passives (~60× 0402/0603) | 250 |
| **Component footprint subtotal** | **~4000** |
| + 30 % routing overhead for inner/outer signal layers | +1200 |
| + courtyard, keep-out, zone-isolation clearance (1.8× practical layout factor) | ×1.8 |
| **Required board area** | **~9400 mm²** |

9400 mm² ≈ 97 × 97 mm minimum. **The four RJ45 jacks on one edge are the dominant horizontal constraint**: 4 × 16.5 mm (body+pitch) + 2× 5 mm edge margin ≈ **76 mm edge minimum** just for the jack row. The per-bus signal chain (shunt → eFuse → INA237 → SN65HVD75 → CMC → TVS → RJ45) needs ~30 mm depth perpendicular to that edge. Add the ESP32-S3 + MAX14830 + power input zone on the opposite half and ~130 × 100 mm falls out naturally with comfortable routing channels.

**Dimensions: 130 mm × 100 mm (13,000 mm²)** — 38 % headroom over the minimum, which covers antenna keep-out (which removes ~350 mm² of usable area), mounting-hole exclusion rings, and sane zone separation. Fits on JLC's "≤ 150 × 150 mm" cheapest tier.

### Layer count: 4

2-layer is infeasible here. Specifically:
- **SMPS + antenna + RS-485 diff pairs + 48 V power + sensitive INA237 Kelvin sensing on one board** demands a continuous uninterrupted GND reference plane. Achievable only with a dedicated GND layer.
- **4× RS-485 A/B differential pairs** need controlled 120 Ω differential impedance routed over a solid reference plane. On 2-layer the reference is swiss-cheesed by the bottom-side traces; impedance control becomes a fiction.
- **USB 2.0 D± (90 Ω differential)** has the same reference-plane requirement.
- **V48 distribution** (up to 6.8 A worst case, 4×1.7 A fault) wants a dedicated power-plane slab or wide poured island; not easy to share a single bottom layer with GND return.
- **LMR36015 switch-node + 2.1 MHz edges** need tight input-loop + GND-return geometry; can't be done cleanly on 2-layer.

4-layer is a ~€30 uplift per board at JLC 5-board prototype tier. Accepted.

6-layer is not justified — no dense BGA (S3 is on a module, MAX14830 is TQFN-32, eFuses are DSBGA-10 but only 10 balls), no high-speed signaling beyond USB 2.0 full-speed.

---

## 2. Stack-up (4-layer, 1.6 mm)

| Layer | Name | Cu wt | Thickness | Purpose |
|---|---|---|---|---|
| L1 | TOP signal | 1 oz (35 µm) | — | All SMD components. Most signal routing. Power-zone top pours (V48, 3V3 islands). |
| — | Prepreg | — | ~0.2 mm | — |
| L2 | GND | 0.5 oz (17 µm) | — | **Solid uninterrupted GND reference.** One and only one split — only to isolate 48 V TVS return if the surge-path analysis (POWER_DESIGN §3a) dictates. Default: no split. |
| — | Core | — | ~1.1 mm | — |
| L3 | PWR (mixed) | 0.5 oz | — | V48 poured island under power zone; 3V3 poured island under MCU/MAX/INA237/SN65HVD75 zone; remainder GND pour stitched to L2 with ≥1 via / cm². |
| — | Prepreg | — | ~0.2 mm | — |
| L4 | BOTTOM signal | 1 oz (35 µm) | — | Short cross-unders only (BGA escape for TPS259827 DSBGA-10, MAX14830 TQFN-32 fan-out, diff-pair crossings). Most of the bottom is GND pour with stitching vias. |

**Why 0.5 oz inner / 1 oz outer:** JLC's standard 4-layer 1.6 mm stack-up; cheapest option. 1 oz outer is required for the V48 current path on TOP (up to 6.8 A) and the buck switch node.

**V48 distribution rule (locked 2026-04-25):** V48 fan-out lives **on L1 (top, 1 oz) only**. L3 inner pour is signal/GND only — no V48 island. Reason: 0.5 oz inner copper carrying 1.7 A per branch under fault leaves only ~10 °C rise margin, which is borderline once layout-dependent voltage drops accumulate. Keeping V48 on L1 sidesteps the inner-layer thermal question entirely.

**Target impedances** (controlled by the layout engineer against JLC's stack-up calculator, tuned at tape-out):
- **100 Ω differential** (single-ended 50 Ω) for USB D± on L1 over L2 GND: approx. 0.22 mm trace / 0.15 mm gap, to be confirmed against JLC's impedance calculator for the exact dielectric chosen.
- **120 Ω differential** for RS-485 A/B on L1 over L2 GND: approx. 0.2 mm trace / 0.3 mm gap, tight pair, to be confirmed.

Surface finish: **ENIG** (gold). Required because DSBGA-10 (TPS259827) soldering is unreliable on HASL — the pads are too small for HASL's thickness variation. ENIG adds ~€5 per 5-board order; worth it.

---

## 3. Floorplan sketch (ASCII, top view)

```
                  ┌─ 100 mm ─┐
   ┌──────────────────────────────────────────────────────────┐  ─┐
   │ J1_DC   [TVS][2ndclamp]  [LM74700][Q1]  [BULK 47µF×2]    │   │
   │ (barrel)                                                  │   │
   │                       [LMR36015][L1][C_OUT]   [MCU-3V3]   │   │
   │  MH1●                                                 MH2●│   │  power / left edge
   │                                                           │   │
   │    ┌─────────────────┐      ┌──────────────────────────┐  │   │
   │    │  ESP32-S3       │      │    MAX14830 + XTAL       │  │   │
   │    │  (antenna KO    │      │                          │  │   │
   │    │   faces ←)      │      │    [BUS_ACT 1 2 3 4]     │  │   │
   │    │                 │      └──────────────────────────┘  │   │  130 mm
   │    │  decap, bulk    │       ↕ SPI                        │   │
   │    └─────────────────┘                                    │   │
   │    [HB LED][PWR_48V][PWR_3V3][FAULT]    [INA237×4 row]    │   │
   │                                                           │   │
   │    [BOOT][RST]       [SWD 2x5]      [USB-C J_USB]          │   │
   │                                                           │   │
   │  MH3●                                                 MH4●│   │
   │                                                           │   │
   │  BUS1     │  BUS2     │  BUS3     │  BUS4                 │   │
   │ [SHUNT]   │ [SHUNT]   │ [SHUNT]   │ [SHUNT]               │   │
   │ [eFuse]   │ [eFuse]   │ [eFuse]   │ [eFuse]               │   │
   │ [INA src] │ [INA src] │ [INA src] │ [INA src]             │   │
   │ [HVD75]   │ [HVD75]   │ [HVD75]   │ [HVD75]               │   │
   │ [CMC+TVS] │ [CMC+TVS] │ [CMC+TVS] │ [CMC+TVS]             │   │
   │ ┌──────┐  │ ┌──────┐  │ ┌──────┐  │ ┌──────┐              │   │
   │ │ RJ45 │  │ │ RJ45 │  │ │ RJ45 │  │ │ RJ45 │              │   │
   │ │  #1  │  │ │  #2  │  │ │  #3  │  │ │  #4  │              │   │
   │ └──┬───┘  │ └──┬───┘  │ └──┬───┘  │ └──┬───┘              │   │
   └────┴──────┴────┴──────┴────┴──────┴────┴──────────────────┘  ─┘
        ▲ front edge: 4× RJ45, cable-exit direction
```

**Edge assignments**
- **Front (bottom in sketch, long edge, 130 mm):** 4× RJ45 in a row. This is the user-facing edge when the unit is rack-mounted or wall-hung; bus cables exit here.
- **Left edge (short, 100 mm):** barrel DC jack. Power cable exits opposite to bus cables so they don't tangle.
- **Right edge (short, 100 mm):** USB-C receptacle. Service port; flush with enclosure edge for easy plug access.
- **Top edge:** no connectors. Hosts the ESP32-S3 module with antenna keep-out extending off-board (see §5).
- **Back (inside):** SWD header + buttons + LEDs arranged mid-board for visibility when cover is off; they do not exit an enclosure edge.

---

## 4. Placement zones

### Zone A — Power input (top-left, ~30 × 30 mm = 900 mm²)

**Contents (revised 2026-04-25):** J1 barrel jack, D1 SMDJ58CA TVS, U1 LM74700-Q1 controller, Q1 SQJ148EP N-MOSFET, LM74700 passives (C_CP charge-pump cap, R_GATE_LM 1 kΩ series, R_GATE_BLEED 10 kΩ), **D_RAIL SMAJ51A on V48_RAIL post-Q1 source**. The pre-2026-04-25 RTH1 inrush NTC and D_SEC SMAJ51A secondary clamp on IC V_IN are deleted (#76 audit).

**Zone shrunk** from 40 × 35 mm to 30 × 30 mm — 500 mm² freed up by the surge-chain simplification.

**Placement constraint:** J1 on the left edge. SMDJ58CA **within 5 mm of J1's + pin** (POWER_DESIGN §3a); its GND return is a short dedicated trace straight to the jack shell GND pad — the TVS surge return **must not** flow through the main board GND pour (it will dump 51 A through the reference plane and crash every IC for the surge duration). Q1's drain-source loop area minimised (POWER_DESIGN §3a layout hint).

**Critical adjacency:** LM74700 within 10 mm of Q1 (gate drive integrity). **D_RAIL within 10 mm of Q1's source pad** — clamps surge tail flowing forward through Q1 body diode.

### Zone B — Buck converter (top-center, ~25 × 30 mm = 750 mm²)

**Contents:** U2 LMR36015FDDA, L1 XAL5030-333, C_IN_BUCK (2.2 µF 1210), C_IN48_HF (4.7 µF 1210), C_IN48_BULK (2× 47 µF AL-poly), C_OUT_BUCK (2× 22 µF 0805), C_SS, EN divider, FB divider.

**Placement constraint:** Switch-node trace (SW pin → L1) **< 10 mm** and kept narrow (POWER_DESIGN §3b). Input MLCC within 3 mm of U2 VIN pin, GND return on same layer direct to PGND pin. L1 oriented so its terminals face SW and VOUT nodes (no jogs).

**Critical adjacency:** Bulk AL-poly caps within 15 mm of U2 VIN; MLCC tight loop to PGND.

**Thermal:** U2 thermal pad → ≥ 4× 0.3 mm vias into L2 GND pour; top-side copper pour ≥ 100 mm² around U2 (POWER_DESIGN §6).

### Zone C — Per-bus cluster (bottom half, ×4, each ~25 × 55 mm = 1375 mm²)

**Contents (one per bus):** R_SHUNT (2512 Kelvin), TPS259827 eFuse (DSBGA-10) + I_LIM, dV/dt, EN pull-down, VIN/VOUT decap, INA237 (VSSOP-10) + filter network + VBUS 10:1 divider + VS decap, SN65HVD75D (SOIC-8), 120 Ω termination, 2× 390 Ω bias, CM choke (SMD), SM712 TVS, RJ45 shielded THT.

**Layout spine (top → bottom / inside → board-edge):**
V48 bus → shunt → eFuse → INA237 sensing → (eFuse VOUT continues as per-bus +48 V going out to RJ45 pins 4/5). RS-485 A/B path runs in parallel: MAX14830 per-channel → SN65HVD75D → CM choke → SM712 TVS → RJ45 pins 3/6. Termination (120 Ω) + bias (2× 390 Ω) at the transceiver end of the choke, between the SN65HVD75D and the CM choke.

**Placement constraint:** **One bus cluster = one column**. Four columns, pitch ~25 mm, align with the four RJ45 footprints on the front edge. Cluster width constrained by RJ45 pitch. No cross-bus signal sharing above the MAX14830 (see §8 diff-pair rules).

**Critical adjacency:**
- R_SHUNT Kelvin taps **matched-length, tight-pair** to INA237 IN+/IN− (POWER_DESIGN §3d).
- SN65HVD75D **within 15 mm** of RJ45 so A/B trace length from transceiver to connector is minimal and well-referenced to GND.
- SM712 TVS **within 5 mm** of RJ45 data pins.
- CM choke between SM712 and RJ45 (TVS clamps before the bus leaves the board).
- RJ45 shield tied **solid** to GND through its mounting tabs (DIGITAL_DESIGN §3f).

**Thermal:** Each eFuse (DSBGA-10) needs a 2×2 thermal-via array under the package (POWER_DESIGN §6). Each shunt (2512) copper minimized beyond the Kelvin taps so IR voltage is measured, not diluted.

### Zone D — MCU (top-center-left, ~40 × 35 mm = 1400 mm²)

**Contents:** ESP32-S3-WROOM-1 module, EN pull-up + RC, BOOT pull-up + debounce, module decoupling (100 nF per VDD + 10 µF + 22 µF bulk), HEARTBEAT LED + R.

**Placement constraint:** Module placed so its **antenna end faces an outer edge of the board** — specifically the **top edge** in the floorplan. Module long axis parallel to the 100 mm edge. No traces, no pour, no components inside the antenna keep-out (see §5). Decoupling caps **on top layer** immediately below each VDD pad on the module.

**Critical adjacency:** SPI lines to MAX14830 (~20 mm run). USB-C on the opposite short edge (right) — route D± diff pair as a single pair along the bottom of Zone D (see §8).

### Zone E — Comms hub (right of MCU, ~25 × 25 mm = 625 mm²)

**Contents:** MAX14830ETJ+ (TQFN-32), 3.6864 MHz crystal, 2× 18 pF load caps, /RESET pull-up, IRQ pull-up, 4× BUS_ACT LEDs + series resistors.

**Placement constraint:** Between MCU (zone D) and the per-bus-cluster row (zone C), so SPI goes east from MCU to hub, and 4× (TX, RX, DE) go south from hub to the four SN65HVD75D transceivers with minimal crossings. Crystal **within 3 mm** of XTAL1/XTAL2 pins; 18 pF caps symmetrical on either side; GND island under crystal tied to MAX14830 GND pin via short trace (not through plane stitching).

**BUS_ACT LEDs** placed along the bottom edge of zone E so they sit roughly above the 4 RJ45 jacks — visually they line up with the bus they indicate.

### Zone F — USB (right edge, ~20 × 25 mm = 500 mm²)

**Contents:** USB-C 16-pin SMD receptacle, USBLC6-2SC6 ESD clamp, 2× 5.1 kΩ CC pull-downs, ferrite bead, 4.7 µF VBUS bulk, shield-bond 0 Ω, hybrid DNP.

**Placement constraint:** Receptacle on the right short edge, flush with board outline. ESD clamp **within 3 mm** of D± pins (before any other component). D± diff pair runs from receptacle west to MCU IO19/IO20 — kept on top layer over solid GND, length matched, <25 mm total (well under USB 2.0 limits).

**Critical adjacency:** VBUS ferrite + cap **within 5 mm** of receptacle VBUS pin. CC pulldowns within 2 mm of CC1/CC2 pins.

### Zone G — Debug (mid-board, ~20 × 15 mm = 300 mm²)

**Contents:** SWD 2×5 1.27 mm header, optional 0 Ω nRESET (DNP), optional 33 Ω series TCK/TDO (DNP), BOOT button, RST button, their debounce caps.

**Placement constraint:** SWD and buttons together, mid-board, accessible with an enclosure cover removed. Not on an edge — they don't exit the enclosure. Button tops 6×6 mm — allow 10 mm clearance in front of each for finger actuation.

### Zone H — LED indicators (mid-board, above RJ45 row, ~30 × 5 mm strip)

**Contents:** PWR_48V (red), PWR_3V3 (green), FAULT (red), HEARTBEAT (blue) in a row. BUS_ACT 1..4 are in zone E above their respective buses.

**Placement constraint:** Row visible through the enclosure cover opening or by pointing the board "label side up" on a bench. Aligned horizontally with silkscreen labels below each LED.

---

## 5. Antenna keep-out

ESP32-S3-WROOM-1's onboard PCB antenna is on the **short end** of the module. Per Espressif's **ESP32-S3-WROOM-1 Hardware Design Guidelines**, the keep-out area must be:

- **Minimum 15 mm × 15 mm** free of copper on **all layers** extending **outward from the antenna end of the module**, with no components, no traces, no via, no pour, no mounting hardware, and no metal enclosure within this volume.
- Espressif recommends the module be placed so the antenna end **protrudes beyond the PCB edge** (module overhangs), or if not possible, the keep-out lives on-PCB.

**Placement decision:** ESP32-S3 module placed in zone D with its antenna end facing the **top edge** of the board. The module footprint sits ~6 mm in from the top edge; the antenna end of the module extends the last ~6 mm toward the top edge. **Keep-out zone: 18 mm (width of module) × 15 mm, on all 4 layers, starting at the antenna end of the module and extending to the top board edge.** This 15 mm zone sits partially on-board (~9 mm) and partially off-board (~6 mm, since we do not overhang — an overhang is fragile). The 9 mm on-board portion is fully copper-free on all layers; no mounting hole, no silkscreen text inside this area (silkscreen is fine but no solder-mask openings or tented vias).

**Cost of keep-out:** ~160 mm² lost to the area budget, already factored into the 130 × 100 dimensioning.

**Mechanical agent flag:** if the enclosure is metal, the metal wall adjacent to the antenna must be **≥ 15 mm away** from the antenna end of the module, or RF performance degrades sharply. See §10.

---

## 6. Mounting holes

- **Count: 4** — one per corner.
- **Diameter: 3.2 mm plated through-hole (M3 clearance).**
- **Annular pad: 6.5 mm OD, unconnected** (isolated island of copper on TOP and BOTTOM that does not connect to the GND pour). Avoids ground-loop issues when the board is bolted to a metal chassis. If chassis-bonding is desired, populate a 0 Ω from the pad to GND on a single corner (the "star-ground" corner) at assembly — leave a DNP pad pattern on all 4.
- **Position (revised 2026-04-25, measured from bottom-left corner of the 130 × 100 board):**
  - MH1: (6, 94)  — top-left
  - MH2: (124, 94) — top-right
  - MH3: (6, 22)   — bottom-left (moved inboard from y=5 to clear the 21 mm RJ45 keep-out)
  - MH4: (124, 22) — bottom-right (same)
- **Exclusion ring: 2 mm** around each hole (no traces, no components). The plated pad itself is 6.5 mm, so the exclusion zone is effectively 8.5 mm diameter. Corner positions give ≥2 mm board-edge clearance on the exclusion rings (was 0.75 mm pre-2026-04-25).
- **No 5th center hole** — the 130 × 100 board is stiff enough at 1.6 mm FR-4 without mid-span support, and there's no heavy THT component mid-board to torque a corner.

---

## 7. Copper-pour strategy

### L1 (TOP)
- **GND pour** in all unused space, stitched to L2 GND with ≥ 1 via / cm².
- **V48 islands** on TOP under zone A → zone B, then branching to four shunt inputs in the per-bus columns (zone C). Width per branch ≥ 2 mm (for 2 A continuous at 1 oz copper, 10 °C rise — standard IPC-2221 table).
- **3V3 island** on TOP under zones D / E plus the INA237 row; not continuous to per-bus transceivers (those get 3V3 via short traces).

### L2 (GND — solid)
- **One uninterrupted plane.** This is the reference for all diff pairs (RS-485 and USB), the MCU decoupling return, the switch-node return for the buck, and the INA237 Kelvin sensing return.
- **No splits** under the diff pair routes — ever.
- **Single conditional split**: TVS surge-return island (a cutout isolating the SMDJ58CA anode pad's return path and tying it directly to the barrel jack's GND shell pin via a dedicated ≥3 mm-wide trace on L1). Only implement this if the layout engineer's surge-path analysis shows the main plane is otherwise impacted. Default: no split; just a direct L1 trace for TVS return, with L2 untouched.

### L3 (PWR — signal/GND only, no V48 pour) — revised 2026-04-25
- **No V48 island on L3.** V48 distribution is L1 only (1 oz top) per stack-up note in §2.
- **3V3 poured slab** under zones D (MCU) and E (MAX14830) and the INA237 row. Connected to L1 3V3 islands with ≥ 1 via / cm².
- **Remainder: GND pour**, stitched to L2 GND via ≥ 1 via / cm² across the whole layer. This gives a second effective GND reference and reduces impedance of the return path for high-frequency signals.

### L4 (BOTTOM)
- **GND pour everywhere** except short cross-under signal segments.
- Stitched to L2 GND with dense via pattern (≥ 2 vias / cm² near high-frequency zones: buck switch node, MCU, MAX14830, USB D±, RS-485 A/B segments).

### Hot-IC thermal pour callouts

| Part | Top-side thermal pour | Thermal vias | Inner-layer connection |
|---|---|---|---|
| U2 LMR36015FDDA | ≥ 100 mm² copper under + around exposed pad | 4× 0.3 mm drill under pad | L2 GND + L4 GND |
| Q1 SQJ148EP (PowerPAK SO-8) | ≥ 100 mm² on drain pad | 4× 0.3 mm under pad | L3 drain island (NOT GND — Q1 drain is a net, pour is for heat spreading on that net) |
| U3..U6 TPS259827 eFuse × 4 | modest pour under package | 2×2 array of 0.3 mm thermal vias per pkg | L2 GND + L4 GND |
| R_SH1..4 shunts (2512) | **minimal pour** — just Kelvin pads | none | N/A (minimize IR loss in the sense path) |
| SMDJ58CA | ≥ 30 mm² pour on cathode pad | 2× 0.3 mm | L2 GND |

### High-voltage clearance rules (V48 nets)

- V48-nominal is 48 V; under surge, up to 93 V momentarily (before the secondary clamp); downstream of the secondary clamp, ≤ 65 V. All nets downstream of the secondary clamp: **≥ 0.2 mm (8 mil) clearance** to signal nets. Upstream of the secondary clamp (TVS node to LM74700 VIN): **≥ 0.4 mm (16 mil) clearance**, IPC-2221 internal-conductor Class 1 for ≤ 100 V.
- No V48 routing under the ESP32-S3 module or its antenna keep-out.
- V48 and GND net clearance on L1 and L3 under the per-bus columns: **≥ 0.3 mm** (headroom).

---

## 8. Critical differential-pair routing rules

### RS-485 A/B (×4 pairs, one per bus)

- **Impedance target: 120 Ω differential.** Tune trace width and intra-pair gap against JLC's stack-up calculator at tape-out (approx 0.2 mm / 0.3 mm for the proposed stack-up; layout engineer confirms).
- **Reference plane: L2 GND, uninterrupted** along the entire path from SN65HVD75D A/B pins → termination + bias network → CM choke → SM712 TVS → RJ45.
- **Length: < 30 mm per pair** (SN65HVD75D is placed within 15 mm of RJ45 per §4 zone C; 30 mm includes the routing through CM choke and TVS).
- **Intra-pair skew: < 1 mm.** RS-485 is half-duplex 3.3 V; bit rate ≤ 1 Mbps in this system, so skew tolerance is generous, but keep it tight to preserve common-mode rejection.
- **Inter-pair spacing: ≥ 3× trace width between adjacent bus pairs.** Four buses in parallel columns are already separated by ~25 mm cluster pitch — ample.
- **No stubs.** Termination (120 Ω) placed directly across A/B at the transceiver side of the CM choke. Fail-safe bias (2× 390 Ω) placed at the same node.
- **Route on L1.** Do not use L4 for diff pairs — L4 is short cross-unders only.

### USB 2.0 D± (single pair)

- **Impedance target: 90 Ω differential** (USB 2.0 spec; single-ended 45 Ω).
- **Reference plane: L2 GND, uninterrupted** from USB-C receptacle → USBLC6-2SC6 → ESP32-S3 IO19/IO20.
- **Length: < 25 mm.**
- **Intra-pair skew: < 0.5 mm.**
- **No vias on the D± pair.** Route entirely on L1 from receptacle to ESD to module.
- **Spacing to other signals: ≥ 3× diff-pair gap (~0.9 mm).** USB D± has no noisy neighbors in this layout since USB is on the right edge and the buck switch node is far away on the top-center.

### Kelvin sense pairs (R_SHUNT → INA237, ×4)

- Matched length within 1 mm.
- Tight pair (intra-pair gap ≤ trace width).
- Route on L1, referenced to L2 GND.
- Do not share reference plane with the buck switch-node area (< 5 mV noise target on the sense line; the INA237 LSB option at 1 mA × 50 mΩ = 50 µV, so plane noise matters).

---

## 9. Silkscreen requirements

**Must-have labels (top side):**

- **RJ45 ports:** `BUS 1`, `BUS 2`, `BUS 3`, `BUS 4` above each jack.
- **RJ45 pin-1 indicator (M6 closed 2026-04-25):** silkscreen `1` next to pin-1 of each RJ45, on the same side of all 4 jacks (e.g. all pin-1 on the left when viewed from the cable-entry face).
- **RJ45 pinout note:** `48V: pin 4/5 (blue) | RS485 A/B: pin 3/6 (green) | GND: pin 7/8 (brown) | T568B STRAIGHT-THROUGH ONLY` — compact text on the silkscreen inside the per-bus column or as a legend block near the jacks. Rationale: MASTER_DECISIONS lock-in is Mode B + T568B (so RS-485 A/B sit on a true twisted pair). A T568A or crossover cable will mis-pair the differential.
- **Barrel jack:** `48V DC` and a `⊕—●—⊖` polarity symbol (center-positive). Also: `60 V MAX` below for over-voltage warning.
- **USB-C:** `USB-C` + small `Service / flash` subtitle.
- **SWD header:** `SWD` + pin-1 triangle marker. Pinout reference card printed near (too dense for the header footprint itself, include on a nearby silkscreen patch).
- **Buttons:** `BOOT` and `RST` labels next to each button.
- **LEDs:** individual labels `48V`, `3V3`, `FAULT`, `HB` (HEARTBEAT), `BUS1`..`BUS4` (activity).
- **Fiducials:** 3× 1 mm copper fiducials (diagonal corners + asymmetric third) for JLC PCBA pick-and-place.
- **Board title block:** top-right corner — `SPLIT-FLAP MASTER v2 REV A`, git SHA placeholder `{{GIT_SHA}}`, date placeholder `{{DATE}}`. **Substitute placeholders at gerber-export time** (CI / KiBot pipeline does this automatically; manual export must do it manually). Documented in fab-handoff checklist.
- **Orientation marker:** `↑ ANTENNA` arrow at the top edge above the ESP32-S3 keep-out, plus `DO NOT ENCLOSE IN METAL WITHIN 15mm` micro-text.
- **Mounting-hole markings:** `M3` next to each corner hole.
- **48 V SELV note:** small text near the barrel jack: `48V SELV — touch-safe; no chassis bond required`.

**Bottom-side silkscreen:** minimal — just a git-SHA re-print and `MADE WITH JLCPCB` credit (optional).

---

## 10. Open issues

- ~~**M3**~~ **Closed 2026-04-25.** Enclosure: designed-to-fit project-box class (Hammond 1455N family or similar). Plastic cover, no chassis bond required (48 V SELV). 4-corner M3 mounting pattern.

- **M4 — Board thickness.** Proposed 1.6 mm (JLC standard, cheapest). 2.0 mm would add stiffness under the 4× RJ45 insertion forces but costs more. Acceptable at 1.6 mm given the 4-corner mounting and the RJ45 jacks' THT leads anchored through the board.

- ~~**M5**~~ **Closed 2026-04-25.** RJ45 shield: solid bond to GND at master only. Backplane and unit shields float. Single bond point eliminates ground-loop hunting across 32 parallel RC paths.

- ~~**M6**~~ **Closed 2026-04-25.** Pin-1 indicator promoted to silkscreen must-have (§9). All 4 jacks have pin-1 on the same side.

- **M7 — Test points.** Layout engineer to sprinkle 1 mm test pads for: each PGOOD (×4), each eFuse VOUT, 3V3, V48, GND (multiple), PROT_FAULT_OR, TELEM_ALERT_OR, SPI CS/MOSI/MISO/SCK. Non-blocking.

- **M8 — Fiducial count and location.** Three fiducials (diagonal corners + one asymmetric) are the JLC PCBA minimum. Layout engineer to place.

- ~~**M9**~~ **Closed 2026-04-25.** Surge-clamp chain simplified: only `D_RAIL` (SMAJ51A) on V48_RAIL within 10 mm of Q1's source pad. R_SERIES + D_SEC chain is deleted (#76 audit).

- **M10 — Overhanging the ESP32-S3 antenna.** Espressif prefers the module overhanging the PCB edge for best RF. Here we do **not** overhang (fragile, ships break during transport). User may override if an injection-moulded enclosure with antenna pocket is used.

---

## 11. Fab-handoff summary (for paste-into-email)

> Master PCB v2 for a split-flap display controller. **130 × 100 mm, 4-layer, 1.6 mm FR-4, ENIG finish, 1 oz outer / 0.5 oz inner.** Stack-up: TOP signal (V48 distribution lives on L1 only — 1 oz copper) / GND solid / PWR (3V3 island + GND, no V48) / BOTTOM signal-plus-GND-pour. Four RJ45 shielded THT jacks on one long edge form the bus-output face; barrel DC jack on one short edge, USB-C on the opposite short edge, SWD + buttons + LEDs mid-board. ESP32-S3-WROOM-1 module is placed with its antenna end at the top edge, requiring an 18 × 15 mm copper-free, component-free, via-free keep-out zone extending to the board edge on all 4 layers. Four M3 mounting holes at corners (6,94)/(124,94)/(6,22)/(124,22), 3.2 mm plated, isolated pads. V48 nets require ≥ 0.2 mm clearance to signal. RS-485 A/B pairs routed as 120 Ω differential over solid L2 GND, USB D± as 90 Ω differential. Per-bus clusters (shunt → eFuse → INA237 → SN65HVD75 → CM choke → TVS → RJ45) are arranged as four parallel columns aligned to the RJ45 footprints. Surge protection: SMDJ58CA at the barrel jack, ideal-diode LM74700-Q1 + Q1 SQJ148EP, single SMAJ51A on V48_RAIL post-Q1 within 10 mm of Q1 source. Thermal vias specified under LMR36015, SQJ148EP, and each TPS259827 eFuse. Silkscreen must call out bus numbering, RJ45 pinout (T568B Mode B: 48 V on pin 4/5, RS-485 A/B on pin 3/6, GND on pin 7/8), pin-1 indicator on every RJ45, "T568B STRAIGHT-THROUGH ONLY" cable warning, LED labels, antenna keep-out warning, 48V SELV note. Electrical design is frozen in `POWER_DESIGN.md` and `DIGITAL_DESIGN.md`; do not substitute parts or change topology. All previously-open issues except M4, M7, M8, M10 are now closed (2026-04-25).
