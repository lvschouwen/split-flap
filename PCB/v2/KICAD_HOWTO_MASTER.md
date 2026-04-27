# KiCad 10 Howto — Master PCB (Beginner)

**Revision:** 2026-04-27
**Audience:** You've finished `KICAD_HOWTO_BUS.md` AND
`KICAD_HOWTO_UNIT.md`. The KiCad workflow is now familiar; this
howto focuses on what's **different** about the Master, not what's
the same.
**Time:** 12-20 hours start to Gerbers. This is the most complex of
the three boards.
**Companion to:** `KICAD_GETTING_STARTED.md`, `SCHEMATIC_MASTER.md`
(491 lines — read it twice), `LAYOUT_MASTER.md`.

The Master is **~100 × 80 mm**, **1 per system**. It carries the
ESP32-S3 + 4 RS-485 buses + USB-C + 12 V/15 A input + 4 row outputs.
The number-one risk is the **antenna keep-out**; the number-two risk
is the **15 A power path**.

---

## What's new vs. Bus / Unit

| Concern | Bus had it? | Unit had it? | Master has it |
|---|---|---|---|
| Single MCU schematic | yes (none) | yes | yes (ESP32-S3 module) |
| Multi-page or hierarchical schematic | no | no | **strongly recommended** — the schematic is too dense for one page |
| RF antenna keep-out | no | no | **YES, critical** (18.6 × 5 mm, both layers) |
| 15 A power path requiring poured copper | no | no | **YES** (J1 → F1 → Q1 → C_bulk → branch point) |
| Differential pair (USB) | no | no | yes — D+/D- |
| Multiple-instance routing (4× identical RS-485 path) | no | no | yes — 4 row sub-circuits |
| SPI bus to UART expander | no | no | yes (ESP32-S3 ↔ SC16IS740) |
| 12 V/3.3 V/5 V (USB-VBUS) coexistence | no (just 12 V passthrough) | yes (12 V + 3.3 V) | yes (12 V + 3.3 V + USB-VBUS isolated) |
| Strap-pin pulls on a module | no | no | yes (ESP32-S3 IO0/3/45/46/EN) |

If anything in the right column is unclear, re-read its `LAYOUT_MASTER.md`
section before opening KiCad.

---

## Step 0 — Project + multi-page schematic setup

Create `PCB/v2/kicad/master/splitflap-master-v2`. Same procedure as
the Unit board.

For a board this size, **use hierarchical sheets** in Eeschema so the
schematic doesn't become a wall of crossing wires. From the root
sheet:

1. **Place → Add Hierarchical Sheet** (or right-click → Add Sheet).
2. Draw a rectangle. KiCad asks for a sheet name and a filename.
   Suggested split:
   - `power.kicad_sch` (J1, F1, D8, Q1, Z1, R_q1g, R_q1g2, C_bulk,
     U2 K7803, C_in, C_out, the 4 polyfuses + C_row + row PWR LEDs)
   - `mcu.kicad_sch` (U1 ESP32-S3 module, all decoupling, all strap
     pulls, BOOT/RST switches, EN cap)
   - `usb.kicad_sch` (J2 USB-C, U8 USBLC6, R_cc ×2, C_usb)
   - `uart_expander.kicad_sch` (U3 SC16IS740, Y1 crystal, C_xtal ×2,
     IRQ pull, RESET pull, mode-sel/CTS ties)
   - `bus0_rs485.kicad_sch` (U4 PHY, R_term, R_bias_a/b, D9 ESD,
     C_decap_phy, C_row, F_row, R_led_row, J3 connector — entire
     bus 0 sub-circuit)
   - **Copy `bus0_rs485.kicad_sch` 3 more times** as `bus1_*`,
     `bus2_*`, `bus3_*`. Tweak only the connector designator and net
     names.
3. Each sheet has its own page. **Hierarchical labels** (`Place →
   Add Hierarchical Label`) are how nets cross sheet boundaries.
   Top-level nets (`+12V_RAIL`, `+3V3`, `GND`, `UART0_TX`, etc.) get
   hierarchical labels in each sheet, and the parent sheet wires
   them between child sheets.

This isn't strictly required — you can do everything on one big
page — but the hierarchical layout makes the 4× RS-485 repetition
trivially copy-paste-able and the ERC errors much easier to localize.

If you'd rather flat-page, that's also fine; just be prepared for
~50 net labels and a lot of horizontal wire-crossing.

---

## Step 1 — Schematic capture by sheet

For each sheet, follow the pattern from `KICAD_HOWTO_UNIT.md` § Step
1: place symbols, set values, add LCSC fields, wire with net labels.

### Sheet: `power`

Components from `SCHEMATIC_MASTER.md` § Power section:
- J1 (KF7.62 2-pin screw terminal — 15 A version), F1 (15 A 5×20
  fuse holder THT), D8 (SMAJ15A SMA), Q1 (AOD409 DPAK P-FET),
  Z1 (BZT52C10 SOD-123), R_q1g (100 Ω), R_q1g2 (10 kΩ),
  C_bulk (470 µF/25 V radial THT), U2 (K7803-1000R3 SIP-3 buck),
  C_in (10 µF/25 V/1206), C_out (×2 10 µF/10 V/0805),
  4× F_row (Bourns MF-LSMF400/16X-2 polyfuse 2920),
  4× C_row (10 µF/25 V/0805),
  4× D4-D7 (row PWR LEDs 0805 green),
  4× R_led_row (4.7 kΩ — **NOT** 1 kΩ; 12 V drop would burn the LED).

Net labels you'll need to expose hierarchically:
- `+12V_RAIL` (post-Q1 source — the master logic 12V rail)
- `ROW0_12V`, `ROW1_12V`, `ROW2_12V`, `ROW3_12V` (post-polyfuse)
- `+3V3`
- `GND`
- `Q1_GATE` (only used inside this sheet, no need to expose)

#### Custom symbol for K7803-1000R3

Same procedure as drawing the SM712 symbol in the Unit howto:

1. Symbol Editor → New Symbol `K7803-1000R3` in `splitflap-custom`.
2. Body rectangle.
3. Pin 1: `VIN`, type Power input.
4. Pin 2: `GND`, type Power input.
5. Pin 3: `VOUT`, type Power output.
6. Default footprint: `Package_TO_SOT_THT:TO-220-3_Vertical` (or
   any L78xx SIP-3 footprint that matches the K7803 mechanical
   datasheet). **Verify the pin order matches** — K7803 pin layout
   per Mornsun datasheet should be VIN/GND/VOUT left-to-right when
   viewed from the labeled face.

### Sheet: `mcu`

- U1 ESP32-S3-WROOM-1 module symbol (`RF_Module:ESP32-S3-WROOM-1`).
- Decoupling caps: 8× 100 nF/0603 (one within 3 mm of each module
  Vcc pin).
- Strap pulls: see `LAYOUT_MASTER.md` § Strap pin pulls table.
  - IO0 (BOOT): 10 kΩ pull-up to 3V3, plus SW1 pull-down to GND.
  - IO3: 10 kΩ pull-up to 3V3.
  - IO46: **explicit external 10 kΩ pull-down to GND.** Don't skip.
  - EN: 10 kΩ pull-up to 3V3, plus SW2 pull-down to GND, plus 1 nF
    cap from EN to GND (debounce reset).
- BOOT and RST switches are 6×6 SMD tact (`Switch:SW_Push`).

Hierarchical-label exposed nets (all module signal pins that talk
to other sheets):
- `+3V3`, `GND` (power)
- USB: `USB_DP`, `USB_DM` (IO19, IO20)
- SPI to expander: `SPI_CS` (IO10), `SPI_MOSI` (IO11), `SPI_SCK`
  (IO12), `SPI_MISO` (IO13), `SPI_IRQ` (IO9)
- UART0 (Bus 0): `UART0_TX` (IO43), `UART0_RX` (IO44), `UART0_DE`
  (IO1), `UART0_RE` (IO2)
- UART1 (Bus 1): `UART1_TX` (IO17), `UART1_RX` (IO18), `UART1_DE`
  (IO16), `UART1_RE` (IO15)
- UART2 (Bus 2): `UART2_TX` (IO5), `UART2_RX` (IO6), `UART2_DE`
  (IO7), `UART2_RE` (IO4)

### Sheet: `usb`

- J2 USB-C 16-pin SMD receptacle (`Connector_USB:USB_C_Receptacle_USB2.0_16P`).
- U8 USBLC6-2SC6 (`Power_Protection:USBLC6-2SC6`).
- 2× R_cc (5.1 kΩ/0603) — CC1 and CC2 each pulled down to GND.
- C_usb (1 µF/0603) on VBUS line near USBLC6 pin 5.
- Shield → chassis GND (single point).

Hierarchical: `USB_DP`, `USB_DM` to MCU sheet.

**Do NOT route VBUS to +3V3 or +12V_RAIL** — USB is for
programming/debug only; the board is powered by J1.

### Sheet: `uart_expander`

- U3 SC16IS740IPW (custom symbol — see below).
- Y1 14.7456 MHz crystal (`Device:Crystal`,
  `Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm`).
- 2× C_xtal 18 pF NP0 (`Capacitor_SMD:C_0603_1608Metric`).
- C_decap_uart (single 100 nF — pin 1 is the only VDD).
- R for IRQ pull-up (10 kΩ).
- R for RESET pull-up (10 kΩ — RESET is **active-LOW**, so HIGH means
  out-of-reset).
- Tie pin 8 (mode-sel) and pin 11 (CTS) directly to GND.

#### Custom symbol for SC16IS740IPW

Same drill as before:

1. Symbol Editor → New Symbol `SC16IS740IPW` in `splitflap-custom`.
2. Body rectangle.
3. 16 pins per the NXP datasheet `SC16IS740_750_760.pdf` Figure 5
   + Table 4. The locked pinout per `SCHEMATIC_MASTER.md` §
   SC16IS740:
   | Pin | Name | Type | Connection |
   |---|---|---|---|
   | 1 | VDD | Power input | +3V3 (with 100 nF decap) |
   | 2 | /CS | Input | SPI_CS from MCU |
   | 3 | SI | Input | SPI_MOSI |
   | 4 | SO | Output | SPI_MISO |
   | 5 | SCLK | Input | SPI_SCK |
   | 6 | NC | NC | — |
   | 7 | IRQ | Output (open-drain) | SPI_IRQ → MCU (with 10 kΩ pull-up to 3V3) |
   | 8 | I²C/SPI | Input | **GND (lock SPI mode)** |
   | 9 | GND | Power input | GND |
   | 10 | RTS | Output | U7 PHY pin 3 (DE) — auto-RS-485 mode |
   | 11 | CTS | Input | **GND (input pin can't float)** |
   | 12 | TX | Output | U7 PHY pin 4 (D) |
   | 13 | RX | Input | U7 PHY pin 1 (R) |
   | 14 | RESET | Input | **+3V3 via 10 kΩ (active-LOW)** |
   | 15 | XTAL2 | Output | Y1 + 18 pF C_xtal to GND |
   | 16 | XTAL1 | Input | Y1 + 18 pF C_xtal to GND |
4. Default footprint: `Package_SO:TSSOP-16_4.4x5mm_P0.65mm`.

Hierarchical exposed nets: `SPI_CS`, `SPI_MOSI`, `SPI_MISO`,
`SPI_SCK`, `SPI_IRQ`, `UART3_TX`, `UART3_RX`, `UART3_DE`.

(UART3_TX = SC16IS740 pin 12 → U7 D; UART3_RX = U7 R → SC16IS740
pin 13; UART3_DE = SC16IS740 pin 10 → U7 DE.)

### Sheets: `bus0_rs485`, `bus1_rs485`, `bus2_rs485`, `bus3_rs485`

Build bus 0 first, then copy. Each has:
- U4 (or U5/U6/U7) SN65HVD75DR (`Interface_UART:SN65HVD75D` or
  generic 8-pin RS-485 transceiver symbol).
- R_term 120 Ω across A/B (0805).
- R_bias_a 1 kΩ from A to 3V3 (0805).
- R_bias_b 1 kΩ from B to GND (0805).
- D9 (or D10/D11/D12) SM712-02HTG ESD (custom symbol from Unit howto).
- F_row (Bourns 2920 polyfuse) on the row's 12V branch.
- C_row 10 µF/0805 on the row's 12V branch.
- D4 (D5/D6/D7) row PWR LED + R_led_row 4.7 kΩ.
- C_decap_phy 100 nF/0603 within 3 mm of pin 8.
- J3 (J4/J5/J6) JST-VH 4-pin male THT (`Connector_JST:JST_VH_B4P-VH-A`).

Net wiring:
- PHY pin 1 (R) ← `UART0_RX` (or UART1/2/3_RX for the other buses).
- PHY pin 2 (/RE) ← `UART0_RE` (or UART1/2_RE for buses 1-2). For
  Bus 3 (PHY U7), **pin 2 (/RE) tied to GND** (always-receive); the
  SC16IS740 only has RTS for DE.
- PHY pin 3 (DE) ← `UART0_DE` (or UART1/2/3_DE) via 1 kΩ R_de.
- PHY pin 4 (D) ← `UART0_TX` (or UART1/2/3_TX).
- PHY pin 5 → GND.
- PHY pin 6 (A) → R_term + R_bias_a + D9 pin 1 + connector pin 2.
- PHY pin 7 (B) → R_term other side + R_bias_b + D9 pin 3 +
  connector pin 3.
- PHY pin 8 (Vcc) → +3V3 (with C_decap_phy).

Connector pinout (J3-J6):
- Pin 1: ROW0_12V (or ROW1/2/3_12V).
- Pin 2: A.
- Pin 3: B.
- Pin 4: GND.

### Add PWR_FLAGs

Add `PWR_FLAG` symbols on `+12V_RAIL`, `+3V3`, `GND` (driven by J1
incoming, U2 buck output, and J1 incoming respectively). Without
these, ERC complains "power input pin not driven".

### Run ERC

`Inspect → Electrical Rules Checker` from the root sheet. KiCad
checks all sheets together. Fix every error. The hierarchical setup
makes it much easier — errors are reported per sheet.

Common errors specific to Master:
- **"Power input pin not driven"** on the ESP32-S3 module's many
  Vcc pins → ensure the +3V3 hierarchical label is wired to all
  module Vcc pins.
- **"Pin not connected"** on unused module GPIOs → place
  No-Connect flags (`Q`) on every NC GPIO. There are many. Or
  silence with No-Connect at the parent sheet level.
- **"Multiple PWR_FLAGs on the same net"** → fine, just consolidate.

When ERC is clean, save and switch to PCB Editor.

---

## Step 2 — Footprint assignment

`Tools → Footprint Assignment Tool` from any sheet. Critical
checks:

| Symbol | Footprint | Trap |
|---|---|---|
| U1 ESP32-S3 | `RF_Module:ESP32-S3-WROOM-1` | KiCad's footprint includes the antenna keep-out as a courtyard but doesn't enforce no-copper inside. We'll add a Rule Area in step 5. |
| U2 K7803 | `Package_TO_SOT_THT:TO-220-3_Vertical` | **Verify pin order matches K7803 datasheet** (VIN/GND/VOUT left-to-right). The TO-220 footprint may need a custom variant if the K7803's mechanical drawing differs. |
| U3 SC16IS740 | `Package_SO:TSSOP-16_4.4x5mm_P0.65mm` | Don't use SOIC-16 or SSOP-16 — TSSOP has 0.65 mm pitch. |
| U4-U7 SN65HVD75 | `Package_SO:SOIC-8_3.9x4.9mm_P1.27mm` | Standard SOIC-8 narrow (150 mil). |
| U8 USBLC6 | `Package_TO_SOT_SMD:SOT-23-6` | SOT-23-6, not SOT-23-3. |
| Q1 AOD409 | `Package_TO_SOT_SMD:TO-252-2` | DPAK / TO-252. Larger than the unit's AO3401. |
| F1 fuse holder | (no stock footprint — custom or omit and use through-pad with appropriate spacing) | 5×20 mm THT holder; many vendors. |
| F_row 2920 polyfuse | `Resistor_SMD:R_2920_7461Metric` (use as a placeholder — verify pad geometry against Bourns MF-LSMF400/16X-2 datasheet) | **Big package — reserve real estate.** |
| J1 KF7.62 screw terminal | `TerminalBlock:TerminalBlock_*_P7.62mm` | Verify against actual MPN datasheet. |
| J2 USB-C | `Connector_USB:USB_C_Receptacle_GCT_USB4085` | Or any GCT/Amphenol-compatible 16-pin SMD. |
| J3-J6 JST-VH 4-pin | `Connector_JST:JST_VH_B4P-VH-A` | Same as Bus board. |
| Y1 crystal | `Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm` | 4-pin SMD crystal — 2 of the pins are mechanical (case ground). |

---

## Step 3 — Open Pcbnew, Board Setup

Same procedure as Bus / Unit. Use the standard JLC values.

Press `F8` to "Update PCB from Schematic". A big cluster of
footprints appears. Don't move any yet.

---

## Step 4 — Board outline + mounting holes

1. Outline rectangle on Edge.Cuts: **(0, 0) to (100, 80)** mm.
2. Mounting holes (4× M3 NPTH): (4, 4), (96, 4), (4, 76), (96, 76).

The exact long-axis dimension can vary slightly (95-100 mm) depending
on how everything fits — leave room for the antenna keep-out
projection past the module's PCB edge. Plan the module placement
first (step 5), then trim or extend the outline.

---

## Step 5 — Place the ESP32-S3 module + antenna keep-out FIRST

**This is the highest-stakes placement decision on the board.** The
module's antenna sticks past one short edge; that edge needs free
PCB area extending 18.6 × 5 mm (per Espressif WROOM-1 datasheet
§ 7.2) with **no copper, no vias, no components**.

1. Find U1 in the cluster (search by reference). Press `M`.
2. Place the module so its antenna end faces an outer edge of the
   board. Suggested: top-right corner, antenna pointing right,
   so the keep-out projects off the right edge.
3. The module's silk usually shows an antenna outline — orient
   accordingly.

### Add the keep-out as a KiCad Rule Area

1. **Place → Add Rule Area** (sometimes labeled "Add Keepout
   Zone").
2. The Rule Area properties dialog opens.
3. **Layer**: tick BOTH `F.Cu` AND `B.Cu` (apply to both layers).
4. ☑ Keep out tracks
5. ☑ Keep out vias
6. ☑ Keep out pads
7. ☑ Keep out copper pour
8. ☑ Keep out footprints
9. Click OK.
10. Draw the rectangle: 18.6 mm × 5 mm, **adjacent to the antenna
    end of the module**, extending OUTSIDE the module footprint
    onto the bare PCB area beyond the module's edge.
11. After drawing, the Rule Area shows as a hatched zone. Verify it
    covers the correct geometry.

> **The keep-out is 18.6 mm × 5 mm extending PAST the module edge,
> not under the module.** The module's own antenna keep-out is
> built into its footprint; what we add is the EXTRA keep-out on
> the bare PCB beyond the module.

If the keep-out cuts into the board edge — that's fine, it can hang
off the edge into nothing. KiCad doesn't care if a Rule Area extends
beyond Edge.Cuts (the manufactured PCB just stops at the cut).

### Silk outline of the keep-out

Add a silk rectangle on F.Silkscreen matching the keep-out so anyone
inspecting the board can see where copper must not go. Helpful for
future revs.

---

## Step 6 — Place J1 (input) + the 15 A power cluster

1. **J1 screw terminal** — top edge, somewhere convenient for the
   power cable to land. (5, 8) is fine.
2. Right next to J1 (within 10 mm): **F1 fuse holder**. The fuse
   should be physically between J1 and the rest of the power
   network — if the cable has a short, F1 is the first thing that
   sees it.
3. **D8 SMAJ15A TVS** — within 5 mm of F1's downstream side.
4. **Q1 AOD409** — next.
5. **C_bulk** (470 µF radial THT) — within 10 mm of Q1's source pin.
6. **Z1 BZT52C10, R_q1g, R_q1g2** — clustered around Q1's gate pin.

The whole cluster (J1 → C_bulk) wants to fit in roughly a 30 × 30 mm
area in one corner.

---

## Step 7 — Place U2 K7803 buck

Near C_bulk, with a Kelvin tap from the input rail (a separate trace
from the per-row branch point). This way, motor inrush on a row
doesn't sag the master logic 3V3.

Place C_in (10 µF/1206) within 3 mm of U2's pin 1 (VIN).
Place C_out (×2 10 µF/0805) within 3 mm of U2's pin 3 (VOUT).

---

## Step 8 — Place USB-C + USBLC6 + buttons

USB-C J2 on the top edge near the corner opposite J1 (so the USB
cable doesn't fight the power cable).

U8 USBLC6 within 3 mm of J2 on the D+/D- nets.

SW1 (BOOT, IO0) and SW2 (RST, EN) near J2 for ergonomic access
during programming.

---

## Step 9 — Place U3 SC16IS740 + crystal

Close to the ESP32-S3 module's SPI pins (IO10-IO13). U3 within
~50 mm of the module.

Y1 crystal within 3 mm of U3 pins 15/16. The 2 GND case pins of
the SMD crystal connect directly to GND pour on the bottom layer.

---

## Step 10 — Place 4 PHYs + per-row clusters

Lay out the 4 RS-485 sub-circuits as 4 vertical "stripes" along the
bottom half of the board, each ~20 mm wide:

```
+-------+-------+-------+-------+
| PHY0  | PHY1  | PHY2  | PHY3  |
| F_row | F_row | F_row | F_row |
| C_row | C_row | C_row | C_row |
| R_term| R_term| R_term| R_term|
| R_bias| R_bias| R_bias| R_bias|
| D_ESD | D_ESD | D_ESD | D_ESD |
| LED   | LED   | LED   | LED   |
+-------+-------+-------+-------+
| J3    | J4    | J5    | J6    |  ← bottom edge
+-------+-------+-------+-------+
```

Each stripe owns one row connector at the bottom edge. The
JST-VH 4-pin connectors face downward (cable exit toward the
chassis).

---

## Step 11 — Place strap pulls + decoupling

Strap pulls (10 kΩ) within 3 mm of each ESP32-S3 strap pin.
Decoupling caps (100 nF) within 3 mm of every Vcc/VDD pin on
every chip.

This is tedious but mechanical. Put them in.

---

## Step 12 — Routing — do power FIRST as filled zones

### The 15 A path is a polygon pour, not a track

1. Layer: `F.Cu`. Net: `+12V_RAIL` (post-Q1 source — this is the
   post-fuse, post-FET, post-bulk-cap rail that distributes to U2
   and the 4 polyfuses).
2. **Place → Add Filled Zone**. Layer F.Cu. Net `+12V_RAIL`.
3. Draw a polygon covering the area from Q1 source to C_bulk to
   the branch point that splits to U2 input and the 4 polyfuses.
4. Repeat on `B.Cu` with the same net — same outline.
5. **Stitch the two pours with vias**: place vias every ~10 mm
   along the pour, all on the +12V_RAIL net. KiCad auto-detects
   the net from the pour underneath.
6. Repeat the same procedure for the **pre-Q1 segment** (J1 → F1
   → D8 cathode → Q1 drain) — this is a *different net*
   (`+12V_INPUT`) so it gets its own zone, also poured on both
   layers and stitched.

The two zones (`+12V_INPUT` and `+12V_RAIL`) meet at Q1 — they're
electrically isolated by the FET except when the FET is conducting.

### GND is also a zone, both layers

7. **Place → Add Filled Zone**. Layer F.Cu. Net `GND`. Draw a
   polygon covering the entire board minus a 0.5 mm clearance to
   Edge.Cuts and minus the antenna keep-out.
8. Repeat on B.Cu.
9. Stitch with vias every ~10 mm.

### 3V3 is also a zone (preferred) or a wide trace

10. From U2 pin 3 (VOUT), pour a `+3V3` zone on B.Cu (or F.Cu,
    whichever has clearer space) covering the area near the
    ESP32-S3, SC16IS740, and 4 PHYs.

### Per-row ROW0_12V → ROW3_12V

11. From each polyfuse output, route a 120-mil trace (or pour) to
    the corresponding row connector pin 1.

### Press `B` to fill all zones

`B` triggers a global zone refill. Verify with `View → 3D Viewer`
that the copper plane has no breaks across the 15 A path.

---

## Step 13 — Routing — signals

Now route all the signal nets per `LAYOUT_MASTER.md` § Signal
routing. Order:

1. **USB D+/D-** as a differential pair (`6` hotkey for diff-pair
   route). 10 mil width, 10 mil spacing.
2. **SPI bus** (CS/MOSI/SCK/MISO + IRQ) — 6 mil traces, length
   under 50 mm.
3. **UART0/1/2** native ESP32-S3 → PHY0/1/2 — 6 mil signal traces.
4. **UART3** SC16IS740 → PHY3 — 6 mil. RTS line goes to PHY3 DE
   pin (auto-RS-485 mode); /RE is **tied to GND directly on the
   PCB**, NOT driven by RTS.
5. **RS-485 A/B** at each row connector — coupled diff pair, 10
   mil. SM712 ESD between PHY and connector.
6. **Strap pulls + EN cap + BOOT/RST switches** — 6 mil traces, all
   short.

### Order tip: route the most-constrained nets first

Diff pairs (USB, RS-485) are constrained by impedance and routing
geometry. Power is constrained by current. Signal traces are the
most flexible — route last so they fill in around the constraints.

---

## Step 14 — Verify the antenna keep-out is HOLY

After all routing, **visually inspect** the antenna keep-out region
on both layers:
1. Hide all layers except F.Cu and Edge.Cuts. Zoom into the
   keep-out. Should be empty.
2. Hide F.Cu, show B.Cu. Same check.
3. Re-show all layers. Run DRC.

DRC will flag any track / via / pad inside the keep-out as a
violation. **Do not skip this check.** The autorouter (if you used
one) and even manual moves can break the keep-out silently.

---

## Step 15 — DRC + 3D viewer

1. Press `B` (refill zones).
2. **Inspect → Design Rules Checker**. Fix every error.
3. **View → 3D Viewer**. Things to verify:
   - Antenna keep-out: no copper visible in that region on either
     layer.
   - 4 row connectors at the bottom edge, all facing the same way.
   - USB-C and BOOT/RST buttons accessible from the top edge.
   - F1 fuse holder and J1 screw terminal physically clear of the
     module footprint.
   - C_bulk (radial THT) doesn't collide with any neighboring SMD
     part — radial THT is tall.
   - K7803 SIP-3 module orientation matches the silk pin labels.

---

## Step 16 — Plot Gerbers

`KICAD_GETTING_STARTED.md` § 9. Include F.Paste / B.Paste if you're
doing JLC PCBA (master is mostly SMT; assembly is reasonable for 5
boards but you can also hand-solder).

---

## Step 17 — JLC order

| Spec | Value |
|---|---|
| Quantity | 5 (1 system + 4 spares — risk hedging since this is the most expensive board) |
| Layers | 2 |
| Material | FR-4 |
| Thickness | 1.6 mm |
| Surface finish | HASL (no pogos here) |
| Copper weight | **1 oz both sides** |
| Estimated cost | ~EUR 8-12 per board at qty 5 (~EUR 40-60 total) |

---

## Verification before clicking "Place Order"

Open `LAYOUT_MASTER.md` § "Verification before Gerber" — every
checkbox. The killers:

- [ ] **Antenna keep-out 18.6 × 5 mm present on BOTH layers**,
      extending past the module's PCB edge, with **no copper / vias
      / pads / components inside**.
- [ ] **U2 is K7803-1000R3 (1 A version)**, NOT K7803-500R3.
- [ ] **Z1 is BZT52C10 (10 V), NOT 12 V.**
- [ ] **F_row polyfuses are 2920 SMD package**, NOT 1812.
- [ ] **D8 SMAJ15A is post-F1 (not pre-fuse)**, cathode → +12V.
- [ ] **15 A power path is filled zone both layers**, stitched
      with vias. NOT narrow tracks.
- [ ] **J1 is ≥15 A screw terminal** (KF7.62-2P or DECA MA231),
      NOT Phoenix MKDS 1.5/2.
- [ ] **Row connectors J3-J6 are JST-VH 4-pin** (B4P-VH-A,
      C144392), NOT JST-XH.
- [ ] **SC16IS740: pin 8 GND, pin 11 GND, pin 14 → 3V3 via 10 kΩ.**
- [ ] **ESP32-S3 IO46 has explicit 10 kΩ pull-down to GND.**
- [ ] **USB-C VBUS NOT tied to +3V3 or +12V_RAIL.**
- [ ] **U7 (Bus 3 PHY) /RE tied to GND, DE driven by SC16IS740 RTS
      via 1 kΩ.** RTS does NOT drive both DE and /RE.
- [ ] **Per-row PWR LEDs use 4.7 kΩ resistor**, NOT 1 kΩ.
- [ ] **C_in is 1206**, not 0805.
- [ ] DRC clean.

---

## What's harder than the Unit board

- **Hierarchical schematic** is new but pays off — every change to
  the bus0 sub-circuit can be replicated to bus1/2/3 by careful
  copy-paste.
- **The antenna keep-out** is the single most consequential physical
  constraint on the board. Treat it as load-bearing.
- **Polygon pours instead of tracks for power.** Set `+12V_RAIL`,
  `+12V_INPUT`, `GND`, and `+3V3` as pour-zone-driven nets — don't
  try to route them as tracks.
- **Strap pin pulls on a module** are easy to forget. Walk through
  the strap-pin checklist while looking at the module footprint
  and verify each pull resistor is physically present on the PCB.
- **The 3D viewer is your friend** — many of the placement
  collisions on this board (radial caps, fuse holder, SIP-3
  module) are obvious in 3D and invisible in 2D.

---

## After all 3 boards: what to do next

1. **Order all 3 PCBs from JLC** — they'll arrive together in
   1-2 weeks.
2. **Hand-solder the Bus board first** (just connectors and 4
   traces — easy first build).
3. **Test pogo contact** on a single Unit + Bus pair before
   committing to 64 units.
4. **Power up the Master** with the lab supply current-limited to
   2 A first (just the master logic, no rows). Verify 3V3 is
   stable and ESP32-S3 boots.
5. **Connect one row** at a time, walk up to 4 rows of 16 units.
   Watch the input current with a clamp meter — peak should stay
   under 12 A with master firmware staggering steps. (See locked
   decision #10 in `KICAD_HANDOFF.md`.)

If something doesn't work on first power-up, the SWD test pads on
the Unit boards and the BOOT/RST buttons on the Master are your
debug points. The verification checklists in each `LAYOUT_*.md` file
catch ~95% of typical fab-time mistakes when reviewed honestly.

Good luck!
