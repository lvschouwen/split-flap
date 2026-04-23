# lib/common — shared firmware helpers

Pure-logic helpers consumed by both `MasterS3/` and `MasterH2/`, plus host-side native tests under `test/`.

## Layout

```
firmware/v1/lib/common/
├── library.json          — PIO library manifest (consumed via lib_extra_dirs)
├── platformio.ini        — host-side native test env
├── src/
│   ├── config.h          — row/unit topology constants (kMaxRows, kMaxUnits, ...)
│   ├── cobs_crc.{h,cpp}  — CRC-16/CCITT-FALSE + COBS + frame codec
│   ├── frame_reader.{h,cpp} — stateful byte→frame reassembler for UART RX
│   └── protocol.{h,cpp}  — PING / PONG / LOG message codec
└── test/
    ├── test_cobs_crc/    — CRC vector, COBS round-trip, frame encode/decode
    ├── test_frame_reader/— Reader state machine + recovery
    ├── test_link_loopback/— end-to-end S3↔H2 codec loopback
    └── test_protocol/    — proto encode/parse + error paths
```

## Wire format

```
  [ COBS( payload || CRC16-BE(payload) ) ] 0x00
```

CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, no xorout). The trailing `0x00` is the only zero byte on the wire, so a receiver can always re-sync by scanning to the next delimiter.

Message header (4 bytes):

```
  [ version:u8 ][ type:u8 ][ seq_hi:u8 ][ seq_lo:u8 ]
```

Types: `PING` (0x01), `PONG` (0x02), `LOG` (0x03). Ping/Pong carry no body; Log carries raw text (no null terminator).

## Consumption from firmwares

Both `MasterS3/platformio.ini` and `MasterH2/platformio.ini` declare:

```ini
lib_extra_dirs = ${PROJECT_DIR}/../lib
lib_deps = common
```

PIO then auto-compiles `src/*.cpp` and exposes headers via `#include "cobs_crc.h"` etc.

## Running the tests

```bash
cd firmware/v1/lib/common
pio test -e native
```

Unity is the test framework. Pure C++17, no Arduino dependency. CI runs this suite alongside the ESPMaster one.

## Planned (later phases)

- `settings_layout.{h,cpp}` — Preferences key names (clean break from ESP8266 flat-byte EEPROM).
- `web_log.{h,cpp}` — Print-subclass log ring (port from ESPMaster).

See meta issue #58 for the phase plan.
