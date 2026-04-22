# lib/common — shared firmware helpers

Pure-logic helpers consumed by both `MasterS3/` and `MasterH2/`, plus host-side native tests.

Empty in Phase 2 (#63) — code lands here from Phase 3 onward:

- `cobs_crc.{h,cpp}` — COBS framing + CRC-16/CCITT-FALSE for the S3↔H2 UART link
- `protocol.{h,cpp}` — S3↔H2 message types and codec
- `settings_layout.{h,cpp}` — Preferences keys (clean break from ESP8266 flat-byte EEPROM)
- `web_log.{h,cpp}` — Print-subclass log ring (port from ESPMaster)

Each module gets a host-buildable test under `lib/common/test/` so we get coverage without flashing hardware. See meta issue #58.
