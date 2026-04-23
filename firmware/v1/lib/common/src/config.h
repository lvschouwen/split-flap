// Shared firmware constants for the Master v2 platform.
//
// Consumed by firmware/MasterS3/, firmware/MasterH2/, and host-side native
// tests under firmware/lib/common/test/. Pure C++ — no Arduino dependencies,
// no <Arduino.h>. Safe to include from .ino, .cpp, and host test files.
//
// Defines the canonical row/unit topology of the Master v2 hardware (8 rows
// × 16 units = 128 unit ceiling) and helpers that translate between the
// human-facing 1..8 row label (silkscreen, web UI, logs, errors) and the
// internal 0..7 channel index used by the TCA9548A I2C mux.

#pragma once

#include <cstdint>

namespace splitflap {

// Maximum number of rows supported by the Master v2 hardware.
// Equal to the number of TCA9548A downstream channels (8).
inline constexpr uint8_t kMaxRows = 8;

// Maximum number of unit Nanos addressable on a single row.
// Limited by the 4-bit DIP switch on the unit boards (16 addresses).
inline constexpr uint8_t kMaxUnitsPerRow = 16;

// Total unit ceiling = kMaxRows * kMaxUnitsPerRow.
inline constexpr uint16_t kMaxUnits = kMaxRows * kMaxUnitsPerRow;  // 128

// Row labels are one-indexed: Row 1 .. Row 8.
inline constexpr uint8_t kFirstRow = 1;
inline constexpr uint8_t kLastRow  = kMaxRows;  // 8

// Returns true if `row` is a valid row label (1..kMaxRows).
constexpr bool rowLabelOk(uint8_t row) {
  return row >= kFirstRow && row <= kLastRow;
}

// Returns the 0..7 channel index for a 1..8 row label.
// Caller must ensure rowLabelOk(row) first.
constexpr uint8_t rowToChannelIndex(uint8_t row) {
  return static_cast<uint8_t>(row - 1);
}

// Returns the TCA9548A channel-select register byte for a 1..8 row label.
// Single-bit value: Row 1 → 0x01, Row 2 → 0x02, ..., Row 8 → 0x80.
// Multi-bit values are intentionally not produced — parallel-row pullup load
// would exceed the unit Nano's I_OL spec. See SCHEMATIC.md §9 for context.
// Caller must ensure rowLabelOk(row) first.
constexpr uint8_t rowToChannelByte(uint8_t row) {
  return static_cast<uint8_t>(1u << rowToChannelIndex(row));
}

}  // namespace splitflap
