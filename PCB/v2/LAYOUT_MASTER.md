# Layout Guide — Master PCB

**Revision:** 2026-04-26
**Companion to:** `SCHEMATIC_MASTER.md` (nets, MPNs, pin maps).
**Read this when:** placing components and routing in KiCad 10. See
`KICAD_HANDOFF.md` for the tool-specific workflow (project setup,
libraries, plot/Gerber, JLC integration).

Most complex of the three boards. ESP32-S3 module + 4 RS-485 PHYs +
SC16IS740 expander + USB-C + 12 V/15 A power input + 4 row outputs.

---

## Board outline

| Spec | Value |
|---|---|
| Outline | **~100 × 80 mm** rectangle |
| Layers | 2 |
| Substrate | FR-4 |
| Thickness | 1.6 mm |
| Copper | **1 oz both sides** — 15 A power path needs poured copper, not narrow tracks |
| Surface finish | HASL |
| Solder mask | Green |
| Silk screen | White |
| Mounting | 4× M3 clearance at corners |

Origin: top-left corner. X = long axis (100 mm), Y = short axis (80 mm).

## ⚠️ CRITICAL constraints — do not violate

### 1. ESP32-S3-WROOM-1 antenna keep-out

**18.6 mm × 5 mm rectangle** extending past the module's PCB edge,
under the antenna area. Per Espressif WROOM-1 datasheet § 7.2:

- **No copper** on either layer inside this rectangle (no traces,
  no pours, no fills, no thieving).
- **No vias** inside the rectangle.
- **No components** inside or directly under it.
- The rectangle extends OUTSIDE the module footprint, off the
  module edge. Plan the master PCB outline so this rectangle has
  free PCB area to project into.

**The module's antenna is its WiFi antenna.** Violating the keep-out
silently kills RF range — the board still works on a bench but
performs badly in a chassis. Do not skip.

### 2. Power input traces are POURED COPPER, not tracks

15 A on 1 oz copper requires **~325 mil of trace OR a polygon pour**.
Pour is shorter, lower R, less ΔT. Replaces an earlier "60 mil track"
which violated 1 oz current capacity by 3×.

| Net | Geometry |
|---|---|
| J1 → F1 → Q1 → C_bulk (12V input, **15 A**) | **Polygon pour on BOTH layers**, stitched with via fence (≥6 vias 0.6 mm drill, 1.0 mm pad) at every layer transition; minimum local width 250 mil where pour necking is unavoidable |
| C_bulk → per-row polyfuses (12V trunk, ~5 A peak per branch) | **Polygon pour preferred**; if traced, ≥150 mil |
| Polyfuse → row connector (4 A per row) | ≥120 mil, polygon pour preferred |
| 3V3 to ESP32-S3 + PHYs + SC16IS740 | 20 mil |
| Signal | 6 mil |
| RS-485 A/B differential | 10 mil routed as a loose pair |

### 3. SC16IS740 RESET pin is active-LOW

Pin 14 RESET pulled to 3V3 via 10 kΩ for normal operation.
**Do NOT tie to GND** — that holds the chip in permanent reset.
(Gemini external review claimed active-HIGH; confirmed wrong vs.
NXP datasheet 2026-04-26.)

### 4. ESP32-S3 IO46 needs an explicit external pull-down

Strapping pin must be LOW at boot. **Add an explicit 10 kΩ external
pull-down to GND on IO46.** Do NOT rely on the internal pull-down —
not always reliable across reset.

## Mounting holes

4× M3 clearance at the 4 corners of ~100 × 80 mm board, ~4 mm in
from each edge:

| Hole | Position |
|---|---|
| TL | (4, 4) |
| TR | (96, 4) |
| BL | (4, 76) |
| BR | (96, 76) |

Drill 3.2 mm, NPTH.

## Component placement zones

```
                      ~100 mm long axis (X)
   ─────────────────────────────────────────────────────────
  ┌──○────────────────────────────────────────────────○──┐
  │ J1     USB-C J2    SW1 BOOT   SW2 RST                 │
  │ 12V/15A             on IO0      on EN                 │
  │ screw                                                 │
  │ terminal                                              │
  │                                                       │
  │  D8 TVS    ┌─────────────────────────┐                │ ~80 mm
  │  Q1 AOD409 │  U1 ESP32-S3-WROOM-1   │                │ short
  │  Z1 + R    │  N16R8 module           │                │ axis
  │  C_bulk    │  ←  ANTENNA KEEP-OUT  → │                │ (Y)
  │  470µF     │  18.6×5mm rectangle     │                │
  │  F1 fuse   │  past PCB edge          │                │
  │  U2 K7803  └─────────────────────────┘                │
  │  -1000R3                                              │
  │  buck      U8 USBLC6 (USB-C ESD)                      │
  │                                                       │
  │  U3 SC16IS740 + Y1 14.7456 MHz crystal                │
  │  (close to ESP32-S3 SPI pins)                         │
  │                                                       │
  │  U4 PHY0    U5 PHY1    U6 PHY2    U7 PHY3             │
  │  R_term0    R_term1    R_term2    R_term3             │
  │  R_bias_a/b R_bias     R_bias     R_bias              │
  │  D9 ESD     D10 ESD    D11 ESD    D12 ESD             │
  │  F_row1     F_row2     F_row3     F_row4              │
  │  C_row      C_row      C_row      C_row               │
  │  D4 row LED D5 row LED D6 row LED D7 row LED          │
  │                                                       │
  │  J3 row 0   J4 row 1   J5 row 2   J6 row 3            │ <-- BOTTOM edge
  │  JST-VH 4p  JST-VH 4p  JST-VH 4p  JST-VH 4p           │
  ├──○────────────────────────────────────────────────○──┤
   ─────────────────────────────────────────────────────────
   ○ = M3 mounting hole (corners)
```

## Component placement order

1. **Place mounting holes first** — outline anchors.
2. **Place ESP32-S3 module roughly central**, antenna pointing
   toward an edge of the PCB. Mark the **18.6 × 5 mm antenna
   keep-out rectangle** as a KiCad **Rule Area** (Place → Add Rule
   Area, with "Keep out tracks / vias / pads / copper / footprints"
   enabled) on BOTH layers, extending past the module's PCB edge.
3. **Place J1 screw terminal** on the top edge (input side). The
   high-current cable from the brick wants to land here.
4. **Place F1 fuse holder, D8 TVS, Q1 P-FET, C_bulk** in a tight
   cluster near J1. The 15 A path is short and wide.
5. **Place U2 K7803-1000R3 buck module** near C_bulk, with a Kelvin
   tap from the input rail (separate trace from the per-row
   branch point). Master logic 3V3 stays stable during row inrush.
6. **Place USB-C J2 + USBLC6 U8** on the top edge. USB-C cable
   exits upward away from the row outputs.
7. **Place SW1 (BOOT) and SW2 (RST)** near the USB-C for user
   access during programming.
8. **Place U3 SC16IS740 + Y1 crystal** close to the ESP32-S3 SPI
   pins (IO10–IO13). Crystal within 3 mm of pins 15/16.
9. **Place 4 RS-485 PHYs (U4–U7) in a row** along the lower section,
   each near its corresponding row connector at the bottom edge.
10. **Place 4 row connectors J3–J6** along the bottom edge.
    JST-VH 4-pin (B4P-VH-A, C144392), pin 1 facing INWARD so the
    cable strain relief is from below.
11. **Place per-row protection (F_row, C_row, row LED, ESD, term,
    bias)** between each PHY and its row connector.
12. **Distribute decoupling caps** within 3 mm of every Vcc/VDD pin.

## Power routing

### 12 V input rail (J1 → F1 → Q1 → C_bulk → distribution)

```
J1 pin 1 (+) ──┬── F1 (15 A slow-blow, 5×20 mm fuse holder THT)
                │
                ├── D8 SMAJ15A: cathode (banded) → +12V (post-fuse), anode → GND
                │   Place AFTER F1 so a sustained TVS clamp opens the fuse.
                │   Polarity reversed = forward-bias = dead short.
                │
                └── Q1 drain (AOD409 P-FET reverse-block, DPAK)

Q1 source → PCB-12V_RAIL (load side):
  - C_bulk 470 µF radial THT (post-Q1 source, single bulk cap)
  - Z1 BZT52C10 10V Zener: cathode → source/+12V_RAIL, anode → gate node
  - R_q1g 100 Ω gate series, R_q1g2 10 kΩ gate pull-down to GND
  - Trunk to per-row polyfuses
  - Kelvin tap to U2 K7803-1000R3 input

PCB-12V_RAIL → per-row branches (×4):
  ├── F_row1 polyfuse → ROW0_12V → C_row 10 µF → J3 pin 1 + D4 row PWR LED via R_led_row 4.7k
  ├── F_row2 polyfuse → ROW1_12V → C_row 10 µF → J4 pin 1 + D5 row PWR LED via R_led_row 4.7k
  ├── F_row3 polyfuse → ROW2_12V → C_row 10 µF → J5 pin 1 + D6 row PWR LED via R_led_row 4.7k
  └── F_row4 polyfuse → ROW3_12V → C_row 10 µF → J6 pin 1 + D7 row PWR LED via R_led_row 4.7k
```

**Polygon pour both layers** for the J1 → F1 → Q1 → C_bulk → branch
point segment. Stitch with vias every ~10 mm. The 12V pour and GND
pour together form a coplanar power plane — keep their edges
parallel for low loop inductance into C_bulk.

### 3V3 rail (U2 buck output)

- U2 buck VOUT → 3V3 net.
- C_in 10 µF / 25 V (1206 X7R) at U2 input pin.
- 2× C_out 10 µF / 10 V (0805 X7R) at U2 output pin.
- Distribute 3V3 to: ESP32-S3 module pin 2, SC16IS740 pin 1, all
  4 SN65HVD75 pin 8, all R_strap pull-ups, USB-C R_cc pull-downs,
  USBLC6 if needed.
- 20 mil minimum trace; pour preferred.

### GND plane

- Solid GND pour both layers, stitched with vias every ~10 mm.
- Single-point GND tie from the master power section to the row
  output GND pins.
- Continuous GND pour between transceivers for crosstalk rejection.

## Signal routing

### USB-C (J2) ↔ USBLC6 (U8) ↔ ESP32-S3 (IO19/IO20)

- D+/D- as a **differential pair**, ~10 mil trace, ~10 mil spacing.
- USBLC6 within 3 mm of the USB receptacle on the D+/D- nets.
- CC1, CC2 each have 5.1 kΩ to GND (R_cc) within 3 mm of the
  receptacle.
- Shield → chassis GND (single-point).
- USB-C VBUS routed to USBLC6 pin 5 only (1 µF C_usb decap). **Do NOT
  tie VBUS to 3V3 or to the +12V_RAIL.** Master is powered from J1.

### SPI (ESP32-S3 ↔ SC16IS740)

- 4 nets: /CS (IO10 → U3 pin 2), MOSI (IO11 → U3 pin 3), SCK
  (IO12 → U3 pin 5), MISO (IO13 ← U3 pin 4).
- 6 mil signal traces. Keep the SPI bus length under ~50 mm.
- IRQ (IO9 ← U3 pin 7): 10 kΩ pull-up to 3V3 (open-drain output).
- **U3 pin 8 (I²C/SPI mode select) tied to GND** — locked for SPI.
- **U3 pin 11 (CTS) tied to GND** — input pin, can't float.
- **U3 pin 14 (RESET) pulled HIGH to 3V3 via 10 kΩ R_uart_strap**
  (RESET is active-LOW per NXP datasheet; high = chip out of reset).
- Y1 crystal + 2× 18 pF C_xtal on U3 pins 15/16, within 3 mm.

### UART0 (ESP32-S3 native, Bus 0)

- IO43 (TX) → U4 pin 4 (D), IO44 (RX) ← U4 pin 1 (R), IO1 (DE) →
  U4 pin 3 via 1 kΩ R_de, IO2 (/RE) → U4 pin 2 via 1 kΩ R_re.
- 6 mil signal traces.

### UART1 (Bus 1) and UART2 (Bus 2)

- UART1: IO17 TX, IO18 RX, IO16 DE, IO15 /RE → U5 transceiver.
- UART2: IO5 TX, IO6 RX, IO7 DE, IO4 /RE → U6 transceiver.
- 6 mil signal traces.

### SC16IS740 → U7 (Bus 3) auto-DE wiring

- SC16IS740 pin 12 (TX) → U7 pin 4 (D).
- SC16IS740 pin 13 (RX) ← U7 pin 1 (R).
- SC16IS740 pin 10 (RTS) → U7 pin 3 (DE) — auto-RS-485 mode (EFCR
  bit 4 = 1) drives DE during TX.
- U7 pin 2 (/RE) tied permanently to GND (always-receive; firmware
  discards TX echo). **Do NOT tie /RE to RTS** — DE and /RE are
  opposite polarity, one signal can't drive both correctly.

### RS-485 A/B (per bus, ×4 identical)

- A/B as a **differential pair**, 10 mil trace, 10 mil spacing.
- 120 Ω termination R_term across A/B at the master end.
- 1 kΩ R_bias_a from A to 3V3, 1 kΩ R_bias_b from B to GND
  (idle bias).
- SM712-02HTG ESD across A/B near the row connector.
- Connect A → row connector pin 2, B → row connector pin 3.

### Strap pin pulls (ESP32-S3 module)

Place each pull within 3 mm of its corresponding module pin:

| Pin | Pull | To | Notes |
|---|---|---|---|
| IO0 (BOOT) | 10 kΩ | 3V3 | + SW1 to GND |
| IO3 (USB-JTAG enable) | 10 kΩ | 3V3 | |
| IO45 | none | — | module-internal default |
| **IO46** | **10 kΩ** | **GND** | **Explicit external pull-down — do not skip.** |
| EN | 10 kΩ | 3V3 | + SW2 to GND + 1 nF cap to GND |

## Layer assignments summary

| Layer | What goes here |
|---|---|
| Top | All components (no bottom-side parts on master), ESP32-S3 module, all signal routing including SPI / UART / RS-485 / USB, silk labels, antenna keep-out |
| Bottom | GND pour return, 12 V power pour (combined with top into a coplanar power plane), via stitching, optional secondary signal routing if needed for via-count reduction |

Master is single-side-population — keeps assembly cheap.

## Silk screen

- Refdes for every component.
- Polarity arrows for D8 SMAJ15A, D9–D12 ESD, all electrolytics
  (C_bulk).
- "BOOT" label near SW1.
- "RST" label near SW2.
- "12V/15A IN" near J1.
- "USB-C" near J2.
- Row labels: "ROW 0", "ROW 1", "ROW 2", "ROW 3" near J3–J6.
- "v2" + revision date + "MASTER" identifier.
- Antenna keep-out outline drawn in silk so it's visible at
  inspection time.

## DRC settings

| Rule | Value |
|---|---|
| Min trace width | 5 mil (0.127 mm) |
| Min clearance | 5 mil |
| Min drill | 0.3 mm |
| Min annular ring | 0.13 mm |
| Edge clearance | 0.5 mm to copper |

## Verification before Gerber

- [ ] Outline ~100 × 80 mm.
- [ ] 4 corner mounting holes.
- [ ] **18.6 × 5 mm antenna keep-out** present on BOTH layers,
      extending past the module's PCB edge, with no copper / vias /
      components inside.
- [ ] **U2 is K7803-1000R3 (1 A version)**, not K7803-500R3.
      Pinout: pin 1 VIN, pin 2 GND, pin 3 VOUT.
- [ ] **Z1 is BZT52C10 (10 V), not BZT52C12 (12 V).**
      Cathode → source (+12V_RAIL), anode → gate.
- [ ] **F_row polyfuses are 2920 SMD package**, NOT 1812 (1812
      family doesn't support 4 A hold).
- [ ] **D8 SMAJ15A is placed AFTER F1 fuse**, before Q1.
      Cathode (banded) → +12V, anode → GND.
- [ ] **Power input is polygon pour both layers** (J1 → F1 → Q1 →
      C_bulk → branch point). NOT a 60 mil track.
- [ ] J1 is a ≥15 A screw terminal (KF7.62-2P or DECA MA231 5 mm),
      NOT Phoenix MKDS 1.5/2 (only 8 A).
- [ ] **Row connectors J3–J6 are JST-VH 4-pin (B4P-VH-A, C144392)**,
      NOT JST-XH (3 A undersized) and NOT C124378 (1×40 single-row).
- [ ] **C_decap_uart qty = 1** (single VDD on SC16IS740IPW).
- [ ] **SC16IS740 pin 8 (mode-sel) tied to GND** for SPI mode.
- [ ] **SC16IS740 pin 11 (CTS) tied to GND** — input pin.
- [ ] **SC16IS740 pin 14 (RESET) pulled HIGH to 3V3 via 10 kΩ**
      (RESET is active-LOW per NXP datasheet).
- [ ] **ESP32-S3 IO46 has an explicit external 10 kΩ pull-down to
      GND.**
- [ ] EN pin: 10 kΩ pull-up + 1 nF cap to GND + SW2 to GND.
- [ ] BOOT button SW1 pulls IO0 to GND when pressed.
- [ ] USB-C CC1 and CC2 each have 5.1 kΩ to GND.
- [ ] USBLC6 U8 placed within 3 mm of the USB-C receptacle on the
      D+/D- nets.
- [ ] U7 (Bus 3 PHY) /RE tied to GND, DE driven by SC16IS740 RTS.
      **NOT** RTS driving both DE and /RE.
- [ ] Termination + bias resistors present at master end of every
      bus (4× R_term 120 Ω, 4× R_bias_a + R_bias_b 1 kΩ).
- [ ] ESD arrays D9–D12 (SM712-02HTG) on all 4 RS-485 A/B pairs
      within 3 mm of row connectors.
- [ ] **Master Q1 + Z1 topology**: drain → incoming 12V (post-fuse),
      source → PCB-12V_RAIL (load), gate via R_q1g 100 Ω +
      R_q1g2 10 kΩ pull-down. Z1 cathode → source, anode → gate.
- [ ] DRC passes with no errors.

## Layout-time gotchas

- **Don't run signal traces through the antenna keep-out.** KiCad's
  Rule Area will block tracks if you set it up correctly, but verify
  visually on both layers before plotting Gerber — autorouter and
  manual moves can both break this.
- **Don't route the 15 A power path as narrow tracks.** Use filled
  zones (KiCad's "Add Filled Zone" tool). KiCad's pushed-trace
  router doesn't auto-pour.
- **Don't tie the USB-C VBUS rail to the master 3V3 or +12V net.**
  USB is for programming/debug only; master is powered from J1.
- **The K7803 buck module footprint is SIP-3 (3 pins in a line) with
  matching L78xx pinout — VIN / GND / VOUT.** Verify against the
  selected MPN's datasheet (Mornsun, RECOM, CUI all match but pin
  ordering can subtly differ).
- **The polyfuses are physically large (2920 = 7.4 × 5.1 mm).**
  Reserve real estate.
- **Per-row PWR LEDs are 4.7 kΩ to 12 V, not 1 kΩ.** Don't reuse
  the 1 kΩ value from the 3V3 status LEDs — would burn the LED.

## Fab order

| Spec | Value |
|---|---|
| Quantity | 5 (1 system + spares for risk) |
| Surface finish | HASL |
| Estimated cost | ~EUR 8–12 per board at qty 5 |
