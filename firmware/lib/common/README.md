# lib/common — shared firmware helpers

Pure-logic helpers consumed by both `MasterS3/` and `MasterH2/`, plus host-side native tests under `test/`.

Landed (Phase 3a, #66):

- `cobs_crc.{h,cpp}` — COBS framing + CRC-16/CCITT-FALSE. Wire format is `COBS( payload || CRC16-BE(payload) ) 0x00`. `splitflap::frame::encode` / `decode` stack the two into a verified codec.
- `protocol.{h,cpp}` — S3↔H2 message types (PING / PONG / LOG) and codec. 4-byte header `[version][type][seq_hi][seq_lo]`, with `seq` big-endian.
- `config.h` — row/unit topology constants (`kMaxRows` = 8, `kMaxUnitsPerRow` = 16, Row 1..Row 8 labeling helpers).

Planned in later phases:

- `settings_layout.{h,cpp}` — Preferences key names (clean break from ESP8266 flat-byte EEPROM).
- `web_log.{h,cpp}` — Print-subclass log ring (port from ESPMaster).

## Running the tests

```bash
cd firmware/lib/common
pio test -e native
```

Unity is the test framework. Tests live in `test/test_cobs_crc/` and `test/test_protocol/`. The native env compiles the two `.cpp` files under this directory plus each test binary. No Arduino dependency — pure C++17.

See meta issue #58 for the phase plan.
