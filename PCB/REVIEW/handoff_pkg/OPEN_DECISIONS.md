# Open Decisions

**Revision:** 2026-04-26

Hardware decisions remaining for v2. Software is being rewritten;
firmware-reuse arguments do not apply.

**Schematic capture is unblocked.** Remaining items are
firmware-configurable or build-time decisions.

## 1. Existing case inventory (deferred — does not block schematic capture)

Master placement is locked to "near the brick", but the brick + master
location relative to the rows is still undecided. Cable lengths and
DIN rail mounting positions follow from that.

When the master/brick mounting position is chosen, this becomes a
build-time spec:
- Cable lengths from master to each row's first bus PCB.
- DIN rail mounting position in each row.
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

## 3. Master-to-bus cable + daisy-chain cable (deferred with #1)

Cable lengths follow from master and bus-PCB placement. Hardware spec
is fixed:

- Connectors: 4-pin shrouded female on every cable end (matching the
  4-pin headers on master and bus PCBs).
- Master cable: 4 conductors (12V, GND, A, B). 22 AWG minimum on power
  legs.
- Daisy-chain cable: same 4 conductors, short (~5-15 cm) between the
  two bus PCBs in each row.
- Twisted pair on A/B preferred for noise immunity.
- Lengths: cut and terminated at build time.

## 4. Row connector family — RESOLVED 2026-04-26 (revised post-ChatGPT review)

**Locked: JST-VH 4-pin (B4P-VH-A, LCSC C144392 — CHECK).** 3.96 mm
pitch, ≥5 A per pin (matches the 4 A row polyfuse with margin). Keyed
crimp housing.

**Earlier JST-XH lock REVERSED.** JST-XH is 3 A per pin and the row is
fused at 4 A — ChatGPT review flagged this as an undersize. JST-VH is
the smallest JST family rated above 4 A.

Used on:
- Master row output ports (J3-J6).
- Both ends of every bus PCB (J_in / J_out).
- Both ends of master cable + daisy-chain cable (matching female crimp
  housings on JST-VH crimp shells, e.g. VHR-4N).

**Unit J2 stepper output stays JST-XH 5-pin** (B5B-XH-A, C158013) —
that connector only sees ~250 mA at the 28BYJ-48, well within JST-XH
rating, and matches the existing v1 chassis ergonomics for the stepper.

The earlier `C124378` reference in `MASTER_BOM.csv` for master J3-J6
is obsolete — the verification checklist in `EASYEDA_HANDOFF.md`
flags this for correction before Gerbers are generated.

## 5. DIN rail and clip mounting hardware (build-time)

- **DIN rail**: standard 35 mm TS35, cut to row length (~600 mm).
- **Bus PCB mounting**: 2× corner brackets per bus PCB, attaching the
  PCB inside or alongside the DIN rail channel. Mechanical fit
  finalized when case mounting position is chosen.
- **Unit DIN rail clip — LOCKED**: 3D-printed asymmetric clip from
  MakerWorld (STL link to be supplied in build notes; one printed per
  unit). Bolts to the unit board through the 4× M3 holes at the locked
  v1 corner positions (3, 3), (3, 77), (37, 77), (37, 3) mm. **The
  clip is the system's only polarization mechanism** (PG_KEY pogo
  approach was withdrawn after ChatGPT review — see `BUS_PCB.md`
  § Polarization).

These are build-time selections; no PCB schematic dependency.

## Resolved decisions (closed; recorded for transparency)

- **Display capacity**: 4 rows × 16 units = 64 units max.
- **Master MCU**: ESP32-S3-WROOM-1-N16R8.
- **Master programming interface**: USB-C native CDC.
- **System power**: single 12 V / 15 A brick into master. Master sources
  power to all 4 rows via per-row polyfuses (4 A hold each). One cable
  per row carries 12V + GND + RS-485 A + B in a 4-pin combined
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
- **Hall sensor**: off-board 3-pin module (KY-003 or compatible) on a
  flying lead, plugged into J3 (JST-XH 3-pin) on the unit PCB.
  **J3 pinout LOCKED: pin 1 = +3V3, pin 2 = GND, pin 3 = HALL_OUT**
  (KY-003 native order — v1 cable plugs straight in). Magnet alignment
  set by chassis bracket, not the PCB. Matches v1 mechanical approach.
  Resolves 2026-04-26.
- **Wiring**: per-row daisy-chain harness, 4 conductors (12V, GND, A, B).
- **Termination**: 120 ohm at master per bus + 120 ohm in a terminator
  plug at far end of each row's harness.
- **Bias**: 1k/1k at master only, per bus.
- **Unit-to-bus contact**: 4 through-hole pogo pins on unit underside,
  vertical line, ~8 mm pitch, gold-plated tips. Contacts ENIG-plated
  trace strips on the DIN-rail bus PCB.
- **Per-row distribution**: 2× 300 mm bus PCBs daisy-chained, both
  built from the same PCB design (8 boards across 4-row system), each
  with 4-pin shrouded box header on each end.
- **All row power connectors locked to JST-VH 4-pin (B4P-VH-A,
  C144392 — CHECK)** (3.96 mm pitch, ≥5 A per pin, keyed crimp
  housing): master row output J3-J6, both ends of every bus PCB, both
  ends of master cable + daisy-chain cable. Earlier JST-XH lock was
  reversed after ChatGPT review (3 A undersized for 4 A row fuse). Unit
  J2 stepper output stays JST-XH 5-pin (B5B-XH-A, C158013).
- **Termination**: 120 Ω resistor in a terminator plug that mates with
  the unused connector on the last bus PCB in each row's chain.
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
