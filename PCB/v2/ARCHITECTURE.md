# v2 Architecture

## Target system

One master. Four rows. Sixteen units per row. **64 units total.**

Each row is a physically distinct case fed by its own 12 V brick. Each
row has its own RS-485 bus from the master — the master has 4 row ports.
No rigid backplane: each row uses a daisy-chain harness or ribbon cable.

## System block diagram

```
                       1× 12 V / 15 A brick
                              |
                              v
                   +----------------------+
                   |  Master              |
                   |  ESP32-S3 + SC16IS740|
                   |  USB-C               |
                   |  4x RS-485 PHY       |
                   |  per-row polyfuse    |
                   |  4x 6-pin row ports  |
                   +-+--+--+--+-----------+
                     |  |  |  |
                     v  v  v  v
                   single combined cable per row
                  (12V/GND/A/B in one 6-pin connector)
                     |  |  |  |
                     |  |  |  +---> Row 3 harness ---> 16 units --- [terminator]
                     |  |  +-----> Row 2 harness ---> 16 units --- [terminator]
                     |  +--------> Row 1 harness ---> 16 units --- [terminator]
                     +-----------> Row 0 harness ---> 16 units --- [terminator]

   Master sources both row power and signal from a single 12 V brick.
   Each row: 12V + GND + A + B daisy-chained through 16 units.
   Each unit has 1 connector (4-pin), 1 IDENTIFY button, 1 IDENTIFY LED.
```

## Bus topology

**4 RS-485 buses, one per row.** Each row has its own dedicated bus from
the master. Logical row index = bus index.

ESP32-S3 has 3 native UARTs. The 4th UART comes from an **SC16IS740
single-channel UART expander** on the master, accessed via SPI. SC16IS740
is a simple, well-supported IC — not MAX14830 — and adds ~EUR 2 to the
master BOM. See `OPEN_DECISIONS.md` #1 for the alternative paths
considered.

## Power flow

- **Single 12 V / 15 A brick** feeds the master. Sized for 64-unit
  worst-case peak (~16 A briefly during simultaneous flap transitions);
  steady-state draw is ~3-5 A.
- Master internally distributes 12 V to each row via per-row polyfuses
  (4 A hold each), then out on the row's 6-pin connector alongside the
  row's RS-485 A/B.
- Master also taps off ~50 mA for its own 3.3 V logic via on-board LDO.
- 12 V flows from the master through the combined cable to the
  harness's master end, then continues through the trunk to every unit.

This means:
- One brick. One master. One cable per row.
- Per-row polyfuse on the master gives row-level fault isolation: a
  shorted row recovers automatically without affecting the others.
- Master physical placement is constrained to "near the brick" (the
  high-current input cable wants to be short and stiff).

## Addressing

Each STM32G030K6T6 has a unique 96-bit UID baked into silicon (read from
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
   master (the master sees one bus per row and tags each UID with its
   bus / row index).
2. Master web UI shows: row 0 has 16 unknown units (UIDs A, B, C, ...).
3. User clicks "assign row 0 column 0". Master broadcasts on bus 0:
   "whoever has IDENTIFY pressed, respond". If no responder, master
   pulses the IDENTIFY LED on all bus-0 units to prompt.
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

## Unit-to-harness connector

4-pin shrouded box header (2x2), 2.54 mm, indexed (notched).

| Pin | Net |
|---|---|
| 1 | 12V (from row brick, daisy-chained through harness) |
| 2 | GND |
| 3 | RS485_A |
| 4 | RS485_B |

The IDENTIFY button is on the unit board itself, not on the connector.
Pin count is intentionally minimal.

If a future hardware feature requires more wires (e.g., remote reset),
the connector can be expanded to 6-pin (2x3) at design time without
changing this commissioning architecture.

## Boot-time chain self-test

At boot, master polls each row bus with a "report yourselves" broadcast.
Every unit on the bus responds with its UID + bus/row index. Master
counts responders per bus; web UI shows "row N has K of 16 units present"
indicators before the system tries to display anything.

## Hardware-only comparison to v1

| Concern | v1 | v2 |
|---|---|---|
| Master MCU | ESP-01 (ESP8266) | ESP32-S3-WROOM-1-N16R8 + SC16IS740 UART expander |
| Master programming | OTA only | USB-C native CDC + OTA |
| Master power | 5 V to master + shared 12 V brick | one 12 V / 15 A brick to master, master distributes to rows |
| Display capacity | 1 case, 16 units | 4 rows, 64 units |
| Bus | I2C 400 kHz, star, 5 V, single case | 4x RS-485 half-duplex, one per row |
| Row power | from master via shared rail | from master, per-row polyfuse, single combined cable carries 12V + signal |
| Unit MCU | ATmega328P (Arduino Nano) | STM32G030K6T6 (UID required) |
| Unit motor | 28BYJ-48 5 V | 28BYJ-48 12 V |
| Unit power input | 5 V + 12 V dual rail | 12 V single rail |
| Unit address source | 4-bit DIP switch (manual config per unit) | 96-bit UID + IDENTIFY button (commissioning) |
| Distribution | ~30 patch cables per case | per-row daisy-chain harness |
| Homing sensor | KY-003 module (discrete) | A1101ELHL on-board hall |
| Unit ESD on bus | none | SM712-02HTG on RS-485 A/B |
| Connector to bus | JST-XH chained per unit | 4-pin shrouded box header (1 connector per unit) |
| Cable from master to row | n/a | single 6-pin combined (12V/GND/A/B) per row |

## Hardware-only comparison to scottbez1

scottbez1 uses shift-register-only modules (no per-unit MCU), one chain,
ribbon cable harness. v2 keeps per-unit MCU so each unit can hold its own
homing state, run self-tests, and respond by UID. v2 borrows the
ribbon-harness wiring concept (`HARNESS.md`) and the 28BYJ-48 12 V choice.
