# Master v2 — ESP32-S3 + ESP32-H2 + TCA9548A carrier PCB

Dedicated controller board for the split-flap display. No onboard flap. Drives up to **8 rows × 16 units = 128 units** via a TCA9548A I2C mux, one I2C bus per row. Carries an ESP32-H2-MINI-1 radio coprocessor for Zigbee 3.0 / Thread / BLE, attached to the primary ESP32-S3 over a UART command/response link. Status LED bar along the front-facing long edge (19 fixed + 16 addressable RGB).

> **Status**: design-stage, capture-ready. This directory is the handoff brief (schematic spec, BOM, pinout, layout constraints). PCB layout is out of scope for this repo — JLCPCB EasyEDA service or a freelance layout engineer takes this brief forward to gerbers.

## Related issues

- [#59](https://github.com/lvschouwen/split-flap/issues/59) — this board
- [#58](https://github.com/lvschouwen/split-flap/issues/58) — ESP32-S3 + ESP32-H2 firmware port (`firmware/MasterS3/`, `firmware/MasterH2/`)
- [#64](https://github.com/lvschouwen/split-flap/issues/64) — LED bar + Row 1..8 convention
- [#55](https://github.com/lvschouwen/split-flap/issues/55) — power distribution (drives input spec)
- [#54](https://github.com/lvschouwen/split-flap/issues/54) — coil de-energize (per-unit firmware, not PCB)

## Conventions

- **Rows are one-indexed: Row 1 .. Row 8.** Silkscreen, web UI, logs, BOM, firmware constants. Matches the one-indexed unit labels (unit 1 .. unit 16, DIP 0000 → unit 1 at I2C 0x01). Internal firmware translates to the TCA9548A's 0..7 channel index via `splitflap::rowToChannelIndex(row) = row - 1`.
- **Capacity: 8 rows × 16 units per row = 128 unit ceiling.**
- **I2C address space per row**: 0x01..0x10 (4-bit DIP offset by I2C_ADDRESS_BASE = 1).
- **Cables**: 4-pin JST XH pitch — 5 V, SDA, SCL, GND — from `J{2+n}` on the carrier to the first unit in Row n. Chained unit-to-unit thereafter.

## Architecture

- **Primary MCU**: **ESP32-S3-WROOM-1-N16R8** (16 MB flash, 8 MB PSRAM, PCB antenna). Owns system: USB, web UI, WiFi, I2C mux, row scheduler, OTA for both MCUs. Boots autonomously.
- **Radio coprocessor**: **ESP32-H2-MINI-1** (RISC-V, 802.15.4, BLE 5, PCB antenna). Slave to the S3. Power-gated by S3 via `H2_RESET_N` (EN, active-low). Does **not** control system state, I2C, or power.
- **Inter-MCU bus**: **UART only** (hard rule). S3's UART1 ↔ H2's UART1 at 921600 bps, 8N1, no hardware flow control. Framing: COBS-encoded frames with 2-byte CRC-16/CCITT-FALSE footer. Model: S3 is master, H2 is responder. Optional `H2_IRQ` line (H2-asserted, active-low, open-drain with 10 kΩ pullup) signals "data available" so S3 doesn't have to poll. **SPI between MCUs is FORBIDDEN — capture-time check.**
- **I2C mux**: TCA9548APWR @ 3.3 V, address 0x70 (A0/A1/A2 → GND). 8 channels, all used.
- **Per-row level shift**: PCA9306DCTR (VSSOP-8) per row, I2C-specific bidirectional translator.
- **Per-row protection**: 2 A polyfuse (1812L200) + SMBJ5.0A TVS on 5 V + PESD3V3L5UW TVS array on SDA/SCL.
- **Power input**: USB-C (5 V, ≤ 1.5 A — logic only) + 5.5/2.1 mm barrel jack (external PSU, up to 8 A — rows + logic backfeed).
- **Power topology**: dual rail. `5V_LOGIC` (USB via LM66100 U2, jack via D_BACKFEED Schottky) feeds the buck. `5V_RAIL` is fed directly from `JACK_5V` and distributes to rows via polyfuses. LM66100 is on the USB path only; high-current jack current never passes through it.
- **3.3 V rail**: **AP63300WU-7** synchronous buck, 3 A capable, 3.8–32 V input, internally compensated + internal soft-start, 0.8 V reference, fixed 500 kHz. Wider V_IN range chosen to leave ~750 mV margin on jack-only operation (worst-case 5V_LOGIC ≈ 4.55 V after Q1 R_DS(on) + SS14 V_F drops).
- **USB protection**: USBLC6-2SC6 ESD array + ferrite bead on VBUS.
- **Board**: **100 × 70 mm, 4-layer**, controlled impedance on USB diff pair.

## Block diagram

```
                 ┌─────────────────────────────────────────────┐
USB-C ──FB1──U1──┤ USB_DP / USB_DN                             │
                 │                                             │
                 │ ┌──────────────────── ESP32-S3-WROOM-1 ───┐ │
                 │ │   GPIO8  I2C SDA  ──── TCA9548A SDA     │ │
                 │ │   GPIO9  I2C SCL  ──── TCA9548A SCL     │ │
                 │ │   GPIO10 MUX_RESET_N ─ TCA9548A /RESET  │ │
                 │ │   GPIO11 H2_RESET_N  ─── H2 EN          │ │
                 │ │   GPIO12 H2_IRQ       ◄── H2 GPIO10     │ │
                 │ │   GPIO13 H2_BOOT_SEL ── H2 GPIO9 (strap)│ │
                 │ │   GPIO17 H2_UART_TX  ─── H2 GPIO4 (RX)  │ │
                 │ │   GPIO18 H2_UART_RX  ◄── H2 GPIO5 (TX)  │ │
                 │ │   GPIO19/20 USB D-/D+                   │ │
                 │ │   GPIO43/44 U0TXD/U0RXD ─── JP_DEBUG_S3 │ │
                 │ └──────────────────┬──────────────────────┘ │
                 │                    │ I2C (3V3)               │
                 │          ┌─────────▼──────────┐              │
                 │          │    TCA9548A (U6)   │              │
                 │          │   8× mux channels  │              │
                 │          └───┬───┬──────┬─────┘              │
                 │              │   │      │                    │
                 │          ┌───▼───▼──────▼─────┐              │
                 │          │  8× PCA9306 (U7..)│               │
                 │          │  3V3 ↔ 5V level    │              │
                 │          │     translators    │              │
                 │          └───┬───┬──────┬─────┘              │
                 │              │   │      │                    │
                 │          ROW1 ROW2 .. ROW8 (XH-4)            │
                 │                                             │
                 │ ┌──────────── ESP32-H2-MINI-1 ───────────┐  │
                 │ │   Zigbee 3.0 / Thread / BLE 5           │ │
                 │ │   UART1 ↔ S3 (921600 8N1, command/rsp) │ │
                 │ │   GPIO23/24 U0TXD/U0RXD → JP_DEBUG_H2   │ │
                 │ │   GPIO26/27 USB D-/D+ → test pads       │ │
                 │ └────────────────────────────────────────┘  │
                 └─────────────────────────────────────────────┘

Power tree:
  USB-C VBUS ── FB1 ── U2 LM66100 ── 5V_LOGIC ──┬── U4 AP63300 buck ── 3V3_RAIL
                                                │
                                    D_BACKFEED SS14 (anode on JACK_5V)
                                                │
  Barrel jack ── Q1 ── D1 ── JACK_5V ───────────┴──────── direct ──────── 5V_RAIL ──┬── F1..F8 ── ROW1..8
                                                                                    │
                                                      per row: PCA9306 + PESD + SMBJ

L2 GND plane uninterrupted. L3 = 5V_RAIL pour + 3V3 island under MCUs.
```

## Key specs

| Item | Value |
|---|---|
| Primary MCU | ESP32-S3-WROOM-1-N16R8 |
| Radio coprocessor | ESP32-H2-MINI-1-N4 |
| Inter-MCU link | UART1, 921600 bps, 8N1, no flow control, COBS + CRC-16 |
| I2C mux | TCA9548APWR @ 3.3 V, addr 0x70 |
| Rows | 8 × XH-4 (5 V / GND / SDA / SCL) |
| Level shift | PCA9306DCTR (VSSOP-8) per row |
| Per-row protection | 2 A polyfuse + SMBJ5.0A TVS + PESD3V3L5UW |
| Power input | USB-C 5 V (logic) + 5.5/2.1 mm barrel jack 5 V (rows + logic backfeed) |
| Logic OR-ing | LM66100DCKR × 1 (USB path only, **not** in high-current path) |
| Jack-to-logic backfeed | SS14 Schottky, 1 A |
| 3.3 V | AP63300WU-7 synchronous buck, 3 A, 3.8–32 V V_IN, internally compensated, 500 kHz |
| Rails | 5V_LOGIC (buck input) + 5V_RAIL (rows) — distinct nets |
| USB ESD | USBLC6-2SC6 + BLM18PG471 ferrite |
| Buttons | BOOT_S3 (S3 GPIO0), RESET_S3 (S3 EN) |
| LED bar | 19 fixed + 16 addressable RGB, long-edge front-facing. 14 fixed driven by TLC5947DAP (S3 SPI IO4/5/6/7), 3 fixed driven by H2 GPIO11/12/13, 2 rail indicators always-on, 16 SK6812 mini on a single S3 data line (IO21). |
| Debug | Native USB (S3) + JP_DEBUG_S3 1×4 UART + JP_DEBUG_H2 1×6 UART+EN+BOOT + H2 USB D± test pads |
| Board | 100 × 70 mm, 4-layer, 1.6 mm FR4 |
| Input current ceiling | 8 A total (input spec) |

## Bulk capacitance summary

| Net | Total bulk | Breakdown |
|---|---|---|
| JACK_5V (input) | **480 µF** | 470 µF Al-polymer + 10 µF X7R |
| 5V_RAIL (row) | **267 µF + HF** | 220 µF Al-polymer + 47 µF X5R + 100 nF |
| 5V_LOGIC (buck input) | **32 µF** | 22 µF X5R (buck C_IN) + 10 µF X7R (U2 OUT) |
| 3V3_RAIL (logic) | **76 µF** | 2× 22 µF X5R (buck out) + 22 µF X5R (S3 bulk) + 10 µF X7R (H2 bulk) + local HF pairs |
| Per-row (at connector) | **≥ 47 µF + 100 nF** | C_ROW{n} 47 µF X5R 1210 + C_ROW{n}_HF 100 nF 0603 |

Per-row capacitance meets the ≥ 47 µF hard requirement.

## Stackup — 4-layer (decision + justification)

**Choice: 4-layer.** Rejected 6-layer.

**Justification:**
- Only two positive power rails (5 V, 3.3 V) + GND. 6-layer adds a second GND or a third power layer that this design does not need.
- No high-speed DDR/HBM interconnect — PSRAM is integrated inside the S3 module.
- Single controlled-impedance pair (USB D+/D−) routes cleanly on L1 over an uninterrupted L2 GND.
- Two 2.4 GHz antennas — both module-integrated with vendor-validated keep-outs. No on-board RF traces requiring stripline.
- Density: S3 module + H2 module + TCA9548A + 8× PCA9306 + connectors + passives fits comfortably on L1 (top) with spillover to L4 (bottom). L3 power-plane splits (5V_RAIL / 3V3 island) are the only routing constraint on the inner layer and do not require a second signal layer.
- Cost: 6-layer roughly doubles fabrication cost at JLCPCB for no measurable benefit here.

**Stackup (JLCPCB `JLC04161H-7628` or equivalent, 1.6 mm FR4, Tg 155):**

| Layer | Copper | Function |
|---|---|---|
| L1 (top) | 1 oz | **Signals + components.** All IC placement. USB diff pair. I2C trunk. Local decoupling routing. SW node for buck. |
| L2 (inner 1) | 0.5 oz | **Solid GND plane.** Uninterrupted — no splits, no cutouts (except antenna keep-outs). Primary return reference for USB diff pair and buck SW node. |
| L3 (inner 2) | 0.5 oz | **Power distribution.** 5V_RAIL solid pour (row power). Carved 3V3 island under both MCU modules + mux + PCA9306 VREF1 pins. 5V_LOGIC sub-island around buck input. |
| L4 (bottom) | 1 oz | **Secondary signals + GND stitch.** UART link between MCUs, miscellaneous control signals, H2 debug breakout. Unused areas flooded GND, stitched to L2 on 5 mm grid. |

Dielectric: 0.3 mm prepreg L1–L2, 1.065 mm core L2–L3, 0.3 mm prepreg L3–L4. Dk 4.4 @ 1 GHz.

Controlled impedance: **90 Ω differential** on L1 over L2 for USB D+/D−. Trace/space 0.18 / 0.13 mm. Specify impedance test coupon on fabrication order.

## RF constraints (numeric, mandatory)

Both modules carry their own PCB antennas and operate in the 2.4 GHz ISM band. **Vague wording is a FAIL condition. All values below are hard rules.**

### ESP32-S3-WROOM-1 antenna

- Keep-out region: **15 mm × 7 mm** from the antenna end of the module.
- **No copper** on any of the 4 layers inside the keep-out — L1, L2, L3, L4 all cleared. This breaks the L2 GND plane locally; stitching must route *around* the keep-out, not through.
- **No traces** routed under the keep-out on any layer.
- **No ground flooding** inside the keep-out on any layer.
- Module orientation: antenna edge faces **PCB top edge** (long side of board).

### ESP32-H2-MINI-1 antenna

- Keep-out region: **11 mm × 6 mm** from the antenna end of the module.
- Same rules as S3 keep-out: no copper on any layer, no traces underneath, no flood.
- Module orientation: antenna edge faces **PCB bottom edge** (opposite edge from S3 antenna) — maximum physical separation.

### Inter-module RF isolation

- **Minimum antenna-to-antenna edge distance: 30 mm.** Measured between the nearest points of the two keep-out regions.
- Modules on opposite board edges (S3 top, H2 bottom): ~60 mm achievable on a 70 mm-tall board. Exceeds the 30 mm floor by 2×.
- No common-mode coupling path: both modules share GND on L2 (required) but with uninterrupted plane between them.
- **Coexistence note** (firmware, not PCB): S3 WiFi/BT and H2 802.15.4/BLE both occupy 2.4 GHz. Hardware does not arbitrate. Firmware MUST implement TDM — typical approach: S3 operates on WiFi channel 1 (2401–2423 MHz), H2 pinned to 802.15.4 channel 25 or 26 (2475–2480 MHz) to avoid spectral overlap. This is an application-layer constraint; the PCB does not preclude it.

### Antenna placement — REQUIRED orientation

```
┌──────────────────── top edge ─────────────────┐
│  ▓▓▓▓▓▓▓  ← S3 antenna keep-out (15×7)       │
│  ┌─────────────┐                              │
│  │  ESP32-S3   │                              │
│  └─────────────┘                              │
│                                               │
│     (remainder of components)                 │
│                                               │
│  ┌───────────┐                                │
│  │ ESP32-H2  │                                │
│  └───────────┘                                │
│  ▓▓▓▓▓  ← H2 antenna keep-out (11×6)          │
└──────────────── bottom edge ──────────────────┘
```

## Firmware constraints

The board electrically allows illegal operating modes; firmware MUST enforce:

- **Single-channel-at-a-time mux selection** (TCA9548A). Channel-select register writes must have exactly one bit set: 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, or 0x80. Multi-bit masks parallel-combine per-row pullups and violate ATmega328P I_OL (3 mA max).
- **Pre-flight mux select guard** rejects non-power-of-two values.
- **MUX_RESET_N pulse** (≥ 10 µs low) during I2C bus-stuck recovery.
- Row-aware probe: iterate (row 1..8, address 0x01..0x10) = 128 slots max.
- **H2 power sequencing**: on S3 boot, hold H2_RESET_N low for ≥ 10 ms, ensure H2_BOOT_SEL high (normal SPI boot), then release H2_RESET_N. Wait ≥ 100 ms for H2 firmware to start. First UART transaction is a hello/version handshake.
- **H2 OTA**: S3 is the authoritative firmware provider. To reflash H2, S3 pulls H2_BOOT_SEL low, pulses H2_RESET_N, and drives the esptool-compatible UART protocol at 921600 bps to write new H2 firmware to H2's SPI flash. Then S3 releases H2_BOOT_SEL and H2_RESET_N for normal boot.
- **UART arbitration**: S3 is always master. H2 only transmits in response to a command, or when it asserts H2_IRQ first and S3 replies with a POLL command.
- **2.4 GHz coexistence**: at minimum, pin H2 to 802.15.4 channel 25 or 26 and S3 WiFi to channel 1. Do not run H2 BLE scanning concurrently with WiFi high-throughput transfers.

## Bill of materials

[BOM.csv](./BOM.csv). JLCPCB PCBA-ready. **All entries have committed LCSC part numbers — no VERIFY stubs.** LCSC stock is order-time; substitutions (same part, different manufacturer) are acceptable if package and electricals match.

## Schematic

- [SCHEMATIC.md](./SCHEMATIC.md) — authoritative textual netlist.
- [master_v2.kicad_sch](./master_v2.kicad_sch) — minimal KiCad container. Layout engineer recaptures from SCHEMATIC.md.

## Pinout

[PINOUT.md](./PINOUT.md) — full ESP32-S3 and ESP32-H2 GPIO tables, strap-pin rules, reserved pins.

## Handoff workflow (mandatory, sequential)

1. **Schematic capture** in target EDA from SCHEMATIC.md + BOM.csv. ERC must pass with zero errors and zero warnings.
2. **Pre-layout review checklist** (all 8 signed off before routing starts):
   1. PCA9306DCTR pin numbering verified against TI SCPS169 Table 3 (VSSOP-8: 1 EN, 2 VREF1, 3 SDA1, 4 GND, 5 SCL1, 6 SCL2, 7 VREF2, 8 SDA2).
   2. TCA9548APWR pin numbering verified against TI SCPS195 (pin 1 A0, pin 4 /RESET, pin 12 GND, pin 22 SCL, pin 23 SDA, pin 24 VCC; channel pin mapping per Table 5-1).
   3. USBLC6-2SC6 routing confirmed: pins 1+6 = IO1 (tie to one D-line), pins 3+4 = IO2 (tie to other D-line), pin 5 = VBUS, pin 2 = GND. **Pin 4 is NOT ground.**
   4. AP63300WU-7 feedback divider: R_FB_TOP 100 kΩ + R_FB_BOT 31.6 kΩ → V_OUT 3.34 V ±1 % (V_FB = 0.803 V typical). C_FB 22 pF present across R_FB_TOP. C_BOOT 100 nF between pins 6 (BST) and 1 (SW).
   5. ESP32-S3 strap pins: GPIO0 drives only BOOT_S3_BTN. GPIO3, GPIO45, GPIO46 have no copper escape beyond module pad.
   6. ESP32-H2-MINI-1 strap pins: IO2, IO8 tied high via 10 kΩ to 3V3. IO9 pulled high via 10 kΩ to 3V3 with 1 kΩ series from S3 GPIO13 for optional pull-low (download mode).
   7. Inter-MCU link is **UART only**. No SPI nets between S3 and H2. No shared I2C. ERC must confirm `H2_UART_TX`, `H2_UART_RX`, `H2_RESET_N`, `H2_IRQ`, `H2_BOOT_SEL` are the only signal nets crossing between modules.
   8. Antenna keep-outs (S3 15×7 mm, H2 11×6 mm) clear on all 4 layers; modules oriented on opposite board edges with ≥ 30 mm antenna edge-to-edge separation.
3. **Placement review** against §"Layout constraints" below.
4. **Routing review** against §"Routing constraints" below.
5. **DRC + impedance report** delivered with gerbers.

## Layout constraints

### Grounding
- L2 is the single GND reference. Uninterrupted, no splits.
- Every IC GND pin stitches to L2 with ≥ 2 vias.
- L4 bottom pour is GND, stitched to L2 on a 5 mm grid via array.
- Antenna keep-outs (15 × 7 mm under S3 antenna, 11 × 6 mm under H2 antenna): no copper on any of the 4 layers.

### High-current return
- Row connector GND pins → direct line-of-sight through L2 back to barrel-jack GND lug and USB-C shield.
- No row-return currents route beneath either MCU module or the antenna keep-outs.

### 5 V distribution (dual rail)
- **5V_RAIL** (row power): L3 solid power pour, fed from JACK_5V direct. Polyfuses F1..F8 break the pour into per-row outbound islands.
- **5V_LOGIC** (buck input): separate L3 island or dedicated L1 trace ≥ 1.0 mm wide, fed by U2 LM66100 output and D_BACKFEED cathode. Does not connect to 5V_RAIL anywhere on the board except via D_BACKFEED Schottky (anode side on JACK_5V).
- **3V3 island** on L3 beneath both MCU modules + TCA9548A + PCA9306 VREF1 pins; fed from buck output via ≥ 1.5 mm L1 trace through two vias.

### Buck (U4 AP63300WU-7 + L_BUCK)
- C_BUCK_IN within 2 mm of U4 VIN pin on 5V_LOGIC net.
- L_BUCK within 4 mm of U4 SW pin.
- SW trace: L1 only, ≤ 6 mm length, ≤ 3 mm² area.
- L2 GND under SW node uninterrupted (no L2 cutouts under U4).
- C_BUCK_OUT_1 within 3 mm of L_BUCK output; C_BUCK_OUT_2 within 5 mm.
- Feedback divider within 5 mm of U4 FB pin; FB trace flanked by GND pour on L1.
- No signal routing under L_BUCK or under SW node on any layer.
- U4 SW pin / thermal copper: ≥ 50 mm² copper area on L1 connected to the SW pad plus GND pour. Stitch vias (≥ 6 × 0.3 mm) from top-layer GND pour under U4 down to L2.

### USB and USB-C connector
- USBLC6-2SC6 placement: **≤ 3 mm from USB-C D+/D− pins**. No trace reaches the ESP32-S3 side before passing U1.
- D+/D− differential pair: 90 Ω on L1 referenced to L2, width 0.18 mm, spacing 0.13 mm, length-matched ±0.2 mm, max length 50 mm from U1 to ESP32-S3 GPIO19/20.
- VBUS ferrite bead FB1 within 5 mm of connector VBUS pin.
- Cable shield tied to GND through a 1 MΩ resistor in parallel with a 10 nF cap (chassis path).

### Inter-MCU UART
- `H2_UART_TX`, `H2_UART_RX`, `H2_RESET_N`, `H2_IRQ`, `H2_BOOT_SEL`: route primarily on L4 referenced to L2 GND. Max length 80 mm (corner-to-corner on 100 × 70 mm board).
- Keep ≥ 5 mm lateral clearance from the buck SW node trace and ≥ 3 mm from the USB diff pair.
- Series damping resistors (R_H2_RST_SER 1 kΩ, R_H2_BOOT_SER 1 kΩ) on S3 side, within 5 mm of S3 module pad.

### I2C routing
- ESP32-S3 → TCA9548A trunk (SDA/SCL on L1): ≤ 30 mm.
- TCA9548A channel n → PCA9306 U(6+n) 3V3 side: ≤ 40 mm on L1.
- PCA9306 5V side → XH-4 connector pin: ≤ 15 mm on L1.
- Bypass jumpers JP_BYPASS_SDA / JP_BYPASS_SCL (0 Ω DNP) between ESP32-S3 bus and TCA9548A channel 0 outputs.

### Per-row protection
- TVS D_ROW{n} within 5 mm of J{n} pin 1 (5 V).
- PESD3V3L5UW within 5 mm of J{n} pins 3/4 (SDA/SCL).
- C_ROW{n} (47 µF 1210) and C_ROW{n}_HF (100 nF 0603) within 5 mm of J{n} power pins.
- Polyfuse F{n} in series in the J{n} 5 V path; ≥ 1.0 mm L1 bridge to L3 island.

### Strap pins and module
- S3 GPIO0 trace to BOOT_S3_BTN ≤ 20 mm, flanked by GND pour.
- S3 GPIO3, GPIO45, GPIO46: no copper escape beyond module pad on any layer.
- H2 IO2, IO8: 10 kΩ pullup to 3V3 within 5 mm of module pad.
- H2 IO9: 10 kΩ pullup to 3V3 within 5 mm of module pad, series 1 kΩ from S3 GPIO13.

### Test points
- 1.0 mm exposed round pad, silkscreen labels per PINOUT.md and SCHEMATIC.md §test-points.
- TP_MUX_SDA1..8 and TP_MUX_SCL1..8 arranged in a linear row adjacent to TCA9548A for scope probe access.
- TP_H2_USB_DP / TP_H2_USB_DN: 1 mm pads near H2 module for optional USB-JTAG access (wire-soldered pigtail or pogo jig).

### DRC
- Min trace/space: 0.15 / 0.15 mm.
- Via: 0.3 mm drill / 0.6 mm pad.
- Minimum annular ring: 0.05 mm.

## Manufacturing

- Service: JLCPCB PCBA, 4-layer FR4 Tg 155, ENIG finish.
- Assembly: top side only. Through-hole connectors (barrel jack, XH-4, pin headers) hand-solder or JLCPCB extended-THT service.
- Controlled impedance: request 90 Ω differential test coupon with the fabrication order.

## Bring-up checklist

**Pre-power**:
1. Visual inspection — solder bridges, polarity of TVS, TCA9548A pin 1, PCA9306 pin 1, S3 module pin 1, H2 module pin 1.
2. Continuity: USB VBUS ↔ GND open; 5V_RAIL ↔ GND open; 3V3_RAIL ↔ GND open.
3. Barrel-jack reverse polarity: apply reversed 5 V, confirm no current draw (PMOS Q1 off).
4. JP_BYPASS_SDA, JP_BYPASS_SCL DNP — unpopulated for first power-on.

**Power-up, no firmware**:
1. Apply 5 V barrel. Measure at TP_5V = 5.0 V ±5 %. Measure at TP_3V3 = 3.33 V ±3 %.
2. Scope U4 SW node: ripple ≤ 150 mV pk-pk at ~500 kHz (AP63300 fixed frequency).
3. Measure 5 V at each row connector pin 1 under no load.
4. USB-C only: confirm CDC device enumerates on host.

**Firmware bring-up**:
1. Flash blinky on S3 GPIO4. Confirm blink.
2. I2C scan at TP_MCU_SDA/SCL returns 0x70 (TCA9548A), no other address with rows unplugged.
3. Probe JP_DEBUG with logic analyzer — transaction patterns match ESP32 I2C API.
4. For each row (1..8), single-channel select, scan. Probe TP_MUX_SDA{n}/SCL{n} (3.3 V logic) and TP_ROW{n}_SDA/SCL (5 V logic). Both show identical ACK pattern; rise time ≤ 1 µs.
5. H2 bring-up: flash H2 factory image via JP_DEBUG_H2 with external USB-UART dongle (3V3, GND, U0TXD, U0RXD, EN, BOOT_SEL). Confirm H2 boots and prints ROM banner on U0TXD at 115200 8N1.
6. S3 ↔ H2 link: run handshake probe. S3 asserts H2_RESET_N low 10 ms, releases, waits 100 ms, sends `{0x7E, 0x01, 0x00, CRC_H, CRC_L, 0x00}` (COBS-framed PING). H2 replies with PONG frame. TP on S3 GPIO17/18 shows 921600 bps traffic in both directions.

**Hardware validation**:
1. Hot-unplug a row cable under 100 mA load. Scope TP_ROW{n}_5V: clamp peak ≤ 9.2 V (SMBJ5.0A spec).
2. Thermal soak U4 at 500 mA load, 10 min. U4 package ≤ 85 °C at 40 °C ambient (IR probe).
3. Simultaneous 16-unit step command on one row. TP_5V droop ≤ 200 mV pk, recovery ≤ 2 ms. Droop > 500 mV indicates bulk cap ESR out of spec.
4. Firmware guard test: writing 0x03 (two-channel enable) to mux is refused by firmware; scope TP_MCU_SDA confirms no slave ACK to illegal write.
5. H2 radio: run Zigbee scan on H2, S3 relays results over UART, log to S3 serial. Confirm neighbour PAN discovery.
6. Coexistence: run WiFi iperf on S3 concurrent with Zigbee ping on H2. Both must sustain traffic; monitor for packet loss correlation.

**Mux-bypass diagnostic** (only when diagnosing mux failure):
1. Power off. Populate JP_BYPASS_SDA and JP_BYPASS_SCL (0 Ω).
2. Pull MUX_RESET_N low via test clip (holds TCA9548A in reset / high-Z).
3. Plug chain into ROW1 only. Power on.
4. ESP32-S3 I2C scan at TP_ROW1_SDA/SCL returns 0x01..0x10. Validates MCU → PCA9306 → unit chain independent of mux.
5. Restore: remove jumpers, release MUX_RESET_N.
