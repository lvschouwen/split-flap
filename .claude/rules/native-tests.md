---
paths:
  - "firmware/v2/*/test/**"
---
# Native tests (`pio test -e native`)

- ArduinoFake: `map()` is a fakeit mock — wire the real formula in each test's `setUp()` or calls abort; `EEPROM` etc. re-wire via `ArduinoFake(EEPROM)`.
- Host tests cover pure logic only; hardware glue is bench-verified.
