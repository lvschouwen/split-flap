# Open Decisions

**Revision:** 2026-07-09

Hardware decisions remaining for v2. Software is being rewritten;
firmware-reuse arguments do not apply.

**Schematic capture is unblocked.** Layout is **BLOCKED on #100**
(insertion-kinematics mock-up — see item 6; the pogo-vs-card-edge
question itself was decided 2026-07-09: **card-edge**). Other
remaining items are firmware-configurable or build-time decisions.

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

## 2. RS-485 baud rate — RESOLVED 2026-07-09

**Locked: 250 kbaud.** Software-changeable later without hardware work.
Well inside the THVD1410's 500 kbps slew-limited rating; more SI margin
and easier scope debugging than 500 k. Since 2026-07-04 (#102) the link
is native-UART-only on both ends (SC16IS740 deleted), and 250 kbaud
divides cleanly (~0.08 % baud error at 64 MHz on the unit side).

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

**Locked: JST-VH 4-pin (B4P-VH-A, LCSC C144392 — verify per #105).** 3.96 mm
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

MASTER_BOM.csv J3–J6 carry C144392 (verify per #105); the stale
kicad/*.net files that still carried C124378 were removed in #102.

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

## 6. Unit/rail insertion kinematics — issue #100 (BLOCKER — resolve before layout)

**Interface decision made 2026-07-09: card-edge** (see Resolved). The
mock-up is re-scoped from pogo preload/sag/coplanarity to **insertion
kinematics**: how the unit's card edge enters the backplane socket
while the clip engages the DIN rail — 3 stations, one dimensioned
cross-section (unit PCB → clip → rail → backplane → socket), clip
keep-out drawing. Not gated on any hardware purchase; needs a 3D
printer and cardboard. The pogo-era findings (sag under preload,
Z-stack, strip geometry) are moot; still live are the station-5 screw
collision and end connectors inside the end-station envelopes, which
carry over to the backplane layout.

## 7. Motor current measurement — issue #101

The 240 mA/unit figure is a paper number under review. Real 12 V
28BYJ-48 coils measure 130–380 Ω/phase (~120–185 mA/unit). Bench
measurement will re-base the power budget; the firmware staggering
requirement stands regardless.

## 8. Master reverse-block topology + fusing — issue #103

Q1 topology unresolved: the NTD2955 alternate was deleted as wrong
for the topology, and AOD409 is thermally unresolved at ≥12 A. Also
covers the off-master fuse-block option and polyfuse re-sizing once
#101 lands.

## 9. First-article gate — issue #104 (shrunk 2026-07-09)

The pogo-hardening programme (asymmetric pad pattern, wipe kinematics,
contact-resistance soak logging) dissolved with the card-edge decision.
What survives: clip material spec (no PLA; PETG min, prefer ASA/PC),
reversed-unit-refuses-to-seat check (socket key is the primary
polarization now), and a short insert/remove + vibration sanity test
on the first backplane + a few rev A units.

## 10. LCSC part-number verification — issue #105

All LCSC numbers still marked "CHECK" (C144392 and friends) must be
verified against the live catalogue before ordering.

## 11. Master J7 debug header — RESOLVED 2026-07-09: moot

The custom master board was cancelled (#103/#178/#181 closed; master =
S3 devkit + minimal carrier). J7 no longer exists as a decision.

## 12. Rail V/A monitoring on the devkit carrier — proposed 2026-07-13 (#253)

Add an **INA226** current-shunt monitor (I2C, +2 mΩ shunt) on the
carrier's 12 V rail, upstream of the row fuses. Rationale: the S3 has
no on-chip supply sensing, so brownout today is *inferred* from
unit-side lifetime reset counters (#139) — a rail monitor makes it a
measurement (live volts/amps tile on the firmware's System tab, and an
empirical answer to reflash-homing inrush questions like v1 #138 /
batch sizing #250). Decisions pending: shunt value vs. full-scale
range at 15 A, whether it shares the unit-bus I2C pins or gets its own,
and LCSC availability (INA226AIDGSR or the cheaper INA219 if 15 A
headroom allows an external shunt anyway).

## Resolved decisions (closed; recorded for transparency)

- **RS-485 transceiver — resolved 2026-07-09 (#179)**: **TI THVD1410**,
  SOIC-8 (D package) as the production footprint on unit and master
  carrier. 3–5.5 V supply (one part for the 3.3 V master and the unit),
  500 kbps slew-limited, true idle/open/short failsafe, ±18 kV IEC
  61000-4-2 contact on the bus pins. Same SOIC-8 pinout as the
  superseded SN65HVD75. Consequences: the master 1k/1k failsafe bias is
  **deleted** (96 mV into the 60 Ω loaded bus was below threshold
  anyway; the true-failsafe receiver makes it unnecessary), and the
  SM712 ESD arrays are **deleted** system-wide (integrated ±18 kV
  exceeds what they added, ~66 parts saved). The 10 kΩ DE pull-downs
  **stay** (deterministic bus release during MCU reset). Purchase:
  ~25× THVD1410DR from Mouser (595-THVD1410DR, restock ~2026-07-17) —
  bench + full-row production stock in one buy.
- **Unit↔bus interface — resolved 2026-07-09 (#100)**: **card-edge**,
  g000ze/Split-Flap-Display precedent; pogo-on-strips withdrawn. The
  unit PCB gets a beveled hard-gold card-edge (standard JLC gold-fingers
  option — same plating spec previously written for the bus strips);
  the bus PCB becomes a **backplane carrying one keyed socket per
  station**. Retires the pogo risk chain (preload, sag, coplanarity,
  fretting soak test); polarization becomes mechanical via the socket
  key instead of resting solely on the 3D-printed clip.
- **Card-edge connector — resolved 2026-07-09**: **2.54 mm dual-row,
  5 positions (2×5, 10 contacts)**. Net per position, mirrored on both
  card faces so every net has two contacts: 12V, GND, A, B, spare
  (routed to a test pad). Polarizing key slot between GND and A;
  GND fingers longest for first-make; card 1.6 mm. Sockets: Sullins
  EBC05-class / LCSC generic "card edge 2.54 mm 10P" — verify per #105.
- **Per-unit MCU re-affirmed 2026-07-09**: scottbez1/splitflap proves
  108 modules on dumb shift-register chains — considered and declined.
  UID addressing, self-contained swappable units, per-unit OTA and
  calibration keep the STM32G030K8 per unit.
- **Phase 2 vehicle — revised 2026-07-09 (#183)**: protocol development
  happens on **10× JLC-assembled unit rev A boards**, not dev boards /
  breakouts (NUCLEO/G030 core boards skipped). Order: #100 mock-up →
  lock geometry → KiCad capture → JLC. Bench flashing via ST-Link
  (needed for production programming regardless); SWD + UART test pads
  already in the unit BOM. #101 (motor current) is measured on a rev A
  board — real power path including F_unit.
- **Display capacity**: 4 rows × 16 units = 64 units max.
- **Master MCU**: ESP32-S3-WROOM-1-N16R8.
- **Master programming interface**: USB-C native CDC.
- **System power**: single 12 V / 15 A brick into master. Master sources
  power to all 4 rows via per-row polyfuses (4 A hold each). One cable
  per row carries 12V + GND + RS-485 A + B in a 4-pin combined
  connector.
- **Master placement**: near the brick (high-current input cable kept
  short).
- **Bus type — revised 2026-07-04 (#102)**: 2x RS-485 half-duplex,
  one per row **pair**. Bus A = rows 0+1 on ESP32-S3 native UART1;
  Bus B = rows 2+3 on UART2; UART0 = console only. Each bus runs
  far-end-of-row-N ↔ master ↔ far-end-of-row-N+1 with the master as
  an unterminated mid-bus tap. (Originally 4 buses, one per row.)
- **4th UART — SUPERSEDED 2026-07-04 (#102)**: the **SC16IS740
  single-channel SPI-UART expander** (chosen over MAX14830 / software
  UART for the earlier 4-bus scheme) is deleted along with the 4th
  bus. Reasons: (1) it cannot generate 250 kbaud from its
  14.7456 MHz crystal — the integer divisor tops out at 230,400
  nearby; (2) its RTS pin resets high, driving the bus at power-on;
  (3) SN65HVD75 is a 3/20 unit-load transceiver (up to 213 nodes),
  so 32 units per bus is trivial. Replaced by the 2-bus scheme on
  native UART1/UART2.
- **Addressing**: STM32 96-bit silicon UID per unit + IDENTIFY
  push-button on each unit board for commissioning. Master persists
  UID -> (row, column) map in flash.
- **No DIP switches, no solder jumpers, no slot wiring on harness, no
  rigid backplane.**
- **Unit MCU — revised 2026-07-04 (#102)**: STM32G030**K8**T6
  (LQFP-32, 64 KB flash, LCSC C431631) — upgraded from K6T6 for OTA
  headroom. Required for hardware UID.
- **Unit motor**: 28BYJ-48 12 V variant.
- **Hall sensor**: off-board 3-pin module on a
  flying lead (sensor part revised 2026-07-04 — see the 3.3 V hall
  entry below), plugged into J3 (JST-XH 3-pin) on the unit PCB.
  **J3 pinout LOCKED: pin 1 = +3V3, pin 2 = GND, pin 3 = HALL_OUT**
  (KY-003 native order — v1 cable plugs straight in). Magnet alignment
  set by chassis bracket, not the PCB. Matches v1 mechanical approach.
  Resolves 2026-04-26.
- **Wiring**: per-row daisy-chain harness, 4 conductors (12V, GND, A, B).
- **Termination — revised 2026-07-04 (#102)**: 120 Ω terminator plugs
  at both far ends of each 2-row bus; the master is an unterminated
  mid-bus tap (its on-board 120 Ω is deleted). Still one plug per
  row, 4 system-wide — hardware unchanged.
- **Bias — SUPERSEDED 2026-07-09 (#179)**: the 1k/1k failsafe at the
  master is deleted; the THVD1410's true failsafe replaces it.
- **Unit-to-bus contact — SUPERSEDED 2026-07-09 (#100)**: the pogo-pin
  system (4 THT pogos onto hard-gold strips) is withdrawn in favour of
  the card-edge interface (see 2026-07-09 entries above).
- **Per-row distribution**: 2× 300 mm backplane PCBs daisy-chained,
  both built from the same PCB design (8 boards across 4-row system),
  each with a JST-VH 4-pin connector (B4P-VH-A) on each end — now
  carrying 8 card-edge sockets each instead of contact strips.
- **All row power connectors locked to JST-VH 4-pin (B4P-VH-A,
  C144392 — verify per #105)** (3.96 mm pitch, ≥5 A per pin, keyed crimp
  housing): master row output J3-J6, both ends of every bus PCB, both
  ends of master cable + daisy-chain cable. Earlier JST-XH lock was
  reversed after ChatGPT review (3 A undersized for 4 A row fuse). Unit
  J2 stepper output stays JST-XH 5-pin (B5B-XH-A, C158013).
- **Termination**: 120 Ω resistor in a terminator plug that mates with
  the unused connector on the last bus PCB in each row's chain.
- **Motor driver**: TPL7407L primary; ULN2003A as PCBA-time substitute
  (footprint shared).
- **IDENTIFY LED**: dedicated yellow LED + dedicated MCU GPIO.
- **Unit LDO + TVS (resolved 2026-07-04, #102)**: LM2937IMP-3.3
  primary (LDL1117S33 as alternate), coordinated with an **SMAJ13A**
  TVS on the unit's 12 V input.
- **Hall sensor voltage rating (resolved 2026-07-04, #102)**: must be
  a 3.3 V-rated open-drain part (DRV5023 / AH3366Q) on the same J3
  flying lead. A genuine A3144-based KY-003 is NOT qualified at
  3.3 V.
- **DE pull-downs (resolved 2026-07-04, #102)**: 10 kΩ pull-down on
  DE at **all** RS-485 transceivers (master + units) so no node
  drives the bus while its MCU is in reset/boot.
- **Per-unit fuse (resolved 2026-07-04, #102)**: 0.5 A polyfuse
  F_unit on each unit's 12 V input.
- **Contact plating (resolved 2026-07-04, #102; carrier changed
  2026-07-09)**: hard/thick gold ("gold fingers", 0.3–0.8 µm Au over
  ≥2.5 µm Ni, or ENEPIG) — originally specced for the bus strips, now
  applied to the **unit's card-edge fingers**, where it is the standard
  JLC gold-fingers option rather than a special order.
- **Power-off insertion (resolved 2026-07-04, #102)**: units are
  inserted/removed with row power OFF — mandatory. TPL7407L COM is
  limited to 0.5 V/µs; hot-plug edges are 5–10 V/µs. (Unchanged by the
  card-edge switch; GND-first-make finger staggering adds margin, not
  permission.)
- **Master USB bench power (resolved 2026-07-04, #102)**: USB 5 V is
  OR-ed with the 12 V-derived rail so the master can be developed and
  flashed from USB without the brick.
- **NTD2955 deleted (resolved 2026-07-04, #102)**: the NTD2955
  alternate for the master reverse-block Q1 was wrong for the
  topology and is removed. Q1 topology itself remains unresolved
  (AOD409 thermally unresolved at ≥12 A) — tracked in issue #103.

## Deferred (firmware-only)

- RS-485 protocol opcode set + framing.
- Boot-time chain self-test scheme.
- Auto UID-discovery on top of the IDENTIFY-button fallback.
- OTA path for unit firmware over RS-485.
- Web UI scope for v2 master, including the commissioning wizard.
- Slot calibration storage in master.
