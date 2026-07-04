# Master PCB

**Revision:** 2026-07-04 (#102 doc-correction pass; previous 2026-04-26)

ESP32-S3 controller. Drives **2 RS-485 buses** (Bus A = rows 0+1 on
UART1, Bus B = rows 2+3 on UART2; superseded 2026-07-04 (#102): the
earlier 4-bus / SC16IS740 design is deleted). 64 units total. USB-C
native programming. **Sources row power**: takes one 12 V / 15 A input
and outputs 12V + GND + A + B per row on a single 4-pin combined
connector (still 4 row connectors — two per bus).

## Block diagram

```
12 V / 15 A DC input (screw terminal)
   |
   +- F1 fuse 15 A slow-blow (5x20 mm)
   +- TVS SMAJ15A
   +- Q1 P-FET reverse-block (AOD409, DPAK) — thermally marginal, topology under review (#103)
   +- Z1 BZT52C10 10 V Zener gate→source clamp on Q1 (+ R_q1g 100 Ω, R_q1g2 10 kΩ)
   +- Cbulk 470 uF / 25 V + 100 nF
   |
   +-> VBUS_12V_RAIL (wide pour, 5+ mm trace)
   |        |
   |        +-> Per-row polyfuses (×4, 4 A hold, 2920 SMD)
   |        |        |
   |        |        +-> 12V_ROW0, 12V_ROW1, 12V_ROW2, 12V_ROW3
   |        |        +-> Each combined into the row's 4-pin output (12V + GND + A + B)
   |        |        +-> Per-row PWR LED (post-polyfuse, indicates row 12V present)
   |        |
   +- U2 buck module K7803-1000R3 12->3.3 V -- D_or2 BAT60A --> VCC_3V3 (master logic; switching, ~85-90% efficient — replaces LDO due to 12V→3.3V dissipation hazard)
                                                    |
   USB-C VBUS -- U9 AP7361C-33 LDO -- D_or1 BAT60A -+  (USB bench power — see below)
                                                    v
   ESP32-S3-WROOM-1-N16R8
       |
       +- USB-C (USBLC6 ESD, native CDC)
       +- BOOT button (IO0 -> GND)
       +- RESET button (EN -> GND)
       +- LEDs: PWR, HEARTBEAT, FAULT, ROW0..ROW3 (7 total)
       |
       +- UART1 -> SN65HVD75 U4 (PHY A) -> Bus A = rows 0+1 (nets BUSA_A / BUSA_B -> J3 + J4)
       +- UART2 -> SN65HVD75 U5 (PHY B) -> Bus B = rows 2+3 (nets BUSB_A / BUSB_B -> J5 + J6)
       +- UART0 (IO43/IO44) -> debug console test pads ONLY — never a bus
          (boot ROM prints on U0TXD at every reset; that output is eFuse-gated
           via UART_PRINT_CONTROL, not sdkconfig-gated)

Superseded 2026-07-04 (#102): the previous 4-bus design (UART0 as Bus 2 +
SC16IS740 SPI-UART expander driving Bus 3) is deleted — see "Bus
architecture" below for why.

Per-row 4-pin output (combined power + signal):
  Pin 1: 12V (post-polyfuse)
  Pin 2: RS485_A
  Pin 3: RS485_B
  Pin 4: GND

Each transceiver block (×2):
   - DE/RE GPIOs (ESP32-S3 native), 10 kΩ DE pull-down to GND +
     10 kΩ /RE pull-up to 3V3 (GPIOs float during the boot-ROM window;
     a floating DE can enable a driver and jam the bus)
   - NO 120 ohm termination at the master — master is an unterminated
     mid-bus tap; the two 120 Ω terminator plugs at the two far ends of
     each bus terminate it
   - 1k bias to 3V3 (A) + 1k bias to GND (B) — failsafe bias stays at
     the master, one set per PHY
   - SM712 ESD across A/B (one per PHY)
   - A/B fanned out to TWO row output connectors:
     U4 -> J3 (row 0) + J4 (row 1); U5 -> J5 (row 2) + J6 (row 3)
```

## Power

- **Input: 12 V / 15 A from a single SMPS brick.** Sized for 64-unit
  worst-case peak (~16 A briefly during simultaneous flap transitions);
  steady-state draw is ~3-5 A.
- Input connector: **2-pin Phoenix-style screw terminal** (5 mm pitch).
  Robust, accepts bare-wire or terminated cable from any 12 V brick.
- Reverse-polarity: P-FET (AOD409 in DPAK). **NTD2955 deleted 2026-07-04
  (#102)** — it is 0.18 Ω max at −10 V VGS → 26–67 W at 12–15 A; not an
  AOD409 equivalent in any sense. **WARNING (thermally unresolved):**
  AOD409 itself is 46 mΩ max at the −10 V the Zener clamp enforces →
  ~6.6 W at the design's 12 A staggered ceiling, sustained for seconds
  during global flap cascades — unresolved on 2-layer FR-4. The final
  reverse-block topology (LM74700-Q1 ideal-diode + N-FET / per-row
  P-FETs / delete Q1) is tracked in **issue #103**. **Z1 BZT52C10 10 V
  Zener clamp on Q1
  gate→source** (cathode → source/+12V_RAIL, anode → gate). AOD409 is
  ±20 V VGS rated so the clamp is not strictly required, but is
  included for symmetry with the unit Q1 fix (unit Q1 AO3401A needs
  10 V to stay safely inside ±12 V VGS abs-max under Zener
  tolerance — a 12 V Zener +10% = 13.2 V puts the FET out of spec).
  R_q1g 100 Ω gate series + R_q1g2 10 kΩ gate pull-down to GND
  complete the topology.
- Input fuse: 15 A slow-blow, 5×20 mm holder.
- TVS: SMAJ15A across input.
- Bulk: 470 uF / 25 V + 100 nF for inrush smoothing.
- Per-row protection: 4× polyfuses (4 A hold / 8 A trip, **2920
  package** — Bourns MF-LSMF400/16X-2 or equivalent; **NOT 1812**,
  which does not support 4 A hold), one per row 12 V output.
- 12 V → 3.3 V for master logic: **switching buck module
  (K7803-1000R3 / R-78E3.3-1.0 / V7803-1000, SIP-3 drop-in for L78xx —
  all 1 A parts; the alternates listed in an earlier draft were 500 mA
  variants and violate the locked 1 A requirement)**,
  fed Kelvin-style from the input rail (before per-row branches) so
  master logic stays stable during row inrush. **Not** a linear LDO —
  12 V→3.3 V at the master's ~150-250 mA load would dissipate ~1.7 W
  in a SOT-89 LDO (rated ~0.5 W), which would thermal-shutdown.

Master is the only point of power injection in the system. No row
bricks. No power distribution board.

**Firmware staggering — HARD REQUIREMENT.** 64 units × 240 mA peak =
15.36 A simultaneous-coil-energise current. (The 240 mA/unit figure is
a paper number under review — bench measurement tracked in **#101**;
real 12 V 28BYJ-48 coils are 130–380 Ω/phase → likely 120–185 mA/unit.)
Master firmware MUST stagger motor steps on power-up and during global
commands so that peak input current stays below 12 A (~50 % units
active at once). Flagged by Gemini external review 2026-04-26.
Corrected rationale (2026-07-04, #102): 15.36 A would NOT nuisance-trip
F1 — a 5×20 time-lag fuse at 1.02× rating essentially never opens. The
actual failure mode is the brick's OCP hiccuping (whole-display
brownout loop); F1 is fire protection against a shorted board, not
overload coordination. Don't uprate the fuse — the 4 A row polyfuses
give per-row fault isolation that uprating the input would defeat.

## USB bench power (added 2026-07-04, #102)

As originally drawn, every bench flash required the 12 V brick — VBUS
fed only the USBLC6 ESD clamp (that part stays). Added:

- **U9 = AP7361C-33** (SOT-223, 1 A LDO) fed from +5V_USB.
- **D_or1 (BAT60A Schottky)** from U9 output into the 3V3 rail.
- **D_or2 (BAT60A)** in series after the buck (U2) output into the same
  rail — blocks backfeed into the buck when running USB-only.
- Diode-fed, the rail sits at ~3.18 V — inside both the ESP32-S3 and
  SN65HVD75 specs (3.0–3.6 V).
- Fine for flashing/debug; do **not** run sustained WiFi TX from a
  500 mA USB port.
- **WARNING: do NOT wire VBUS to the buck module input** — the K7803
  input floor is 6 V.

## ESP32-S3 module

- ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB PSRAM).
- USB: native CDC -> USB-C through USBLC6-2SC6 ESD.
- Reset/boot buttons.

## Bus architecture (2 buses)

- **Bus A = rows 0+1 on UART1** (native). **Bus B = rows 2+3 on UART2**
  (native).
- **UART0 (IO43/IO44) is the debug console ONLY — never a bus.** The
  ESP32-S3 boot ROM prints on U0TXD at every reset, and that output is
  eFuse-gated (UART_PRINT_CONTROL), not sdkconfig-gated. Keep the UART0
  test pads.
- SN65HVD75 is a 3/20 unit-load device (213 nodes/bus), so 32 nodes per
  2-row bus is trivial.
- Baud stays 250 kbaud — native UARTs on both ends (STM32G030 @ 64 MHz
  has 0.08 % BRR error at 250 kbaud).
- SPI pins IO10–IO13 and IRQ IO9 are freed — spare GPIO.

**Superseded 2026-07-04 (#102):** the earlier 4-bus design used UART0
as Bus 2 plus an SC16IS740 SPI-UART expander (+ 14.7456 MHz crystal)
driving Bus 3. Deleted because: (a) the SC16IS740 cannot generate
250 kbaud from a 14.7456 MHz crystal (integer divisor
14.7456e6/(16×250000) = 3.69 — nearest achievable is 230,400 baud);
(b) its RTS pin resets de-asserted-HIGH, actively driving the Bus-3
RS-485 driver from power-on until firmware programs EFCR — a pull-down
cannot fix a driven pin; (c) deletion removes a second firmware driver
path, a hand-drawn KiCad symbol, a crystal, 2 PHYs and 2 ESD arrays.

## RS-485 paths (×2)

Two identical sub-circuits, one per bus. Each PHY's A/B nets fan out to
TWO row output connectors that also carry those rows' 12 V + GND:
U4 → J3 (row 0) + J4 (row 1); U5 → J5 (row 2) + J6 (row 3).

The master is an **unterminated mid-bus tap**: each 2-row bus is one
linear path from the far end of one row, through its bus PCBs and
cable, through the master's on-board A/B traces, out the other row's
cable to that row's far end. The two 120 Ω terminator plugs at the two
far ends of each bus terminate it (still exactly one plug per row,
4 plugs system-wide — unchanged hardware).

Per bus:
- Transceiver: SN65HVD75DR (3.3 V, half-duplex).
- DE/RE controlled separately by native ESP32-S3 GPIOs. **10 kΩ
  pull-down on DE to GND** (required — SN65HVD75 has no internal pulls
  and ESP32-S3 GPIOs float during the boot-ROM window; a floating DE
  can enable a driver and jam the bus) + 10 kΩ pull-up on /RE to 3V3
  (receivers stay quiet during boot).
- TX/RX from the corresponding native UART.
- Termination: **none at the master** (mid-bus tap — see above; the
  master 120 Ω termination resistors are deleted).
- Failsafe bias: 1 k from A to 3V3, 1 k from B to GND (one set per PHY).
- ESD: SM712-02HTG across A/B to GND (one per PHY).
- Output connectors: **JST-VH 4-pin (B4P-VH-A, C144392 — verify per
  #105), 3.96 mm pitch** — two per bus.

## Output connector pinout (per row)

| Pin | Net | Notes |
|---|---|---|
| 1 | 12V | post-polyfuse |
| 2 | RS485_A | from local transceiver |
| 3 | RS485_B | from local transceiver |
| 4 | GND | |

Pin order is **12V / A / B / GND** (outer-inner-inner-outer) so 12 V
and GND are on opposite ends of the connector — adjacent-pin shorts
cannot directly bridge the supply rails. Same pinout on every 4-pin
row connector in the system (master rows, bus PCB ends, terminator
plug, daisy-chain cable).

4-pin JST-VH (B4P-VH-A, C144392 — verify per #105) at 3.96 mm pitch,
≥5 A per pin. The 12V pin carries up to
~3 A typical / 4 A peak per row — within standard pin rating since real-
world peaks are brief and steady-state is ~1-2 A.

## UART pin assignment (suggested)

| Function | Pin/IO |
|---|---|
| UART1 (Bus A, rows 0+1) TX/RX/DE//RE | IO17 / IO18 / IO16 / IO15 |
| UART2 (Bus B, rows 2+3) TX/RX/DE//RE | IO5 / IO6 / IO7 / IO4 |
| UART0 (debug console ONLY, never a bus) TX/RX | IO43 / IO44 — test pads |
| Spare GPIO (freed by SC16IS740 deletion: ex-SPI + IRQ) | IO9 / IO10 / IO11 / IO12 / IO13 |
| HEARTBEAT LED | IO48 |
| FAULT LED | IO47 |
| ROW0..ROW3 PWR LEDs | hardwired to post-polyfuse 12V via current-limited path |

ESP32-S3 GPIO matrix lets these be remapped.

## LEDs

- D1 PWR (green, hardwired to 3V3).
- D2 HEARTBEAT (blue, GPIO).
- D3 FAULT (red, GPIO).
- D4-D7 ROW0..ROW3 (4× green, hardwired to per-row post-polyfuse 12V).

## Test pads

- VIN, 3V3, GND.
- UART0 TX/RX (IO43/IO44) test pads — debug console; keep these (boot
  ROM output lands here).
- Bus UART TX/RX/DE (UART1, UART2) on a small breakout header.
- (SPI breakout deleted with the SC16IS740, 2026-07-04 #102.)

## PCB stack-up

- 2-layer HASL.
- Recommended outline: ~100 × 80 mm. Power section needs wide copper
  pours and the 4× 4-pin output connectors take edge real estate.
- Mounting: 4× M3 corners.

## Layout notes

- USB-C and 4× 4-pin row output connectors along the same long edge for
  cable routing.
- Screw terminal input on the opposite edge (or on the back) so the
  power cable doesn't fight the row cables.
- Wide 12 V rail copper from the input through the polyfuses to each
  row connector. Aim for >5 mm trace width or full-fill pour.
- Polyfuses placed close to their respective row connectors.
- TVS at the input within ~10 mm of the screw terminal.
- Master 3.3 V buck module input tapped Kelvin-style from the post-fuse
  rail (separate trace, not the high-current pour).
- Continuous GND pour between transceivers for crosstalk rejection.
- ESP32-S3 antenna keep-out per WROOM-1 datasheet.
- Each PHY placed between its two row connectors so the A/B fan-out
  traces stay short (U4 between J3/J4, U5 between J5/J6).

## BOM

See `MASTER_BOM.csv`.
