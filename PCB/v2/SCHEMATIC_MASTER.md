# Master PCB — Schematic + Layout Specification

**Revision:** 2026-07-04 (#102 doc-correction pass; previous 2026-04-26)

Tool-agnostic spec for the master PCB. **1 of these per system.** Most
complex board — ESP32-S3 module, 2× RS-485 paths (Bus A = rows 0+1 on
UART1, Bus B = rows 2+3 on UART2), 15 A power input. See
`KICAD_HANDOFF.md` for the KiCad 10 build steps.

**Superseded 2026-07-04 (#102):** the earlier 4-bus design (UART0 as a
bus + SC16IS740 SPI-UART expander for a 4th bus) is deleted. The
SC16IS740 cannot generate 250 kbaud from a 14.7456 MHz crystal (16×
divisor = 3.69; nearest is 230,400) and its RTS pin resets
de-asserted-HIGH, actively driving its bus's RS-485 driver from
power-on until firmware programs EFCR. SN65HVD75 is 3/20 unit-load
(213 nodes/bus), so 32 nodes per 2-row bus is trivial.

## Component list (with LCSC starting points)

| Ref | Value | Footprint | LCSC# (CHECK) | Notes |
|---|---|---|---|---|
| U1 | ESP32-S3-WROOM-1-N16R8 | SMD module 18×25.5 mm | C2913203 | 16 MB flash, 8 MB PSRAM, native USB |
| U2 | **K7803-1000R3** (or R-78E3.3-1.0 / V7803-1000) | SIP-3 module, drop-in for L78xx pinout (VIN/GND/VOUT) | C113973 (CHECK 1A version) | 12V→3.3V **switching buck** module, **1 A** (NOT 500 mA — peak load is ESP32-S3 ~400 mA WiFi TX + 2× SN65HVD75 ~30 mA = ~430 mA, leaving zero headroom on a 500 mA part; upgrade flagged by Gemini external review 2026-04-26). **Do NOT use a linear LDO here** — 12V→3.3V at ~200 mA = 1.74 W in SOT-89 (~0.5 W rated) → thermal shutdown. The 3-pin SIP buck module is a self-contained drop-in replacement; pinout matches an L78xx SIP. |
| U4 | SN65HVD75DR | SOIC-8 | C57928 | Bus A RS-485 PHY (rows 0+1, UART1) — A/B fan out to J3 + J4 |
| U5 | SN65HVD75DR | SOIC-8 | C57928 | Bus B RS-485 PHY (rows 2+3, UART2) — A/B fan out to J5 + J6 |
| U8 | USBLC6-2SC6 | SOT-23-6 | C7519 | USB-C ESD |
| U9 | AP7361C-33 | SOT-223 | (CHECK) | USB bench-power LDO, 1 A, fed from +5V_USB (added 2026-07-04, #102) |
| D_or1 | BAT60A Schottky | SOD-323 | (CHECK) | U9 output → 3V3 rail OR-diode (rail ~3.18 V diode-fed — inside ESP32-S3/SN65HVD75 3.0–3.6 V specs) |
| D_or2 | BAT60A Schottky | SOD-323 | (CHECK) | U2 buck output → 3V3 rail OR-diode; blocks backfeed into the buck when USB-only |
| Q1 | AOD409 | DPAK / TO-252 | C160005 (AOD409) | P-FET reverse-block. **WARNING (thermally unresolved):** AOD409 is 46 mΩ max at the −10 V VGS the Zener clamp enforces → ~6.6 W at the design's 12 A staggered ceiling, sustained for seconds during global flap cascades — unresolved on 2-layer FR-4. Final reverse-block topology (LM74700-Q1 ideal-diode + N-FET / per-row P-FETs / delete Q1) tracked in **issue #103**. (NTD2955 "alternate" deleted 2026-07-04 (#102) — 0.18 Ω max at −10 V VGS → 26–67 W at 12–15 A; not an equivalent.) A 10 V Zener clamp gate→source (Z1) is included for symmetry with the unit Q1 fix and to absorb brick over-voltage. |
| Z1 | BZT52C10 (or MMSZ5240B) | SOD-123 | C8062 (CHECK) | **10 V** Zener clamp on Q1 VGS — cathode → source (+12V_RAIL), anode → gate. **10 V (not 12 V)** for BOM commonality with unit Z1 (unit Z1 must be 10 V to keep AO3401A inside ±12 V VGS abs-max under Zener tolerance; master AOD409 ±20 V tolerates either). |
| F1 | 15 A fuse 5×20 mm + holder | THT | n/a (off-shelf) | Master input fuse, slow-blow. **Holder MPN required before order — #105.** |
| F_row1 | Polyfuse 4 A hold | **2920 SMD** (NOT 1812) | C175125 (CHECK — Bourns MF-LSMF400/16X-2 or equivalent 2920 4 A 16 V) | row 0. **1812 family does not support 4 A hold; 2920 is the smallest viable package for 4 A.** |
| F_row2 | same | 2920 | C175125 (CHECK) | row 1 |
| F_row3 | same | 2920 | C175125 (CHECK) | row 2 |
| F_row4 | same | 2920 | C175125 (CHECK) | row 3 |
| D1 | LED green | 0805 | C72043 | PWR indicator |
| D2 | LED blue | 0805 | C2293 | HEARTBEAT |
| D3 | LED red | 0805 | C2286 | FAULT |
| D4-D7 | LED green | 0805 | C72043 | ROW0..ROW3 power indicators |
| D8 | SMAJ15A | DO-214AC SMA | C167238 | Input TVS unidirectional. **Polarity: cathode (banded end) → +12V_RAIL (post-fuse), anode → GND.** Wrong-way orientation forward-biases and shorts the rail. **Placement: AFTER F1 fuse**, before Q1 — placing it before the fuse leaves no fuse to open during sustained TVS clamp. |
| D9 | SM712-02HTG (Bus A ESD) | SOT-23 | C172881 | RS-485 ESD across BUSA_A/BUSA_B |
| D10 | SM712-02HTG (Bus B ESD) | SOT-23 | C172881 | RS-485 ESD across BUSB_A/BUSB_B |
| J1 | Screw terminal 2-pin, **≥15 A continuous** | 7.62 mm pitch THT | C8270 (KF7.62-2P, 16 A) or C237691 (DECA MA231 5 mm 15 A) | 12V/15A input. **Do NOT use Phoenix MKDS 1.5/2 (only 8 A nominal).** Pick a 15+ A terminal block (KF7.62 series is the cheapest LCSC-stocked option). |
| J2 | USB-C 16-pin SMD | TYPE-C-31-M-12 | C165948 | USB receptacle, shielded |
| J3-J6 | **JST-VH 4-pin male, THT (B4P-VH-A)** | 3.96 mm pitch | **C144392** (CHECK — locked across system per OPEN_DECISIONS #4) | Row outputs (12V/A/B/GND). **JST-VH (≥5 A per pin), NOT JST-XH (3 A)** — row is fused at 4 A so JST-XH is undersized. |
| J7 | 2×4 pin header (or omit; replace with labeled test pads) | 2.54 mm | C2904 (CHECK — generic 2×4 male shrouded) **or remove and use test pads** | UART debug breakout (SPI breakout deleted with SC16IS740, #102 — IO9–IO13 are spare GPIO). Earlier C124378 reference was wrong (C124378 is a 1×40 single-row strip). If keeping, lock to a real 2×4 LCSC code; otherwise drop J7 in favour of labeled test pads on the UART nets. UART0 (IO43/IO44) test pads MUST stay — debug console. |
| SW1 | Tact switch 6×6 | SMD | C318884 | BOOT (IO0) |
| SW2 | Tact switch 6×6 | SMD | C318884 | RST (EN) |
| C_bulk | 470 µF / 25 V | Radial THT 8×11 mm | C107821 | Input bulk |
| C_in | 10 µF / 25 V X7R | 1206 | C19702 | Buck module input (per K7803/R-78E datasheet — 4.7–10 µF X7R, 1206 to ensure JLC-stocked X7R availability) |
| C_out | 10 µF / 10 V X7R | 0805 (×2) | C15850 | Buck module output (datasheet ≥ 10 µF) |
| C_decap_esp | 100 nF X7R | 0603 (×8) | C14663 | ESP32-S3 module decap |
| C_esp_bulk | 22 µF / 10 V X7R | 1206 (×1) | (CHECK) | ESP32-S3 module 3V3-pin bulk cap (see pin map — pin 2) |
| C_decap_phy | 100 nF X7R | 0603 (×2) | C14663 | SN65HVD75 Vcc decap — one per PHY (U4, U5), placed within ~3 mm of pin 8 |
| C_usb | 1 µF / 10 V X7R | 0603 (×2) | C15849 | USB rail decap |
| C_ldo | 1 µF / 10 V X7R | 0603 (×2) | C15849 | U9 AP7361C-33 input + output caps (per datasheet) |
| C_row | 10 µF / 25 V X7R | 0805 (×4) | C19702 | Per-row output bulk (post-polyfuse) |
| R_bias_a | 1 kΩ 1% | 0805 (×2) | C17513 | A bias to 3V3, one per PHY |
| R_bias_b | 1 kΩ 1% | 0805 (×2) | C17513 | B bias to GND, one per PHY |
| R_de_pd | 10 kΩ 1% | 0603 (×2) | C25804 | **DE pull-down to GND, one per PHY — REQUIRED.** SN65HVD75 has no internal pulls and ESP32-S3 GPIOs float during the boot-ROM window; a floating DE can enable a driver and jam a bus. |
| R_re_pu | 10 kΩ 1% | 0603 (×2) | C25804 | /RE pull-up to 3V3, one per PHY — receivers stay quiet during boot |
| R_led | 1 kΩ 1% | 0603 (×3) | C21190 | PWR/HB/FAULT LED current limit |
| R_led_row | 4.7 kΩ 1% | 0603 (×4) | C25879 | Row LED current limit (12V) |
| R_cc | 5.1 kΩ 1% | 0603 (×2) | C25905 | USB-C CC pull-downs |
| R_strap | 10 kΩ 1% | 0603 (×4) | C25804 | ESP32-S3 EN pull-up + strap pulls |

**Deleted 2026-07-04 (#102):** U3 SC16IS740IPW, U6/U7 SN65HVD75 (Bus 2/3
PHYs), Y1 14.7456 MHz crystal + C_xtal 18 pF load caps, C_decap_uart,
D11/D12 SM712, R_term 120 Ω master termination (×4), R_uart_strap
(SC16IS740 IRQ/reset pulls). No master termination remains — the master
is an unterminated mid-bus tap (see RS-485 section).

## High-level schematic blocks

```
                 +─────────+
   12V input ── │ Power   │── 3V3 ── ESP32-S3 ── UART1 ── U4 PHY A ── J3 (row 0) + J4 (row 1)
                │  in     │── 12V   (USB-C    ── UART2 ── U5 PHY B ── J5 (row 2) + J6 (row 3)
                │ Q1, F1, │   row    debug,   ── UART0 ── debug test pads ONLY (never a bus)
                │ TVS,    │   poly-  IO map)
                │ Cbulk   │   fuses)
                │ K7803   │
                +─────────+

   USB-C VBUS ── U9 AP7361C-33 LDO ── D_or1 BAT60A ─┐
   U2 buck output ─────────────────── D_or2 BAT60A ─┴── 3V3 rail (diode-OR; USB bench power)
```

## Power section

```
J1 pin 1 (+) ──┬── F1 (15 A slow-blow fuse holder + fuse)
                │
                ├── D8 SMAJ15A: cathode (banded) → +12V (post-fuse), anode → GND
                │   (placed AFTER F1 so the fuse can open during sustained TVS clamp;
                │    polarity reversed = forward-bias = dead short)
                │
                └── Q1 drain (AOD409 P-FET reverse-block)

Q1 (P-FET high-side reverse-block, standard topology):
  drain  ← incoming 12V (post-fuse, post-TVS)
  source → PCB-12V_RAIL (load side)
  gate   ── R_q1g 100 Ω series ── (gate node) ── R_q1g2 10 kΩ to GND
  Z1     10 V Zener: cathode → source, anode → gate (clamps |Vgs| under brick over-voltage; 10 V matches unit Z1 for BOM commonality)

How this works:
  - Normal polarity (input = +12 V): body diode (anode = drain = +12 V,
    cathode = source = load) conducts from input → load through the
    diode initially, charging C_bulk. Source rises to ~+11.4 V. Gate
    is at 0 V → Vgs ≈ -11.4 V → P-FET fully enhanced ON → forward
    conduction is the low-Rds(on) channel, not the body diode.
  - Reversed polarity (input = -12 V): source has no path to incoming,
    sits at ~0 V; gate is also 0 V → Vgs ≈ 0 → FET stays OFF. Body
    diode (anode at drain = -12 V, cathode at source = 0 V) is
    reverse-biased — no conduction. Protection works.
  - The 10 kΩ gate-to-GND resistor is the only gate drive needed; do
    NOT pull gate to incoming 12 V (that would defeat the protection).

C_bulk 470 µF (post-Q1 source) ─── PCB-12V_RAIL
   |
   ├── U2 buck module input (pin 1 VIN + C_in 10 µF X7R 1206)
   ├── F_row1 polyfuse → ROW0_12V → C_row 10 µF → J3 pin 1 + D4 row PWR LED
   ├── F_row2 polyfuse → ROW1_12V → C_row 10 µF → J4 pin 1 + D5 row PWR LED
   ├── F_row3 polyfuse → ROW2_12V → C_row 10 µF → J5 pin 1 + D6 row PWR LED
   └── F_row4 polyfuse → ROW3_12V → C_row 10 µF → J6 pin 1 + D7 row PWR LED

J1 pin 2 (-) ── PCB-GND plane

U2 buck module (**K7803-1000R3** / R-78E3.3-1.0 / V7803-1000, SIP-3, 1 A):
  pin 1 VIN  ← PCB-12V_RAIL (with C_in 10 µF X7R close to pin)
  pin 2 GND  → GND
  pin 3 VOUT → 3V3 + C_out 10 µF + per-IC decoupling

  Switching efficiency: ~85–90 %. Max heat at 200 mA load ≈ 75 mW
  (vs. 1.7 W for the LDO this replaces). Pin order matches the L78xx
  series so KiCad's L78xx SIP-3 footprint can be reused if no
  library symbol exists for the chosen module. **Verify pin order against the
  selected MPN's datasheet** — Mornsun, RECOM, and CUI all use VIN /
  GND / VOUT but pin numbering can differ.

Per-row PWR LED (×4):
   ROW_n_12V ── R_led_row 4.7 kΩ ── D_n anode ── D_n cathode ── GND
```

## ESP32-S3 module (U1)

Wire per Espressif's WROOM-1 datasheet (Table 3-1 "Pin Definitions" of
the ESP32-S3-WROOM-1 datasheet v1.6). Module is 41-pin + EPAD.

**Pin map (verified against datasheet, 2026-04-25):**

| Module pin | Signal | Net | Notes |
|---|---|---|---|
| 1, 40, 41, EPAD | GND | GND | All GND pads tied to plane (only these 4 — earlier doc revisions over-counted) |
| 2 | 3V3 | 3V3 | + 100 nF + 22 µF decap close to module |
| 3 | EN | EN | 10 kΩ pull-up to 3V3 + SW2 (RST button to GND) + 1 nF cap to GND |
| 4 | IO4 | UART2_/RE | → U5 SN65HVD75 pin 2 (/RE) via 1 kΩ + R_re_pu 10 kΩ pull-up to 3V3 |
| 5 | IO5 | UART2_TX | → U5 SN65HVD75 pin 4 (D) |
| 6 | IO6 | UART2_RX | ← U5 SN65HVD75 pin 1 (R) |
| 7 | IO7 | UART2_DE | → U5 SN65HVD75 pin 3 (DE) via 1 kΩ + R_de_pd 10 kΩ pull-down to GND |
| 8 | IO15 | UART1_/RE | → U4 SN65HVD75 pin 2 (via 1 kΩ + R_re_pu 10 kΩ pull-up to 3V3) |
| 9 | IO16 | UART1_DE | → U4 SN65HVD75 pin 3 (via 1 kΩ + R_de_pd 10 kΩ pull-down to GND) |
| 10 | IO17 | UART1_TX | → U4 SN65HVD75 pin 4 |
| 11 | IO18 | UART1_RX | ← U4 SN65HVD75 pin 1 |
| 12 | IO8 | (spare) | NC |
| 13 | IO19 | USB_DM | → U8 USBLC6 + USB-C D- |
| 14 | IO20 | USB_DP | → U8 USBLC6 + USB-C D+ |
| 15 | IO3 | strap | **Strapping pin** — add 10 kΩ pull-up to 3V3 to keep USB-JTAG functional by default |
| 16 | IO46 | strap | **Strapping pin — must be LOW at boot.** Add explicit external 10 kΩ pull-down to GND on this pin. Internal pull-down is not always reliable across resets. |
| 17 | IO9 | (spare) | NC — freed by SC16IS740 deletion (was IRQ) |
| 18 | IO10 | (spare) | NC — freed (was SPI CS) |
| 19 | IO11 | (spare) | NC — freed (was SPI MOSI) |
| 20 | IO12 | (spare) | NC — freed (was SPI SCK) |
| 21 | IO13 | (spare) | NC — freed (was SPI MISO) |
| 22 | IO14 | (spare) | NC |
| 23 | IO21 | (spare/test) | NC |
| 24 | IO47 | LED_FAULT | → D3 cathode (anode → 3V3 via R_led 1 kΩ) |
| 25 | IO48 | LED_HEARTBEAT | → D2 cathode (anode → 3V3 via R_led 1 kΩ) |
| 26 | IO45 | strap | Leave default per WROOM-1 datasheet (sets VDD_SPI level — module-internal). For N16R8 the module-internal circuitry pulls IO45 to a defined state. |
| 27 | IO0 | BOOT | **Strapping pin — must be HIGH at boot.** 10 kΩ pull-up to 3V3 + SW1 (BOOT button to GND). |
| 28-35 | IO35-IO42 | (spare) | NC (IO35-IO37 are used internally for SPI flash/PSRAM on N16R8 — do not connect externally) |
| 36 | RXD0 (IO44) | UART0_RX | → test pad only (debug console) |
| 37 | TXD0 (IO43) | UART0_TX | → test pad only (debug console) |
| 38 | IO2 | (spare) | NC — freed (was UART0_/RE) |
| 39 | IO1 | (spare) | NC — freed (was UART0_DE) |

**UART0 = debug console ONLY, never a bus** (superseded 2026-07-04
(#102): the earlier design routed UART0 to a Bus-2 RS-485 PHY). The
ESP32-S3 boot ROM prints on U0TXD at every reset, and that output is
eFuse-gated (UART_PRINT_CONTROL), not sdkconfig-gated — it cannot be
suppressed by build configuration, so it must never land on an RS-485
bus. Route IO43/IO44 to labeled test pads so a USB-serial dongle can
tap the console during bring-up. Keep the UART0 test pads.

**Strap pin summary** (per WROOM-1 datasheet Table 2-4):

| Pin | Required boot state | Implementation |
|---|---|---|
| IO0 | HIGH (SPI boot) | 10 kΩ pull-up + BOOT button to GND |
| IO3 | HIGH (USB-JTAG enabled) | 10 kΩ pull-up to 3V3 |
| IO45 | module-internal | leave default (no external pull) |
| IO46 | LOW | 10 kΩ pull-down to GND (explicit, do not rely on internal) |

## USB-C (J2 + U8 USBLC6-2SC6)

```
J2 (USB-C receptacle):
  VBUS (pins A4, A9, B4, B9) ── tied to local **+5V_USB** net which
                                  feeds U8 USBLC6 pin 5 (Vbus) AND the
                                  U9 AP7361C-33 bench LDO (see "USB
                                  bench power" below; added 2026-07-04,
                                  #102). Do not connect +5V_USB to any
                                  other circuitry — in particular NOT
                                  to the U2 buck input (K7803 input
                                  floor is 6 V). Provide a labeled
                                  test pad for bring-up scope access.
  GND  (pins A1, A12, B1, B12) ── GND plane
  D+   (pin A6, B6) ── USB_DP (paralleled with B6 if Type-C)
  D-   (pin A7, B7) ── USB_DM
  CC1  (pin A5)     ── R_cc 5.1 kΩ ── GND   (device-side pull-down)
  CC2  (pin B5)     ── R_cc 5.1 kΩ ── GND
  Shield ── chassis GND

U8 USBLC6-2SC6 (ESD across D+/D-):
  pin 1 (I/O1)   ── USB_DM
  pin 2 (GND)    ── GND
  pin 3 (I/O2)   ── USB_DP
  pin 4 (I/O2)   ── USB_DP (yes, repeated, 2 pins for differential)
  pin 5 (VBUS)   ── USB-C VBUS (USB +5 V rail) + 1 µF C_usb decap to GND.
                    **Do NOT tie to 3V3.** USBLC6's clamp reference must
                    be the supply rail being protected (USB +5 V).
  pin 6 (I/O1)   ── USB_DM

ESP32-S3 IO19/IO20 ──┬─ U8 ── USB-C D+/D-
                       └─── (no series R needed for native USB)

Add 1 µF decap on the VBUS rail at U8.
```

## USB bench power (added 2026-07-04, #102)

As previously drawn, every bench flash required the 12 V brick — VBUS
fed only the USBLC6 clamp (which stays). Added circuit:

```
+5V_USB ── U9 AP7361C-33 (SOT-223, 1 A LDO, C_ldo 1 µF in/out)
              │
              └── D_or1 BAT60A Schottky ──┐
                                          ├── 3V3 rail
U2 buck VOUT ── D_or2 BAT60A Schottky ────┘
```

- D_or2 sits in series after the buck output — it blocks backfeed into
  the buck when running USB-only.
- Diode-fed, the 3V3 rail sits at ~3.18 V — inside both the ESP32-S3
  and SN65HVD75 supply specs (3.0–3.6 V).
- Fine for flashing/debug; do **not** run sustained WiFi TX from a
  500 mA USB port.
- **WARNING: do NOT wire VBUS to the buck module input** — the K7803
  input floor is 6 V.

## SC16IS740 UART expander — DELETED (superseded 2026-07-04, #102)

Decision history: earlier revisions specified an SC16IS740IPW SPI-UART
bridge (U3) + 14.7456 MHz crystal (Y1) to provide a 4th UART for a
4-bus design. **Deleted** because (a) the SC16IS740 cannot generate
250 kbaud from a 14.7456 MHz crystal — the integer baud divisor is
14.7456e6/(16×250000) = 3.69, so the nearest achievable rate is
230,400 baud; (b) its RTS pin resets de-asserted-HIGH, actively driving
its bus's RS-485 driver from power-on until firmware programs EFCR — a
pull-down cannot fix a driven pin; and (c) SN65HVD75 is 3/20 unit-load
(213 nodes/bus), so 32 nodes on a 2-row bus is trivial and only 2 buses
are needed. The deletion also removes a second firmware driver path, a
hand-drawn KiCad symbol, a crystal, 2 PHYs and 2 ESD arrays. SPI pins
IO10–IO13 and IRQ IO9 are freed as spare GPIO.

## RS-485 paths (×2 identical sub-circuits)

Per bus (`n` = A for Bus A / rows 0+1 / UART1 / U4, or B for Bus B /
rows 2+3 / UART2 / U5):

```
Master UART_n ── SN65HVD75 U4/U5 (transceiver):
  pin 1 (R)   → UART_n RX
  pin 2 (/RE) ← UART_n /RE (via 1 kΩ R_re) + R_re_pu 10 kΩ pull-up to 3V3
  pin 3 (DE)  ← UART_n DE  (via 1 kΩ R_de) + R_de_pd 10 kΩ pull-down to GND
  pin 4 (D)   ← UART_n TX
  pin 5 (GND) ── GND
  pin 6 (A)   → BUSn_A ── R_bias_a 1 kΩ to 3V3 + SM712 + BOTH row connectors pin 2
  pin 7 (B)   → BUSn_B ── R_bias_b 1 kΩ to GND + SM712 + BOTH row connectors pin 3
  pin 8 (Vcc) ── 3V3 + 100 nF decap

Fan-out — each PHY's A/B nets go to TWO row connectors:
  U4 (Bus A): J3 (row 0) pins 2/3 + J4 (row 1) pins 2/3
  U5 (Bus B): J5 (row 2) pins 2/3 + J6 (row 3) pins 2/3

Output connector J3/4/5/6 (Row n):
  pin 1: ROW_n_12V (post-polyfuse F_rown)
  pin 2: RS485_A (that row's bus A-line)
  pin 3: RS485_B (that row's bus B-line)
  pin 4: GND
```

**No 120 Ω termination at the master** (master termination resistors
deleted 2026-07-04, #102). The master is an **unterminated mid-bus
tap**: each 2-row bus is one linear path from the far end of one row,
through its bus PCBs and cable, through the master's on-board A/B
traces, out the other row's cable to that row's far end. The two 120 Ω
terminator plugs at the two far ends of each bus terminate it (still
exactly one plug per row, 4 plugs system-wide — unchanged hardware).
The **failsafe bias (1 k/1 k) stays at the master, one set per PHY**
(2 sets).

**DE//RE boot-float pulls (added 2026-07-04, #102):** SN65HVD75 has no
internal pulls and ESP32-S3 GPIOs float during the boot-ROM window — a
floating DE can enable a driver and jam a bus. **R_de_pd 10 kΩ from
each DE pin to GND (×2) is REQUIRED.** R_re_pu 10 kΩ pull-ups on both
/RE lines (×2) are included so receivers stay quiet during boot.

SM712-02HTG (2× ESD arrays D9, D10), one per PHY:

```
SM712 pin 1 (I/O1) ── BUSn_A
SM712 pin 2 (I/O2) ── BUSn_B
SM712 pin 3 (GND)  ── GND
```

(Pinout corrected 2026-07-04, #102 — an earlier draft wrongly put GND
on pin 2. Verify against the Littelfuse datasheet drawing during
symbol creation.)

## LEDs (3 master-status, hardwired-12V row indicators)

```
3V3 ── R_led 1 kΩ ── D1 anode ── D1 cathode ── GND  (PWR, hardwired-on when 3V3 alive)
3V3 ── R_led 1 kΩ ── D2 anode ── D2 cathode ── ESP32-S3 IO48  (HEARTBEAT, sinks)
3V3 ── R_led 1 kΩ ── D3 anode ── D3 cathode ── ESP32-S3 IO47  (FAULT, sinks)

ROW_n_12V ── R_led_row 4.7 kΩ ── D_n+3 anode ── D_n+3 cathode ── GND  (row power-OK, hardwired)
```

## Boot/Reset buttons

```
SW1 (BOOT button):
  pin A ── ESP32-S3 IO0 (with 10 kΩ pull-up to 3V3)
  pin B ── GND

SW2 (RST button):
  pin A ── ESP32-S3 EN (with 10 kΩ pull-up to 3V3 + 1 nF to GND)
  pin B ── GND
```

## Floorplan (rough placement)

```
                     ~100 mm
   +─────────────────────────────────────────────+
   │ J1 screw    USB-C J2     SW1     SW2        │ <-- TOP edge
   │ terminal              BOOT    RST        ... │
   │ 12V/15A                                      │
   │                                              │
   │  D8 TVS                                      │
   │  Q1 P-FET   F1 fuse                          │
   │  C_bulk                                      │
   │  U2 buck module  U8 USBLC                    │
   │                                              │
   │  ┌─────────────────────────┐                  │ ~80 mm
   │  │ U1 ESP32-S3-WROOM-1     │                  │
   │  │   antenna keep-out →     │                  │
   │  │                          │                  │
   │  └─────────────────────────┘                  │
   │                                                │
   │  U9 AP7361C + D_or1/D_or2 (USB bench power)    │
   │                                                │
   │      U4 PHY A            U5 PHY B              │
   │  D9 ESD, biasA       D10 ESD, biasB            │
   │  (A/B fan out to     (A/B fan out to           │
   │   J3 + J4)            J5 + J6)                 │
   │  F_row1   F_row2   F_row3   F_row4 (polyfuses) │
   │  D4       D5       D6       D7   (row LEDs)    │
   │                                                │
   │  J3       J4       J5       J6                 │ <-- BOTTOM edge
   │  row 0    row 1    row 2    row 3              │ (4-pin JST-VH
   │  Bus A    Bus A    Bus B    Bus B              │  row outputs)
   │                                                │
   +─────────────────────────────────────────────+
   M-holes at 4 corners
```

Layout principles:
- Power input (J1, F1, Q1, TVS, bulk) along the top edge.
- ESP32-S3 module roughly centred. Antenna keep-out per WROOM-1
  datasheet section 7.2: **18.6 mm × 5 mm rectangle** under the
  antenna area extending past the module's PCB edge. No copper,
  components, traces, or vias inside this rectangle on either layer.
- 2 RS-485 PHYs along the bottom section, each placed between its TWO
  row output connectors (U4 between J3/J4, U5 between J5/J6) so the
  A/B fan-out traces stay short.
- Per-row polyfuses near the master power section, distributing 12V
  through wide traces to each output connector.
- USB-C and BOOT/RST buttons on the top edge for user access.
- U2 buck module Kelvin-tap from the input rail (separate trace before
  per-row branches) to keep master logic 3V3 stable during row inrush.

## Power trace widths (1 oz copper, IPC-2152 + 30 °C rise budget)

The earlier table (60 mil for 15 A, 30 mil for 4 A) violates 1 oz
copper current capacity by ~3×. Replace with poured copper for the
full power path:

| Net | Geometry | Reason |
|---|---|---|
| J1 → F1 → Q1 → C_bulk (12V input, **15 A**) | **Polygon pour on BOTH layers**, stitched with via fence (≥6 vias 0.6 mm drill, 1.0 mm via pad) at every layer transition; minimum local width 250 mil (6.35 mm) where a polygon necking is unavoidable | 15 A on 1 oz copper requires ~325 mil of trace OR a copper pour. A pour is shorter, lower R, and has less ΔT. |
| C_bulk → per-row polyfuses (12V trunk, ~5 A peak per branch) | **Polygon pour preferred**; if traced, ≥150 mil (3.81 mm) | 5 A per row branch on 1 oz needs ≥110 mil; spec 150 mil for headroom and to keep ΔT < 20 °C |
| Polyfuse → row output J3-J6 (4 A per row) | **≥120 mil (3.05 mm)**, polygon pour preferred | 4 A on 1 oz needs ~90 mil for 30 °C rise; spec 120 mil for margin |
| 3V3 to ESP32-S3 / PHYs (≤500 mA) | 20 mil (0.5 mm) | Comfortable margin at ≤500 mA |
| Signal traces | 6 mil (0.15 mm) | Low current; matches JLC default |
| RS-485 differential A/B pairs | 10 mil (0.25 mm) routed as a loose pair | Low-speed (250 kbaud) so trace impedance is non-critical |

**GND pour on both layers** for return paths and crosstalk shielding.
The 12 V pour and GND pour together form a coplanar power plane —
keep their edges parallel for low loop inductance into C_bulk.

## Fab parameters

| Parameter | Value |
|---|---|
| Outline | ~100 × 80 mm |
| Layer count | 2 |
| Substrate | FR-4 |
| Thickness | 1.6 mm |
| Copper weight | 1 oz |
| Surface finish | HASL (cheaper) |
| Mounting holes | 4× M3 clearance at corners |

## KiCad 10 build steps

1. New KiCad project: `splitflap-master-v2` (under `PCB/v2/kicad/master/`).
2. Schematic editor:
   - Place all components from the LCSC table at the top of this doc.
     ESP32-S3-WROOM-1 needs a custom symbol or Espressif's official
     KiCad library (https://github.com/espressif/kicad-libraries).
   - Add `LCSC` field per part (see `KICAD_HANDOFF.md` § 4).
   - Wire per the section-by-section nets above. Use net labels
     liberally — `12V`, `GND`, `3V3`, `ROW0_12V`...`ROW3_12V`,
     `RS485_A0`...`RS485_A3`, `RS485_B0`...`RS485_B3`, `UART0_TX`,
     etc.
   - Group decoupling caps near their ICs.
3. ERC: fix any unconnected pins (`Inspect → Electrical Rules Checker`).
4. Update PCB from Schematic.
5. PCB editor (place per `LAYOUT_MASTER.md`):
   - Draw outline ~100 × 80 mm on `Edge.Cuts`.
   - Place per the floorplan zones.
   - **Mark the WROOM-1 antenna keep-out as a Forbidden Area
     (Rule Area) on BOTH layers** — 18.6 × 5 mm rectangle past the
     module's PCB edge, no copper / vias / components / traces
     allowed inside.
   - Route 12V power per the trace-width table — use **filled
     zones** (Add Filled Zone) on both layers for the 15 A path,
     stitched with vias every ~10 mm.
   - Add filled zones for GND (both layers) and 3V3 (top layer
     distribution).
   - Press `B` to fill all zones.
6. DRC: fix all errors.
7. 3D viewer sanity check (View → 3D Viewer).
8. Plot Gerbers + drill files + BOM + CPL (`KICAD_HANDOFF.md` § 10).
9. Order: 5 boards from JLC.

## Known design risks (verify in KiCad)

1. **WROOM-1 footprint orientation**: KiCad's library may not have
   the ESP32-S3-WROOM-1-N16R8 — use Espressif's official KiCad lib
   or hand-draw. Verify pin numbering matches the wiring table.
2. **SM712 symbol pinout**: pin 1 = I/O1 (A), pin 2 = I/O2 (B),
   pin 3 = GND. Verify against the Littelfuse datasheet drawing during
   symbol creation — an earlier draft wrongly put GND on pin 2.
3. **USB-C VBUS feeds only the USBLC6 clamp + U9 bench LDO**: Master
   runtime power is from J1. Do NOT wire VBUS to the U2 buck module
   input (K7803 input floor is 6 V) or to the 12 V rail.
4. **AOD409 P-FET orientation (high-side reverse-block, standard
   topology)**: **drain** to incoming 12 V (post-fuse), **source** to
   PCB-12V_RAIL (load). Gate to GND via 10 kΩ. This is the opposite
   of an earlier draft that swapped source/drain — the corrected
   topology has the body diode pointing INPUT→LOAD so it conducts on
   power-up, then the FET enhances ON via Vgs = -V_load. Reversed
   input keeps the FET off and the body diode reverse-biased. Verify
   the symbol orientation in KiCad matches this description.

## Verification checklist

- [ ] All LCSC parts verified in catalogue.
- [ ] WROOM-1 antenna keep-out present: **18.6 × 5 mm rectangle**
      extending past the module edge with no copper, components,
      traces, or vias on either layer.
- [ ] EN pin (chip enable) has 10 kΩ pull-up + 1 nF cap (per
      Espressif reference).
- [ ] BOOT button pulls IO0 to GND when pressed.
- [ ] USB-C CC1 and CC2 each have 5.1 kΩ to GND.
- [ ] All 4 polyfuses sized 4 A hold (Bourns MF-LSMF400/16X-2, 2920
      package — NOT MF-MSMF/1812).
- [ ] Row output connectors all JST-VH 4-pin (B4P-VH-A, 3.96 mm),
      keyed/indexed.
- [ ] ESD arrays (SM712, D9 + D10) present on both bus A/B pairs;
      SM712 pin 3 = GND (pin 1 = I/O1/A, pin 2 = I/O2/B).
- [ ] **NO 120 Ω termination at the master** (mid-bus tap); failsafe
      bias resistors (1 k/1 k) present at the master, one set per PHY.
- [ ] 10 kΩ DE pull-downs to GND on both PHYs; 10 kΩ /RE pull-ups
      to 3V3.
- [ ] UART0 (IO43/IO44) routed to test pads only — never to a PHY.
- [ ] USB bench power: U9 AP7361C-33 + D_or1/D_or2 BAT60A diode-OR
      into 3V3; VBUS NOT wired to the buck input.
- [ ] DRC passes.
