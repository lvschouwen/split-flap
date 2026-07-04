# v2 Architecture

**Revision:** 2026-04-26

## Target system

One master. Four rows. Sixteen units per row. **64 units total.**

**One 12 V / 15 A brick** feeds the master. The master internally
distributes power to four separately fused row outputs (one
polyfuse per row) — the master has 4 row ports. Signal-wise the rows
are paired onto **two RS-485 buses** (Bus A = rows 0+1, Bus B =
rows 2+3), driven directly by the ESP32-S3's native UART1/UART2.
**Rows do not have their own bricks.** No rigid backplane: each row
uses a daisy-chain DIN-rail bus PCB (see `BUS_PCB.md`).

## System block diagram

```
                        1× 12 V / 15 A brick
                               |
                               v
                    +----------------------+
                    |  Master              |
                    |  ESP32-S3 (UART1 +   |
                    |  UART2 drive buses;  |
                    |  UART0 console only) |
                    |  USB-C               |
                    |  2x RS-485 PHY       |
                    |  per-row polyfuse    |
                    |  4x 4-pin row ports  |
                    +-+--+--+--+-----------+
                      |  |  |  |
                      v  v  v  v
                4-pin combined cable per row (12V/GND/A/B)
                      |  |  |  |
   Power: each row keeps its own fused 12 V feed and 4-pin cable.
   Signal: rows are paired per bus, with the master as an
   UNTERMINATED MID-BUS TAP between the two rows of each pair:

   [120R]◄─row 1 far end ──► Master UART1 (Bus A) ◄── row 0 far end─►[120R]
   [120R]◄─row 3 far end ──► Master UART2 (Bus B) ◄── row 2 far end─►[120R]

   Termination: ONLY the 120R terminator plugs at the far ends
   (still one plug per row, 4 system-wide). No termination at the
   master. 1k/1k failsafe bias at the master, one set per PHY.

   Per row, the cable plugs into the first of two daisy-chained bus PCBs:

   Master Row 0 ──► [Bus PCB A0] ──short cable──► [Bus PCB B0] ──► [120R term]
                          |                              |
                       8 pogo-pin stations           8 stations
                          |                              |
                       Units 0..7 clip on            Units 8..15 clip on
                       via DIN rail                  via DIN rail

   ... same topology for Rows 1, 2, 3

   Each unit: 4 pogo pins on underside (12V/A/B/GND), DIN rail clip,
              IDENTIFY button + LED. No cable to unit.
```

> **⚠ GEOMETRY UNRESOLVED — issue #100**: the documented unit outline
> cannot sit on the 37 mm station pitch (80 × 40 mm board, 80 mm along
> the rail = 43 mm overlap; rotated is still 40 > 37 mm and puts the
> pogo column parallel to the traces), and the bus PCB has
> mounting/clearance collisions. Station pad geometry and unit
> orientation WILL be redesigned after a dimensioned cross-section +
> mock-up. Do not treat the pogo/station geometry as final.

## Bus topology

**2 RS-485 buses, each serving a row pair.** Bus A = rows 0+1 on the
ESP32-S3's native UART1; Bus B = rows 2+3 on UART2. UART0 stays
console-only. The logical bus index identifies a row **pair**; row and
column within the pair are disambiguated during commissioning by the
IDENTIFY-button flow (no UX change — see Addressing below).

Physically, each 2-row bus runs far-end-of-row-N ↔ master ↔
far-end-of-row-N+1: the master is an **unterminated mid-bus tap**.
Termination is ONLY the two 120 Ω terminator plugs at the two far
ends (still one plug per row, 4 system-wide — unchanged). The
master's own 120 Ω termination is deleted; the 1k/1k failsafe bias
stays at the master, one set per PHY. Baud stays 250 kbaud.

**Superseded 2026-07-04 (#102):** an earlier revision used 4 buses
(one per row); since the ESP32-S3 has only 3 native UARTs, the 4th
came from an SC16IS740 single-channel SPI-UART expander. The
SC16IS740 was deleted because: (1) it cannot generate 250 kbaud from
its 14.7456 MHz crystal — the integer divisor tops out at 230,400
nearby; (2) its RTS pin resets high, driving the bus at power-on;
(3) SN65HVD75 is a 3/20 unit-load transceiver (up to 213 nodes), so
32 units on one bus is trivial. See the superseded 4th-UART entry in
`OPEN_DECISIONS.md`'s resolved list for the record.

## Power flow

- **Single 12 V / 15 A brick** feeds the master. Sized for 64-unit
  worst-case peak (~16 A briefly during simultaneous flap transitions);
  steady-state draw is ~3-5 A.
- **Firmware staggering — HARD REQUIREMENT.** 64 units × 240 mA peak
  = 15.36 A simultaneous-coil current would nuisance-trip the 15 A
  fuse. Master firmware MUST stagger motor steps so peak input
  current stays under ~12 A (i.e. ≤50 % units active at once).
  Flagged by Gemini external review 2026-04-26. **Note (2026-07-04):**
  the 240 mA/unit figure is a paper number under review — real 12 V
  28BYJ-48 coils measure 130–380 Ω/phase (~120–185 mA); bench
  measurement is tracked in issue #101. The staggering requirement
  stands regardless. Power-path decisions (Q1 topology, off-master
  fuse-block option, polyfuse re-sizing) are tracked in issue #103.
- Master internally distributes 12 V to each row via per-row polyfuses
  (4 A hold each), then out on the row's 4-pin connector alongside the
  row's RS-485 A/B.
- Master also taps off ~500 mA for its own 3.3 V logic via on-board
  K7803-1000R3 switching buck (1 A version — 500 mA was too tight
  with ESP32-S3 WiFi peaks).
- 12 V flows from the master through the combined cable to the
  harness's master end, then continues through the trunk to every unit.

This means:
- One brick. One master. One cable per row.
- Per-row polyfuse on the master gives row-level fault isolation: a
  shorted row recovers automatically without affecting the others.
- Master physical placement is constrained to "near the brick" (the
  high-current input cable wants to be short and stiff).

## Addressing

Each STM32G030K8T6 has a unique 96-bit UID baked into silicon (read from
`UID_BASE` register, 3x32-bit words). Units identify themselves by UID.

**Master holds a UID -> (row, column) mapping table** persisted in flash.

### IDENTIFY button (commissioning hardware)

Each unit board has:
- A momentary push-button on the unit PCB (e.g. tact switch on edge).
- A dedicated IDENTIFY LED (or reuse FAULT LED with pulse pattern).
- 1 GPIO from the MCU reads the button state.

The button is the **dead-simple manual disambiguation** for UID
assignment. It has no addressing role at runtime — it is only used during
commissioning and re-commissioning.

### Commissioning flow

1. Power-up. All units boot, send "I'm here, my UID is X" to their bus
   master (the master sees one bus per row **pair** and tags each UID
   with its bus index; the IDENTIFY flow below disambiguates row and
   column within the pair — no UX change vs the earlier 1-bus-per-row
   scheme).
2. Master web UI shows: bus A (rows 0+1) has 32 unknown units
   (UIDs A, B, C, ...).
3. User clicks "assign row 0 column 0". Master broadcasts on bus A:
   "whoever has IDENTIFY pressed, respond". If no responder, master
   pulses the IDENTIFY LED on all bus-A units to prompt.
4. User presses IDENTIFY on the physical unit they want to be column 0.
5. Pressed unit responds with its UID + IDENTIFY-pressed flag.
6. Master writes to flash: UID = (row 0, column 0).
7. Repeat for all 64 units (or batch-flow: master walks the user through
   all 16 columns of each row).
8. Map persists. Subsequent boots use the stored map without
   re-commissioning.

### Auto-discovery (optional, firmware-only)

Firmware may add automatic UID-discovery later (e.g., binary-tree prefix
match). The IDENTIFY button is the guaranteed-working fallback that does
not depend on collision-handling logic.

### What is explicitly NOT used

- DIP switches.
- Solder jumpers.
- SLOT_ID / ROW_ID pins on the harness.
- Backplane wiring of any kind.
- Firmware-stored EEPROM addresses set during one-off provisioning.

## Unit-to-bus contact (pogo pins)

Units have no connector to the bus. Each unit contacts the DIN-rail
bus PCB via 4 spring-loaded pogo pins on its underside (see
`BUS_PCB.md` for the station geometry and contact zones):

| Pogo | Net |
|---|---|
| 1 (top) | 12V (from master, daisy-chained through bus PCBs) |
| 2 | RS485_A |
| 3 | RS485_B |
| 4 (bottom) | GND |

The IDENTIFY button is on the unit board itself. Net count is
intentionally minimal.

> **⚠ GEOMETRY UNRESOLVED — issue #100**: the unit outline vs the
> 37 mm station pitch is impossible as currently documented; station
> pad geometry and unit orientation WILL be redesigned after a
> dimensioned cross-section + mock-up. The 4-net pogo order above is
> the electrical contract; do not lay out against the current
> physical pattern.

## Boot-time chain self-test

At boot, master polls each bus with a "report yourselves" broadcast.
Every unit on the bus responds with its UID + bus index. Master
counts responders per bus; web UI shows "row N has K of 16 units present"
indicators before the system tries to display anything.

## Hardware-only comparison to v1

| Concern | v1 | v2 |
|---|---|---|
| Master MCU | ESP-01 (ESP8266) | ESP32-S3-WROOM-1-N16R8, native UART1/UART2 drive the buses (SC16IS740 expander deleted 2026-07-04, #102) |
| Master programming | OTA only | USB-C native CDC + OTA |
| Master power | 5 V to master + shared 12 V brick | one 12 V / 15 A brick to master, master distributes to rows |
| Display capacity | 1 case, 16 units | 4 rows, 64 units |
| Bus | I2C 400 kHz, star, 5 V, single case | 2x RS-485 half-duplex, one per row pair (rows 0+1, rows 2+3) |
| Row power | from master via shared rail | from master, per-row polyfuse, single combined cable carries 12V + signal |
| Unit MCU | ATmega328P (Arduino Nano) | STM32G030K8T6 (UID required; 64 KB flash for OTA headroom) |
| Unit motor | 28BYJ-48 5 V | 28BYJ-48 12 V |
| Unit power input | 5 V + 12 V dual rail | 12 V single rail |
| Unit address source | 4-bit DIP switch (manual config per unit) | 96-bit UID + IDENTIFY button (commissioning) |
| Distribution | ~30 patch cables per case | per-row daisy-chain harness |
| Homing sensor | KY-003 module (discrete, flying lead) | KY-003 module (discrete, flying lead via 3-pin connector) — same mechanical alignment path as v1 |
| Unit ESD on bus | none | SM712-02HTG on RS-485 A/B |
| Connector to bus | JST-XH chained per unit | 4 pogo pins on unit underside (no connector) |
| Distribution within row | n/a | 2× 300 mm DIN-rail bus PCBs daisy-chained, units clip on |
| Cable from master to row | n/a | single 4-pin combined (12V/GND/A/B) to first bus PCB |
| Cable between bus PCBs | n/a | short 4-pin daisy-chain cable |

## Hardware-only comparison to scottbez1

scottbez1 uses shift-register-only modules (no per-unit MCU), one chain,
ribbon cable harness. v2 keeps per-unit MCU so each unit can hold its own
homing state, run self-tests, and respond by UID. v2 borrows the
28BYJ-48 12 V choice.
