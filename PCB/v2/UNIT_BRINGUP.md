# Unit PCB v2 — Bring-up procedure

> First-power sequence + per-test-point readings + standalone test mode. Created 2026-04-25.

## Pre-power inspection

1. Visual: verify the 2×3 backplane connector orientation matches the unit silkscreen (pin 1 marked).
2. Verify Q_REV (AO3401) source-on-input / drain-on-load orientation (corrected 2026-04-25 — pre-2026 had labels reversed).
3. Verify TPS54360DDA's HSOP-8 PowerPAD has the thermal-via cluster + ≥ 200 mm² bottom GND pour around it.
4. Verify TPL7407L is populated (NOT ULN2003 — the v2 spec is the MOSFET array).
5. Verify the AS5600's 10 × 10 mm magnet keep-out is genuinely free of copper / vias / ferrous components.
6. Verify SM712 ESD array on RS-485 lines (NOT SP0504BAATG — v2 spec).

## Standalone test (recommended) — bench-test a single unit without master/backplane

This mode exercises the unit using only a bench PSU and a USB-UART jig on the SWD pads.

### Setup
- Apply 48 V from a bench PSU directly to the unit's backplane connector pins 1, 3 (V48) and pins 5, 6 (GND). Use a 0.2 A current-limited supply to mimic the polyfuse.
- Plug a 28BYJ-48 motor (with magnet glued to the shaft end) into J_MOTOR.
- Connect a USB-UART (3.3 V) to the SWD pogo pads:
  - Pad 1 (PA13) → USB-UART RX
  - Pad 2 (PA14) → USB-UART TX
  - Pad 4 (GND) → USB-UART GND
  - Leave NRST floating (pull-up keeps it high).
- Open a serial terminal at 115200 baud.

### Expected
- 3.3 V rail comes up: STATUS LED enters 2 Hz blink (firmware running, awaiting address).
- Hold NRST low briefly to enter standalone test mode (firmware detects no SWD master and falls back to UART text protocol on the same pads).
- Terminal shows a banner: `UNIT v2 standalone mode. Type 'help' for commands.`

### Commands
- `id` → prints the STM32 96-bit UID (canonical address identity).
- `home` → spins motor until AS5600 detects the home reference angle; prints the angle reading.
- `step <N>` → advances motor by N steps (signed).
- `angle?` → prints the current AS5600 angle reading.
- `coil <N>` → energizes a single TPL7407L OUT_N for inspection.
- `reset` → soft-resets the unit.

### Pass criteria
- `id` returns a unique 96-bit UID.
- `home` completes within 3 s and reports a stable home angle.
- `step 100` rotates the motor smoothly without lost steps (verify by reading `angle?` before and after).
- `coil 1` through `coil 4` each energize one motor coil distinctly.
- I²C transactions to AS5600 succeed (no NACK errors).

## In-system test (with master + backplane)

1. Slot the unit into a backplane slot.
2. Power up the master and trigger UID-based discovery.
3. Verify the unit's status LED transitions: 2 Hz (awaiting address) → solid (enumerated/idle).
4. Master sends a `step` command; unit responds within ~100 ms.
5. Verify INA237 on master reports the unit's current draw is ~25 mA at idle, ~100 mA peak during stepping.

## Common bring-up faults

| Symptom | Likely cause |
|---|---|
| 3.3 V missing, 12 V present | LDO HT7833 fault — check SOT-89 thermal pad pour, check input/output 1 µF caps are NOT the same LCSC (pre-2026-04-25 bug) |
| 3.3 V missing, 12 V also missing | Buck not switching — verify C_BOOT_BUCK (100 nF / 25 V X7R) is populated CBOOT-to-SW within 2 mm; verify Q_REV is conducting (gate pulled to GND through 10 kΩ) |
| Unit draws current with reversed V48 polarity | Q_REV labels reversed — check AO3401 source vs drain on schematic against 2026-04-25 corrected topology |
| AS5600 NACKs on every I²C read | DIR pin floating, or RC filter wrong, or buck switch noise coupling — verify 100 Ω + 100 pF RC filter is between MCU and AS5600 SDA/SCL |
| AS5600 readings noisy (>10 LSB jitter) | Magnet too far from IC face (>3 mm air gap), or ferrous component in the keep-out zone |
| Stepper hums but doesn't advance | TPL7407L unused inputs IN5–IN7 floating — verify they're hard-tied to GND copper, NOT through 10 kΩ |
| Unit doesn't enumerate over RS-485 | Verify SN65HVD75 VCC = 3.3 V, A/B differential at idle should be ≈ 200–400 mV (master bias driving) |
| RS-485 traffic visible on scope but unit ignores it | UART baud wrong (must be 500 kbaud 8N1) or COBS framing decoder error |

## Stepping current measurement (mandatory at first prototype)

Per scottbez1 audit, the v1 estimate of 74 mA hold current may underestimate stepping peaks. With a current probe (or by measuring across a low-side shunt added temporarily in series with the V48 input), capture:
- Idle current (motor not stepping): expect ~25 mA.
- Stepping current peak: expect 200–400 mA (vs the 74 mA assumption).
- Stepping current average over a full rotation: expect 80–150 mA.

If stepping peak exceeds ~600 mA, the unit's buck inductor (Isat ≥ 1 A spec) and the backplane's 0.2 A polyfuse are inadequate — escalate to firmware for current-limited stepping or to a higher-rated inductor + 0.5 A polyfuse.
