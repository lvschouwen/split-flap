# Open Decisions

Decisions that block schematic capture. Each one is a hardware choice
(software is being rewritten; firmware-reuse arguments do not apply).

## 1. Motor driver

**Options**
- A. TPL7407L (modern N-MOSFET array)
- B. ULN2003A (Darlington array)

**Hardware tradeoffs**
- Pin-compatible 16-SOIC; PCB layout identical.
- TPL7407L: ~12 mW per coil-on at 200 mA. ULN2003A: ~180 mW.
- ULN2003A: deepest stock at every distributor; v1 + scottbez1-proven.
- TPL7407L: JLC Basic, less stable historical availability.

**Recommendation: TPL7407L primary, ULN2003A as PCBA-time substitute.**
Footprint is shared.

## 2. Existing case inventory

**Action required.** Photograph and measure each row case.

What we need:
- Cable entry points for the row harness and 12 V brick.
- Internal layout to plan harness routing.
- Position of the master PCB physically in the system.
- Whether all 4 rows are in the same enclosure, separate enclosures, or
  some mix.

This drives harness length, cable gauge selection, and master output
cable length.

## 3. RS-485 baud rate

**Options**
- A. 115200 baud
- B. 250 kbaud (recommended)
- C. 500 kbaud

**Hardware tradeoffs**
- All three work cleanly on shielded twisted pair within a hobby
  installation.
- 250 kbaud over a daisy-chain harness with short stubs is well within
  spec for SN65HVD75 + SC16IS740.
- Lower baud = more SI margin and easier scope debugging.

**Recommendation: 250 kbaud.** Software-changeable later without hardware
work.

## 4. IDENTIFY LED — dedicated or shared

**Options**
- A. Dedicated yellow LED + dedicated GPIO.
- B. Reuse FAULT LED with a distinctive pulse pattern.

**Hardware tradeoffs**
- A: 1 extra LED, 1 extra resistor, 1 extra GPIO. ~EUR 0.05.
- B: shared LED with a software-distinguished pulse pattern. Slight
  ambiguity if both states active simultaneously, but firmware can
  arbitrate.

**Recommendation: A (dedicated).** A handful of cents for a far clearer
commissioning UX. Reverse to B if board area is tight after layout.

## 5. Inter-row cable from master to row harness

The master's 4 row ports are 3-pin (A, B, GND). The harness's master end
takes 12 V + GND from the row brick separately.

**Decision: master row port is 3-pin shrouded header on master PCB.**
Cable from master port to harness signal end is 3-conductor shielded
twisted pair (e.g., a short length of CAT5e or 2-pair instrument cable).
Length: depends on physical layout — typically 0.3-2 m.

## 6. Harness trunk type

**Options**
- A. Round 4-conductor cable (22 AWG), unit drops T-tapped onto trunk.
- B. Flat ribbon cable (28 AWG, 1.27 mm pitch), IDC mass-terminated drops.

**Hardware tradeoffs**
- Round is more rugged, handles higher 12 V current, but each drop is
  a hand soldering job.
- Ribbon is faster to assemble (mass-termination tool) but limited to
  short rows due to 28 AWG wire's 12 V drop.

**Recommendation: round 22 AWG for any row > 1 m. Ribbon for compact
single-cabinet rows.** Final pick depends on case inspection (#2).

## Resolved decisions (closed; recorded for transparency)

- **Display capacity**: 4 rows × 16 units = 64 units max.
- **Master MCU**: ESP32-S3-WROOM-1-N16R8.
- **Master programming interface**: USB-C native CDC.
- **Master power**: own 12 V supply, ~5 W. Master does not source row
  power.
- **Per-row power**: own 12 V brick per row (~5 A per row, sized to peak
  16-stepper current).
- **Bus type**: 4x RS-485 half-duplex. One bus per row.
- **4th UART**: **SC16IS740 single-channel UART expander** on master,
  driven by SPI. Not MAX14830. No software UART.
- **Addressing**: STM32 96-bit silicon UID per unit + IDENTIFY
  push-button on each unit board for commissioning. Master persists
  UID -> (row, column) map in flash.
- **No DIP switches, no solder jumpers, no slot wiring on harness, no
  rigid backplane.**
- **Unit MCU**: STM32G030K6T6 (LQFP-32). Required for hardware UID.
- **Unit motor**: 28BYJ-48 12 V variant.
- **Hall sensor**: A1101ELHL on-board.
- **Wiring**: per-row daisy-chain harness, 4 conductors (12V, GND, A, B).
- **Termination**: 120 ohm at master per bus + 120 ohm in a terminator
  plug at far end of each row's harness.
- **Bias**: 1k/1k at master only, per bus.
- **Unit-to-harness connector**: 4-pin shrouded box header (2x2, 2.54 mm,
  indexed).

## Deferred (firmware-only)

- RS-485 protocol opcode set + framing.
- Boot-time chain self-test scheme.
- Auto UID-discovery on top of the IDENTIFY-button fallback.
- OTA path for unit firmware over RS-485.
- Web UI scope for v2 master, including the commissioning wizard.
- Slot calibration storage in master.
