# Master GPIO allocation policy

Governs how GPIO ownership is assigned on the master ESP32-S3. Applies to the v2 PCB only; the v1 ESP-01 master is out of scope.

## Status

Policy rules are **frozen**. Specific GPIO numbers are **not yet assigned** — resolution gated on [`OPEN_QUESTIONS.md`](./OPEN_QUESTIONS.md) §Q1–Q3. Once chosen, [`PINOUT.md`](./PINOUT.md) becomes the single source of truth and this policy governs every subsequent change.

## Rules

1. **Single owner per GPIO.** Every GPIO has exactly one owner function unless explicitly marked as a shared bus (I²C, SPI, etc.).
2. **UART0 is reserved** — boot, field recovery, and debug only. The TX/RX pins assigned to UART0 do not carry application traffic and are not remapped after boot.
3. **GPIO48 is avoided** for any critical signal (alerts, bus interfaces, strap-adjacent lines). Acceptable uses: non-critical status LED, test pad, or unassigned.
4. **Alerts get dedicated GPIOs.** Each INA237 `ALERT` pin has its own GPIO. Alert inputs must be non-strap and non-boot-sensitive.
5. **Spare IRQ pool.** At least two GPIOs remain unallocated as spare IRQ / fault inputs, with 10 kΩ pull-ups and test pads placed so they accept a future sensor or protection signal without a respin.
6. **Strap pins are no-route.** IO3, IO45, IO46 get no copper escape beyond the module pad.
7. **PINOUT.md is canonical.** Any schematic change that assigns a GPIO updates `PINOUT.md` in the same commit, or the change is rejected.

## Allocation order

Assign in this strict order. Each step completes before the next begins, so later steps never displace earlier ones.

1. **Boot / debug / recovery** — reset button, UART0 TX/RX, native USB D±, strap pins.
2. **Bus interfaces** — I²C, RS-485 UARTs, S3↔H2 coprocessor link, other multi-slave buses.
3. **Alert / interrupt inputs** — INA237 alerts first, then the reserved spare-IRQ pool (≥ 2).
4. **Shared control buses** — sideband control lines that coordinate bus peripherals (H2 reset/boot/IRQ, future mux control, etc.).
5. **Optional LEDs / status / expansion** — TLC5947 control, RGB data, heartbeat LEDs, expansion headers.

## Enforcement

- A schematic change that assigns a GPIO either updates `PINOUT.md` in the same commit or is rejected in review.
- Adding a new alert/interrupt signal consumes a spare-IRQ slot. If fewer than two spares remain after the change, the PR blocks until a new spare is carved out.
- Any assignment to IO48 requires an explicit waiver comment referencing this policy.
