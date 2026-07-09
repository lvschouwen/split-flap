# Unit PCB — Schematic + Layout Specification

**Revision:** 2026-07-09

Tool-agnostic spec for the unit PCB. **64 of these per system; one
PCB design.** See `KICAD_HANDOFF.md` for the KiCad 10 build steps.

> **2026-07-09 amendments (decision sweep, #179/#100):**
> **U4 = THVD1410** (same SOIC-8 pinout as the superseded SN65HVD75;
> true failsafe, ±18 kV IEC bus ESD) — **D5 SM712 is deleted**, A/B
> route straight from U4 to the contacts. The **pogo-pin system
> (PG1–PG4) is superseded by a 2×5 card-edge** (2.54 mm dual-row,
> 12V/GND/A/B/spare mirrored on both faces, keyed, GND first-make,
> bevel + hard gold). Where this document says PG1/PG2/PG3/PG4, read
> the 12V/A/B/GND card-edge fingers; exact edge geometry comes from
> the #100 insertion-kinematics mock-up.

## Component list (with LCSC starting points)

| Ref | Value | Footprint | LCSC# (CHECK) | Notes |
|---|---|---|---|---|
| U1 | STM32G030K8T6 | LQFP-32 | C431631 | MCU; required for silicon UID. **K8 (64 KB flash) supersedes the earlier K6T6 (32 KB)**: deferred OTA-over-RS-485 needs a bootloader + A/B app slots; the G0 has no dual-bank flash; 32 KB has no headroom. Same LQFP-32 package/pinout. **Use the official KiCad library symbol** (`MCU_ST_STM32G0:STM32G030K8Tx`), not a hand-drawn symbol. |
| U2 | TPL7407L | **SOIC-16 (narrow, 150 mil body)** | C383290 | Stepper driver primary; ULN2003A drop-in alt: C2358. **Both parts ship in standard narrow SOIC-16 (150 mil body / 3.9 mm). Do NOT use SOIC-16W (300 mil / 7.5 mm) — pads will not bridge the IC's leads.** |
| U3 | LM2937IMP-3.3 | SOT-223 | CHECK per #105 | **12V→3.3V LDO — PRIMARY** (TI, **26 V operating / 60 V transient**). **Pinout (LM1117 family convention) — LOCKED: pin 1 = GND, pin 2 = VOUT (= tab), pin 3 = VIN.** Heatsink copper pour goes on **pin 2 + tab (VOUT, 3V3 net)**, NOT on GND — they're internally tied; pouring tab to GND shorts 3V3 to GND. Dissipation at 55 mA load = 0.48 W; SOT-223 with ~1 cm² VOUT pour holds θJA ~50 °C/W. **Alternate: LDL1117S33TR** (ST, C434348, 18 V op max / 20 V abs max, same SOT-223, same pinout convention) — demoted from primary because D4's clamp voltage must stay under the LDO rating: SMAJ13A's VC max 21.5 V fits the 26 V LM2937, whereas the superseded SMAJ15A's VC max 24.4 V exceeded LDL1117's 20 V abs max. **Do NOT substitute HT7833 / AMS1117 / MCP1825 — wrong VIN ratings** (HT7833 is 6.5 V max VIN — destroyed at 12 V). |
| U4 | THVD1410DR | SOIC-8 | CHECK per #105 | RS-485 transceiver — **locked 2026-07-09 (#179)**: 3–5.5 V supply, 500 kbps slew-limited, true idle/open/short failsafe, ±18 kV IEC contact on bus pins. Same SOIC-8 pinout as the superseded SN65HVD75. Deletes D5 (SM712) and the master-side failsafe bias. |
| J3 | Hall connector | JST-XH 3-pin male, vertical THT (B3B-XH-A) | C145756 | 3-pin header for external hall sensor module on flying lead. **Pinout LOCKED: pin 1 = +3V3, pin 2 = GND, pin 3 = HALL_OUT** — matches KY-003 module native order so v1's existing flying-lead cable plugs straight in. Different sensor modules adapt at the cable end. **Sensor spec: 3.3 V-capable open-drain hall required** (TI DRV5023 / Diodes AH3366Q / Honeywell SS361RT on a KY-003-style 3-wire lead) — a genuine KY-003 carries an A3144 (4.5 V min supply) and is NOT qualified at 3.3 V; v1 modules must be individually bench-verified or replaced. |
| Q1 | AO3401A | SOT-23 | C15127 | P-FET reverse-block. **VGS rated only ±12 V** — at nominal 12 V brick the gate-source margin is zero. Add Z1 10 V Zener clamp gate→source to absorb brick tolerance + transients; see Power section. |
| Z1 | BZT52C10 (or MMSZ5240B) | SOD-123 | C8062 (CHECK) | **10 V** Zener clamp on Q1 VGS — cathode → source, anode → gate. **10 V (not 12 V)** to give margin against AO3401A's ±12 V VGS abs-max under Zener tolerance (BZT52C12 +10% upper bound = 13.2 V puts the FET out of spec; cross-validated by Gemini + ChatGPT external review 2026-04-26). |
| F_unit | Polyfuse 0.5 A hold / ~1 A trip | 1206 SMD | CHECK per #105 | Per-unit input polyfuse, in series: **12 V card-edge finger → F_unit → Q1 → PCB-12V rail**. A shorted unit no longer takes down its whole row, and self-identifies (its LEDs go dark). **Sizing under review (#180)** — resize after #101 measures a rev A board (likely 0.75–1 A hold in 1812). |
| D1 | LED blue | 0805 | C2293 | HEARTBEAT |
| D2 | LED red | 0805 | C2286 | FAULT |
| D3 | LED yellow | 0805 | C2298 | IDENTIFY |
| D4 | SMAJ13A | DO-214AC SMA | CHECK per #105 | 12V-rail TVS unidirectional, **13 V standoff** (above 12 V + 5% brick tolerance), VC max 21.5 V — coordinated with the 26 V LM2937 LDO. **Supersedes SMAJ15A**, whose VC max 24.4 V exceeded LDL1117's 20 V abs max. **Placement LOCKED to post-Q1 (load-side PCB-12V rail)** — cathode (banded end) → PCB-12V (post-Q1 source), anode → GND. (Earlier draft said "post-pogo, before Q1"; that contradicted the net diagram below — ChatGPT external review caught it 2026-04-26.) Wrong-way orientation forward-biases and shorts the rail. |
| SW1 | Tact switch 6×6 | SMD 6x6x5 | C318884 | IDENTIFY button |
| CE | Card-edge fingers 2×5 | PCB feature (beveled edge, hard gold) | n/a | **Locked 2026-07-09 (#100)** — 2.54 mm pitch, 5 positions, fingers mirrored on both card faces (10 contacts): 12V, GND, A, B, spare (spare → test pad). Polarizing key slot between GND and A; **GND fingers longest (first-make)**; 1.6 mm card; hard gold 0.3–0.8 µm Au over ≥2.5 µm Ni (standard JLC gold-fingers option). Replaces PG1–PG4 pogo pins. Edge position/orientation from the #100 mock-up. Mating socket is on the backplane. |
| J2 | JST-XH **5-pin** male | THT B5B-XH-A | C158013 | Stepper output to 28BYJ-48: pins 1-4 = coil drives from TPL7407L OUT1-OUT4, **pin 5 = +12 V** (carries the 28BYJ-48's red wire from the unit's internal 12 V rail). Avoids 64× hand-splicing the +12 V wire into the chassis harness. |
| C_in | 22 µF / 25 V X7R | **1206** | C45783 (CHECK: confirm 1206 22 µF/25 V X7R is in JLC stock; 22 µF/25 V/X7R/0805 is effectively unobtainable) | 12V input bulk |
| C_in2 | 100 nF X7R | 0603 | C14663 | 12V decap |
| C_ldo_in | 10 µF / 25 V X7R | 0805 | C19702 | LDO input |
| C_ldo_out | 10 µF / 10 V X7R | 0805 | C15850 | LDO output |
| C_decap (×4) | 100 nF X7R | 0603 | C14663 | MCU + transceiver decoupling |
| C_rst | 100 nF X7R | 0603 | C14663 | NRST filter |
| C_id | 100 nF X7R | 0603 | C14663 | IDENTIFY button debounce |
| R_hall | 10 kΩ 1% | 0603 | C25804 | Hall sensor pull-up |
| R_de | 1 kΩ 1% | 0603 | C21190 | DE driver line series (PA12 → THVD1410 DE) |
| R_de_pd | 10 kΩ 1% | 0603 | C25804 | **DE pull-down to GND** — STM32 GPIOs float as analog inputs during reset; a floating DE can jam the bus. Kept even if the THVD1410 has internal biasing (verify datasheet at capture) — an explicit 10 k is deterministic |
| R_led (×3) | 1 kΩ 1% | 0603 | C21190 | LED current limit |
| R_id | 10 kΩ 1% | 0603 | C25804 | IDENTIFY button pull-up |
| R_rst | 10 kΩ 1% | 0603 | C25804 | NRST pull-up |

## Net-by-net wiring

### Power section

```
CE 12V finger (12V_IN) ── F_unit polyfuse (0.5 A hold / ~1 A trip, 1206) ── Q1 drain (P-FET, high-side reverse-block)
                              │
                              Q1 source ── PCB-12V rail ──┬── D4 SMAJ13A cathode (banded → +12V; anode → GND)
                                                           ├── C_in 22 µF
                                                           ├── C_in2 100 nF
                                                           ├── U2 TPL7407L pin 9 (COM)
                                                           ├── J2 pin 5 (28BYJ-48 +12V common)
                                                           └── U3 LM2937IMP-3.3 input (pin 3 = VIN; pin 1 is GND)

Q1 AO3401A (P-FET high-side reverse-block, standard topology):
  drain  ← PG1 (incoming 12V)
  source → PCB-12V rail (load side)
  gate   ── R_q1g 100 Ω series ── (gate node) ── R_q1g2 10 kΩ to GND
  Z1     10 V Zener: cathode → source (PCB-12V), anode → gate node
                     (clamps |VGS| at ~10 V to keep AO3401A safely inside its
                      ±12 V abs-max — a BZT52C12's +10% tolerance would push
                      the FET out of spec, per external review 2026-04-26)

  Same topology + behaviour as the master Q1 — see SCHEMATIC_MASTER.md
  power section for the full rationale. Do NOT pull the gate to +12V;
  that would defeat the reverse-block. The series 100 Ω + Zener clamp
  protects against brick over-tolerance and inrush transients (without
  the Zener, AO3401A operates with zero VGS margin at 12 V nominal).

D4 SMAJ13A polarity (CRITICAL):
   cathode (banded end) → +12V_RAIL
   anode                → GND
  Reversed orientation forward-biases the diode and shorts the rail.
  Mark on silkscreen with a polarity arrow + verify on first article.

U3 LM2937IMP-3.3 (PRIMARY — 26 V operating / 60 V transient, SOT-223,
                  LCSC code CHECK per #105):
   pin 1               → GND
   pin 2 (VOUT, = tab) → 3V3 + C_ldo_out 10 µF + per-IC decoupling
                         **Heatsink copper pour goes here on pin 2 + tab**
   pin 3 (VIN)         ← PCB-12V (post-Q1 source)
  Pinout LOCKED to LM1117-family convention (pin 1=GND, pin 2=OUT=tab,
  pin 3=IN). Earlier draft had pin 2/pin 3 swapped AND called the tab
  "GND" — pouring the tab to GND shorts 3V3 to GND through the
  internally-tied tab/pin 2 node. ChatGPT external review caught this
  2026-04-26; verify the wiring at capture.
  Thermal: 0.48 W dissipation at 55 mA load; with ~1 cm² VOUT-pour
  heatsink (θJA ~50 °C/W) the junction sits ~24 °C above ambient —
  well within the 125 °C Tj limit. LM2937 is primary so the D4
  SMAJ13A clamp (VC max 21.5 V) stays inside the LDO rating; the
  former primary LDL1117S33TR (18 V op / 20 V abs max) remains a
  listed alternate — it was superseded because the earlier SMAJ15A's
  VC max 24.4 V exceeded its 20 V abs max. **NOT HT7833** (HT7833 is
  6.5 V max VIN — would be destroyed at 12 V).

PG4 (bottom pogo, GND) ── PCB-GND plane
```

**Hot-swap is prohibited.** Power the row off before inserting or
removing a unit: hot-plugging charges the 22 µF input cap through
~50–100 mΩ of edge-contact resistance ≈ 5–10 V/µs on the rail —
the TPL7407L's COM pin abs max is 0.5 V/µs. (GND-first finger
staggering adds margin, not permission.)

### MCU (STM32G030K8T6, LQFP-32)

**Pin map corrected against ST DS12991** ("STM32G030KxT LQFP32 pin
definition"). An earlier draft claimed to be "locked against DS12991"
but had pins 1–17 wrong — it invented a second VDD pin and a
"PF2/NRST" pin; see the corrections list below. The K-suffix LQFP-32
package puts **PC6 at pin 20** between PA9 (pin 19) and PA10
(pin 21) — there is **no PA11/PA12 → PA9/PA10 SYSCFG remap
dependency**; USART1 TX/RX/DE go directly to PA9/PA10/PA12 at AF1.
**Use the official KiCad library symbol for the STM32G030K8T6**
(`MCU_ST_STM32G0:STM32G030K8Tx`) rather than a hand-drawn symbol —
it encodes this pinout.

**Functional assignment:**

| Function | STM32 signal | Alt. function | Notes |
|---|---|---|---|
| UART_TX | USART1_TX (PA9) | AF1 | Direct, no remap |
| UART_RX | USART1_RX (PA10) | AF1 | Direct, no remap |
| UART_DE | USART1_RTS_DE_CK (PA12) | AF1 | Hardware-toggled DE |
| (no /RE GPIO) | — | — | Tie THVD1410 /RE to GND (always-receive); firmware discards TX echo on RX |
| STEPPER_IN1..4 | PA4 / PA5 / PA6 / PA7 | GPIO | TPL7407L inputs |
| HALL_IN | PB0 | GPIO + EXTI | Hall sensor with 10 kΩ pull-up |
| IDENTIFY_BTN | PA8 | GPIO + EXTI | 10 kΩ pull-up + 100 nF debounce |
| LED_HEARTBEAT | PB1 | GPIO out | **sink-driven** (GPIO → cathode) |
| LED_FAULT | PB2 | GPIO out | **sink-driven** (GPIO → cathode) |
| LED_IDENTIFY | PA15 | GPIO out | **sink-driven** (GPIO → cathode) |
| SWDIO | PA13 | SYS | Test pad |
| SWCLK | PA14 | SYS | Test pad. **PA14 is SWCLK _and_ BOOT0** — the nBOOT_SEL option bit must remain 1 (factory default) so the pin is ignored at boot; SWD provisioning of virgin parts works via the empty-check |
| NRST | NRST (dedicated pin 6) | reset | 10 kΩ pull-up + 100 nF + SWD pad |

**Pin-by-pin LQFP-32 K-suffix map** (per ST DS12991):

| Pin | Function | Net |
|---|---|---|
| 1 | PB9 | NC |
| 2 | PC14-OSC32_IN | NC |
| 3 | PC15-OSC32_OUT | NC |
| 4 | **VDD/VDDA** | 3V3 — the **single** supply pin on this package; consolidate the MCU decaps here (100 nF ×2 within 3 mm) |
| 5 | **VSS/VSSA** | GND |
| 6 | **NRST** | NRST: 10 kΩ pull-up + 100 nF + SWD pad (dedicated reset pin — "PF2" does not exist on this part) |
| 7 | PA0 | NC (spare) |
| 8 | PA1 | NC (spare) |
| 9 | PA2 | NC (spare) |
| 10 | PA3 | NC (spare) |
| 11 | PA4 | STEPPER_IN1 → U2 pin 1 |
| 12 | PA5 | STEPPER_IN2 → U2 pin 2 |
| 13 | PA6 | STEPPER_IN3 → U2 pin 3 |
| 14 | PA7 | STEPPER_IN4 → U2 pin 4 |
| 15 | PB0 | HALL_IN ← J3 pin 3 (external hall OUT, 10 kΩ R_hall pull-up to 3V3) |
| 16 | PB1 | LED_HEARTBEAT → **D1 cathode** (sink); D1 anode → R_led 1 kΩ → 3V3 |
| 17 | PB2 | LED_FAULT → **D2 cathode** (sink); D2 anode → R_led 1 kΩ → 3V3 |
| 18 | PA8 | IDENTIFY_BTN ← SW1 (10 kΩ pull-up + 100 nF debounce) |
| 19 | **PA9** | UART_TX (USART1_TX, AF1) → THVD1410 D (pin 4) |
| 20 | **PC6** | NC (spare) — sits between PA9 and PA10 on K-suffix package |
| 21 | **PA10** | UART_RX (USART1_RX, AF1) ← THVD1410 R (pin 1) |
| 22 | **PA11** | NC (spare) — leave unconnected unless deliberately used |
| 23 | **PA12** | UART_DE (USART1_RTS_DE_CK, AF1) → THVD1410 DE (pin 3) via 1 kΩ R_de |
| 24 | PA13 (SWDIO) | SWDIO test pad |
| 25 | PA14 (SWCLK, **also BOOT0**) | SWCLK test pad — nBOOT_SEL option bit must remain 1 (factory default) so BOOT0 is ignored at boot; SWD provisioning of virgin parts works via the empty-check |
| 26 | PA15 | LED_IDENTIFY → **D3 cathode** (sink); D3 anode → R_led 1 kΩ → 3V3 |
| 27 | PB3 | NC |
| 28 | PB4 | NC |
| 29 | PB5 | NC |
| 30 | PB6 | NC |
| 31 | PB7 | NC |
| 32 | PB8 | NC |

**Important corrections vs. earlier drafts:**
- Earlier drafts had pins 1–17 wrong despite claiming a DS12991 lock:
  they invented **two** VDD pins (at 1 and 17, each with its own decap
  set) — this package has exactly **one** VDD/VDDA pin (**pin 4**,
  with VSS/VSSA at pin 5); the decaps are consolidated at pin 4.
- Earlier drafts placed "PF2/NRST" at pin 4 — **PF2 does not exist on
  this part**; **pin 6 is a dedicated NRST**.
- Earlier drafts placed PA10 at pin 20; **pin 20 is PC6**, not PA10.
- Earlier drafts assumed a PA11/PA12 → PA9/PA10 SYSCFG remap. **No
  remap is needed** — PA9/PA10/PA12 are at their package-pin-labelled
  positions (19/21/23) and route directly to USART1 AF1.
- LED nets terminate at the LED **cathode** (sink-driven). Earlier
  drafts inconsistently said "→ D_n anode". The driving direction is
  fixed: 3V3 → R_led → anode → cathode → MCU GPIO (sinks low to
  illuminate).

`/RE` is **not** wired to a GPIO — THVD1410 pin 2 is tied permanently
to GND. The firmware listens to its own TX echo and discards it,
which is standard half-duplex practice and saves a GPIO + a series
resistor.

### TPL7407L (stepper driver, SOIC-16 narrow)

| Pin | Function | Net |
|---|---|---|
| 1 | IN1 | STEPPER_IN1 ← MCU PA4 |
| 2 | IN2 | STEPPER_IN2 ← MCU PA5 |
| 3 | IN3 | STEPPER_IN3 ← MCU PA6 |
| 4 | IN4 | STEPPER_IN4 ← MCU PA7 |
| 5 | IN5 | GND (unused channel) |
| 6 | IN6 | GND (unused channel) |
| 7 | IN7 | GND (unused channel) |
| 8 | GND | GND |
| 9 | COM | 12V (motor common) |
| 10 | OUT7 | NC |
| 11 | OUT6 | NC |
| 12 | OUT5 | NC |
| 13 | OUT4 | J2 pin 4 (motor coil 4) |
| 14 | OUT3 | J2 pin 3 (motor coil 3) |
| 15 | OUT2 | J2 pin 2 (motor coil 2) |
| 16 | OUT1 | J2 pin 1 (motor coil 1) |

J2 (JST-XH **5-pin**) carries the 4 motor coil drive lines plus the
+12 V supply to the 28BYJ-48 stepper:

| J2 pin | Wire colour (28BYJ-48) | Net | Source |
|---|---|---|---|
| 1 | orange | coil 1 | TPL7407L pin 16 (OUT1) |
| 2 | yellow | coil 2 | TPL7407L pin 15 (OUT2) |
| 3 | pink   | coil 3 | TPL7407L pin 14 (OUT3) |
| 4 | blue   | coil 4 | TPL7407L pin 13 (OUT4) |
| 5 | red    | +12 V common | PCB-12V rail (post-Q1) |

Routing pin 5 to the on-board +12 V rail eliminates 64× hand-splicing
the red wire into the chassis harness. The 28BYJ-48 plugs in directly
with its standard 5-pin connector.

(Unused TPL7407L channels IN5-IN7 must be tied to GND, not floating.)

### THVD1410 (RS-485 transceiver, SOIC-8)

| Pin | Function | Net |
|---|---|---|
| 1 | R | UART_RX → MCU PA10 (USART1_RX) |
| 2 | /RE | **tied to GND** (always-receive; firmware discards TX echo) |
| 3 | DE | DE ← MCU PA12 (USART1_RTS_DE_CK) via 1 kΩ R_de; **R_de_pd 10 kΩ pull-down to GND on the DE pin** — STM32 GPIOs float as analog inputs during reset; a floating DE can jam the bus. Kept even if the THVD1410 has internal biasing (verify at capture) |
| 4 | D | UART_TX ← MCU PA9 (USART1_TX) |
| 5 | GND | GND |
| 6 | A | RS485_A → CE finger A |
| 7 | B | RS485_B → CE finger B |
| 8 | VCC | 3V3 + 100 nF decap |

No external ESD array on A/B: the THVD1410's bus pins are rated
±18 kV IEC 61000-4-2 contact — the former D5 SM712 is deleted (#179).

### Hall sensor connector (J3) — 3-pin JST-XH male, vertical THT

**Pinout LOCKED to KY-003 module native order** (so v1's existing
flying-lead cable plugs straight in without re-wiring):

| Pin | Function | Net |
|---|---|---|
| 1 | +3V3 (VCC) | 3V3 |
| 2 | GND | GND |
| 3 | HALL_OUT (SIG) | HALL_IN → MCU PB0 (via 10 kΩ R_hall pull-up to 3V3) |

If a different sensor module is used, the cable end adapts (re-crimp
the 3-pin housing or solder a pigtail) — the PCB pinout stays fixed.

**Hall sensor lives off-board** on a 3-conductor flying lead (matches
v1 mechanical alignment path). Choice of sensor module is build-time:

- **Spec a 3.3 V-capable open-drain hall** on the same KY-003-style
  3-wire lead: **TI DRV5023, Diodes AH3366Q, or Honeywell SS361RT**.
  Open-drain output works directly with the on-board R_hall pull-up.
- A genuine **KY-003** (what v1 uses) carries an A3144 with a
  **4.5 V minimum supply** and is **NOT qualified at 3.3 V**. v1
  KY-003 modules may work anecdotally at 3.3 V but must be
  individually bench-verified or replaced.
- Any other 3-pin hall module with the same VCC/GND/OUT pinout and a
  3.3 V-qualified open-drain output.

The unit PCB does not constrain magnet alignment — that is set by the
chassis bracket holding the hall module relative to the flap drum.
The PCB just provides the cable interface and signal conditioning
(R_hall pull-up, optional debounce capacitor on the OUT line).

### IDENTIFY button (SW1)

```
SW1 pin A ── IDENTIFY_BTN net ──┬── MCU PA8
                                  ├── R_id 10kΩ ── 3V3
                                  └── C_id 100nF ── GND
SW1 pin B ── GND
```

### LEDs (sink-driven — locked across pin table and this section)

```
3V3 ── R_led 1kΩ ── D1 anode ── D1 cathode ── MCU PB1  (LED_HEARTBEAT, MCU sinks low to illuminate)
3V3 ── R_led 1kΩ ── D2 anode ── D2 cathode ── MCU PB2  (LED_FAULT)
3V3 ── R_led 1kΩ ── D3 anode ── D3 cathode ── MCU PA15 (LED_IDENTIFY)
```

LEDs **sink-driven**: MCU GPIO connects to the LED **cathode** and
sinks current to GND when the GPIO is driven low. Anode is tied
through R_led 1 kΩ to 3V3. This matches the pin table above; earlier
drafts that said "GPIO → anode" were wrong.

### Card-edge fingers (CE, 2×5)

```
Position 1: 12V   ─── 12V net (→ F_unit → Q1 drain)     [both faces]
Position 2: GND   ─── GND net — LONGEST fingers (first-make) [both faces]
   — polarizing key slot —
Position 3: A     ─── RS485_A net (to THVD1410 pin 6)   [both faces]
Position 4: B     ─── RS485_B net (to THVD1410 pin 7)   [both faces]
Position 5: spare ─── test pad only                      [both faces]
```

Each net is on both card faces (dual-row socket reads top + bottom),
so every contact is doubled. The key slot between GND and A makes
reversed insertion mechanically impossible — the socket key is the
primary polarization; the asymmetric DIN clip is secondary. Edge
position/orientation on the board outline comes from the #100
insertion-kinematics mock-up.

### SWD test pads

| Pad | Net | Notes |
|---|---|---|
| TP_SWDIO | MCU PA13 | |
| TP_SWCLK | MCU PA14 | |
| TP_NRST | MCU NRST | 10kΩ pull-up + 100nF cap on this net |
| TP_GND | GND | |

### General-purpose test pads

| Pad | Net |
|---|---|
| TP1 | GND |
| TP2 | 3V3 |
| TP3 | UART_RX (PA10) |
| TP4 | UART_TX (PA9) |
| TP5 | NRST |

## Floorplan (rough placement) — chassis-compatible with v1

> **⚠ GEOMETRY SUPERSEDED — issue #100 (card-edge decided
> 2026-07-09)**: this floorplan was drawn for the withdrawn pogo
> system. The unit now mates via a 2×5 card-edge on one board edge;
> the pogo coordinates below are obsolete and the outline/edge
> position comes from the #100 insertion-kinematics mock-up — do NOT
> capture this section in KiCad yet. Netlist, part choices, and the
> protection chain are unaffected and capture-ready.

The v2 unit PCB outline is **80 × 40 mm**, identical to v1
(`PCB/v1/Gerber_PCB_Splitflap.zip`), with the same 4-corner mounting
hole pattern. v2 is a chassis drop-in replacement.

**Hard placement constraints (preserve v1 chassis cable runs):**
- **J2 stepper output**: top-left short edge, same XY as v1's
  "28BYJ-48 Stepper" header (now 5-pin to carry +12V — see J2 spec).
- **J3 hall sensor connector**: just below J2, same XY as v1's
  "Magnet Sensor" 3-pin header.
- **Mounting holes (LOCKED, extracted from v1 Gerber_Drill_NPTH.DRL)**:
  4× M3 clearance (3.2 mm) at:
    - (3.0, 3.0)  mm  ← top-left
    - (3.0, 77.0) mm  ← bottom-left
    - (37.0, 77.0) mm ← bottom-right
    - (37.0, 3.0) mm  ← top-right
  **Coordinate convention LOCKED from the v1 drill file: board 40 mm
  (X, short axis) × 80 mm (Y, long axis), origin bottom-left.**
  34 × 74 mm hole-to-hole bounding rectangle, centred on the board
  with 3 mm edge clearance. This pattern is what the 3D-printed DIN
  clip mounts to. Pre-layout check: overlay the v1 drill file on the
  v2 board in KiCad and confirm the 4 holes coincide.

**Layer assignments (preserve v1 heritage where compatible):**
- **Front (top)**: connectors (J2, J3), IDENTIFY button (SW1) on a
  visible edge, status LEDs (D1/D2/D3), SWD test pads.
- **Back (bottom)**: stepper driver U2 TPL7407L in **SOIC-16 narrow
  (150 mil)** placed near v1's ULN2003A position on the top-right
  back. **The footprint is NOT identical to v1**: v1's footprint
  (whatever its width) must be replaced with the narrow SOIC-16
  pattern in KiCad — verify against the TPL7407L datasheet
  (TI SLOSEH7) and the ULN2003AD (narrow) datasheet. LDO U3
  **LM2937IMP-3.3 (SOT-223, 26 V operating / 60 V transient)** placed
  near v1's AMS1117 position; SOT-223 with the **VOUT tab** (pin 2 +
  tab, internally tied) pulled into a copper pour for heatsinking
  (~1 cm² on bottom layer of the 3V3 net — NOT GND). LM2937 follows
  the LM1117/AMS1117 pin-order convention (pin 1=GND, pin 2=OUT=tab,
  pin 3=IN), so the AMS1117 footprint is reusable; verify against the
  LM2937 datasheet anyway as part of bring-up. MCU U1 STM32G030K
  LQFP-32, RS-485 transceiver U4 THVD1410, F_unit + Q1 AO3401 +
  Z1 10 V Zener clamp, ESD, decaps — placed wherever leaves a clear ~10 mm
  vertical channel through the long-axis centre for the pogo column.

```
   ─────────────────────── 80 mm ───────────────────────
  ┌──○─────────────────────────────────────────────○──┐
  │ J2 28BYJ-48                                       │
  │  ●●●                                              │
  │ J3 hall                            (FRONT side)   │   40 mm
  │  ●●●                                              │
  │                                                   │
  │   SW1 IDENT       (PG column on back layer        │
  │   D1 D2 D3        directly below — see below)     │
  │   HB FT ID                                        │
  │                                                   │
  │   TP_SWD pads                                     │
  ├──○─────────────────────────────────────────────○──┤

   ────────────── BACK side (component layer) ──────────
  ┌──○─────────────────────────────────────────────○──┐
  │                                                   │
  │           [U2 TPL7407L SOIC-16 narrow]            │   40 mm
  │           (placed near v1 ULN2003A spot;          │
  │            footprint = 150 mil narrow, not v1's)  │
  │                                                   │
  │   [U1 STM32G030] ●  PG1 12V    (y=+12)            │
  │   [U4 THVD1410] ●  PG2 A      (y= +4)            │
  │                  ●  PG3 B      (y= -4)            │
  │ [U3 LM2937-3.3   ●  PG4 GND    (y=-12)            │
  │  SOT-223 + pour] ↑                                │
  │   [Q1, Z1 10V Z, column at long-axis CENTRE       │
  │    D4 TVS, D5    (x = 40 mm)                      │
  │    ESD]                                           │
  │                  (x = 40 mm)                      │
  ├──○─────────────────────────────────────────────○──┤
   ───────────────────── 80 mm ─────────────────────────

  ○ = mounting hole, M3 clearance, position copied from v1
```

Hall sensor connector (J3) position: matches v1's "Magnet Sensor"
header. The 3-conductor flying lead exits the same edge it does on
v1, so existing chassis bracket + cable run is unchanged.

**DIN rail clip on the back** — **3D-printed asymmetric clip from
MakerWorld** (one printed per unit; user supplies STL link in build
notes). Bolts to the unit board through the 4× M3 holes at (3, 3),
(3, 77), (37, 77), (37, 3). The clip is the **only** polarization
mechanism — its asymmetric profile means the unit physically only
seats one way up on the rail. Reversed orientation does not engage
the rail (no PG_KEY pogo backup). Clip carries the unit's long axis
(80 mm) parallel to the rail. The bus PCB sits in the rail channel
directly above the clip; pogo pins on the back at long-axis centre
contact the bus PCB's contact stations.

## Pogo pin geometry

> **⚠ GEOMETRY UNRESOLVED — issue #100**: the pogo column position and
> orientation on the unit underside are pending the #100 mechanical
> mock-up, and the coordinates below use the superseded "long axis =
> X" convention (the locked convention is 40 mm = X/short axis, 80 mm
> = Y/long axis, origin bottom-left). Do NOT capture these positions
> in KiCad yet.

**4** through-hole pogo pins on the unit's **bottom layer**, dead-centre
on the long axis (`x = 40 mm` on an 80 × 40 mm board, or `x = 0` if
origin is the board centre). Single vertical line, no polarization
pogo:

| Pin | X (centred on long axis) | Y (along short axis) | Function |
|---|---|---|---|
| PG1 | 0 | +12 mm | 12V |
| PG2 | 0 |  +4 mm | RS485_A |
| PG3 | 0 |  -4 mm | RS485_B |
| PG4 | 0 | -12 mm | GND |

Spacing matches bus PCB trace centre-to-centre pitch (8 mm). Y-offsets
±12, ±4 mm exactly match the bus PCB trace centres (4 / 12 / 20 /
28 mm from top of the 32 mm board) so there is no mechanical Y
mismatch.

**Polarization is enforced mechanically by the 3D-printed DIN clip on
the back, NOT by a 5th pogo pin.** The clip is asymmetric — only one
mounting orientation engages the rail correctly. A reversed unit
cannot seat. See Mechanical section for the clip MPN/STL.

**Centred for DIN rail mounting**: the rail runs parallel to the
unit's long axis, with the rail clip on the back centreline. The bus
PCB rests in the rail channel directly above the clip; pogo pins
contact the bus PCB's nearest contact station as the unit is clipped
in. The 24 mm pogo column fits inside the 40 mm short axis with
8 mm clearance to each long edge.

## Fab parameters

| Parameter | Value |
|---|---|
| Outline | **80 × 40 mm** (matches v1 outline + 4-corner mounting hole pattern; chassis drop-in compatible) |
| Layer count | 2 |
| Substrate | FR-4 |
| Thickness | 1.6 mm |
| Copper weight | 1 oz |
| Surface finish | HASL (cheaper) — pogo pins on bus PCB side, not unit side, so unit doesn't need ENIG |
| Mounting holes | 4× M3 clearance at corners |

## KiCad 10 build steps

1. New KiCad project: `splitflap-unit-v2` (under `PCB/v2/kicad/unit/`).
2. Schematic editor: place all components per the LCSC table at the
   top of this doc. Add `LCSC` field per part (see
   `KICAD_HANDOFF.md` § 4 for bulk-edit via Symbol Fields Table).
3. Wire per the net tables. Use net labels for clarity:
   `12V`, `GND`, `3V3`, `RS485_A`, `RS485_B`, `STEPPER_IN1..4`,
   `HALL_IN`, `IDENTIFY_BTN`, `LED_HEARTBEAT`, `LED_FAULT`,
   `LED_IDENTIFY`, `NRST`, `UART_TX`, `UART_RX`, `DE`. (`/RE` is
   tied to GND on the THVD1410 — no net.)
4. Run ERC, fix all errors.
5. Update PCB from Schematic.
6. PCB editor (place per `LAYOUT_UNIT.md`):
   - Draw outline **40 × 80 mm** (locked convention: 40 mm = X/short
     axis, 80 mm = Y/long axis, origin bottom-left) on `Edge.Cuts`.
   - Place 4× `MountingHole_3.2mm_M3` at the locked v1 coordinates:
     (3, 3), (3, 77), (37, 77), (37, 3) mm — extracted from
     `PCB/v1/Gerber_PCB_Splitflap.zip` → `Gerber_Drill_NPTH.DRL`.
     Overlay the v1 drill file and confirm the 4 holes coincide.
   - Place U1 STM32G030K8T6 (official KiCad symbol
     `MCU_ST_STM32G0:STM32G030K8Tx`) centrally on the back side
     (near v1 ATmega328 footprint area).
   - Place stepper driver U2 (TPL7407L, **SOIC-16 narrow**) near the
     v1 ULN2003A position on the back. Use footprint
     `Package_SO:SOIC-16_3.9x9.9mm_P1.27mm` (NOT the 7.5 mm wide
     variant).
   - Place LDO U3 (LM2937IMP-3.3, footprint `SOT-223-3_TabPin2`) near
     v1's AMS1117 position; assign the **VOUT (pin 2 + tab) net**
     a generous filled zone for heatsinking. Tab is NOT GND —
     pouring it to GND shorts 3V3 to GND.
   - Place RS-485 path (U4 THVD1410) near the card-edge fingers.
   - Place J3 hall connector on a clean edge for cable exit toward
     the chassis hall bracket (matches v1 "Magnet Sensor" XY).
   - Place IDENTIFY button SW1 on the edge so it's accessible after
     install.
   - Card-edge fingers (2×5, see CE row in the component table) on
     one board edge — **do NOT capture the edge position/orientation
     yet; geometry comes from the #100 mock-up** (see warning above).
7. Route per `LAYOUT_UNIT.md`. Power traces 15–20 mil for 12V
   internal; signal 6 mil; A/B as a loose differential pair.
8. Add filled zones for GND (both layers) and VOUT-tab heatsink
   (bottom layer near U3). Press `B` to fill.
9. DRC: fix all errors.
10. Plot Gerbers + drill files (`KICAD_HANDOFF.md` § 10).
11. Order: 70 boards (covers 64 + spares). HASL plating is fine
    (pogos are on the bus PCB side, not unit side).
