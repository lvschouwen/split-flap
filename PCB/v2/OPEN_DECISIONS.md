# Open Decisions

Hardware decisions remaining for v2. Software is being rewritten;
firmware-reuse arguments do not apply.

**Schematic capture is unblocked.** Remaining items are
firmware-configurable or build-time decisions.

## 1. Existing case inventory (deferred — does not block schematic capture)

Master placement is locked to "near the brick", but the brick + master
location relative to the rows is still undecided. Cable lengths follow
from that and are cable-assembly concerns, not PCB-schematic concerns.

When the master/brick mounting position is chosen, this becomes a
build-time spec:
- Cable lengths from master to each row's harness master-end.
- Cable entry points into row enclosures.
- Whether all 4 rows are in the same enclosure or split.

## 2. RS-485 baud rate

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

## 3. Master-to-harness cable (deferred with #1)

Cable lengths follow from master placement. Hardware spec is fixed:

- Master row port: 6-pin shrouded box header on master PCB.
- Harness master-end: matching 6-pin shrouded female.
- Cable: 4 active conductors (12V, GND, A, B). 22 AWG minimum on power
  legs; 24 AWG OK on A/B. Twisted pair on A/B preferred. Custom-built
  with 4-conductor instrument cable or CAT5e cut-down (1 pair power,
  1 pair GND parallel, 1 pair A/B twisted, 1 pair spare).
- Lengths: cut and terminated at build time.

## 4. Harness trunk type (deferred — harness assembly, does not block schematic capture)

**Options**
- A. Round 4-conductor cable (22 AWG), unit drops T-tapped onto trunk.
- B. Flat ribbon cable (28 AWG, 1.27 mm pitch), IDC mass-terminated drops.

**Hardware tradeoffs**
- Round: more rugged, handles higher 12 V current, hand-soldered drops.
- Ribbon: faster to assemble, limited to short rows (~1 m) due to thinner
  conductors' 12 V drop.

**Recommendation: round 22 AWG for any row > 1 m. Ribbon for compact
single-cabinet rows.** Decision happens at harness build, after master
placement is settled (#1/#3).

## Resolved decisions (closed; recorded for transparency)

- **Display capacity**: 4 rows × 16 units = 64 units max.
- **Master MCU**: ESP32-S3-WROOM-1-N16R8.
- **Master programming interface**: USB-C native CDC.
- **System power**: single 12 V / 15 A brick into master. Master sources
  power to all 4 rows via per-row polyfuses (4 A hold each). One cable
  per row carries 12V + GND + RS-485 A + B in a 6-pin combined
  connector.
- **Master placement**: near the brick (high-current input cable kept
  short).
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
- **Motor driver**: TPL7407L primary; ULN2003A as PCBA-time substitute
  (footprint shared).
- **IDENTIFY LED**: dedicated yellow LED + dedicated MCU GPIO.

## Deferred (firmware-only)

- RS-485 protocol opcode set + framing.
- Boot-time chain self-test scheme.
- Auto UID-discovery on top of the IDENTIFY-button fallback.
- OTA path for unit firmware over RS-485.
- Web UI scope for v2 master, including the commissioning wizard.
- Slot calibration storage in master.
