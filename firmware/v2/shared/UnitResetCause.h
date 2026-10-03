#pragma once
// Why a unit last reset (#502). One definition for both sides: the unit
// classifies its own reset to decide which lifetime counter moves, and the row
// masters decode GET_STATUS byte 1 with the same rules.
//
// The unit's bootloader clears MCUSR before the sketch runs and leaves the
// cause in GPIOR0; the sketch reads both. A reset the sketch asked for itself
// (SFP_CMD_REBOOT, SFP_CMD_ENTER_BOOTLOADER, an address burn, boot-update
// stage 1) is a watchdog reset on the chip, so the sketch leaves a marker in
// EEPROM first — without it every reflash would read as a hang.

#include <stdint.h>

// ATmega328P MCUSR bit values (datasheet 10.9.1).
#define UNIT_MCUSR_PORF   0x01
#define UNIT_MCUSR_EXTRF  0x02
#define UNIT_MCUSR_BORF   0x04
#define UNIT_MCUSR_WDRF   0x08
#define UNIT_MCUSR_MASK   0x0F

// GET_STATUS byte 1, bit 7: the watchdog reset in the low bits was requested
// by the sketch. MCUSR itself never sets this bit.
#define UNIT_RESET_REQUESTED_FLAG  0x80

enum UnitResetKind : uint8_t {
  UNIT_RESET_UNKNOWN = 0,  // no cause recorded
  UNIT_RESET_POWER_ON,
  UNIT_RESET_BROWNOUT,
  UNIT_RESET_WATCHDOG,     // the sketch hung
  UNIT_RESET_REQUESTED,    // the sketch reset itself on purpose
  UNIT_RESET_EXTERNAL,     // reset pin
};

// Power-on wins: a rail that rises slowly sets BORF alongside PORF, and that
// is not a brownout — which also means a sag deep enough to trip the power-on
// reset counts as a power cycle; the chip cannot tell the two apart. A
// brownout wins over a requested reset because the rail is the more urgent
// finding.
inline UnitResetKind unitResetClassify(uint8_t mcusr, bool requested) {
  if (mcusr & UNIT_MCUSR_PORF) return UNIT_RESET_POWER_ON;
  if (mcusr & UNIT_MCUSR_BORF) return UNIT_RESET_BROWNOUT;
  if (mcusr & UNIT_MCUSR_WDRF) {
    return requested ? UNIT_RESET_REQUESTED : UNIT_RESET_WATCHDOG;
  }
  if (mcusr & UNIT_MCUSR_EXTRF) return UNIT_RESET_EXTERNAL;
  return UNIT_RESET_UNKNOWN;
}

// What the unit puts in GET_STATUS byte 1.
inline uint8_t unitResetStatusByte(uint8_t mcusr, UnitResetKind kind) {
  uint8_t b = (uint8_t)(mcusr & UNIT_MCUSR_MASK);
  if (kind == UNIT_RESET_REQUESTED) b |= UNIT_RESET_REQUESTED_FLAG;
  return b;
}

// What a master makes of GET_STATUS byte 1.
inline UnitResetKind unitResetFromStatusByte(uint8_t statusByte) {
  return unitResetClassify(statusByte,
                           (statusByte & UNIT_RESET_REQUESTED_FLAG) != 0);
}

inline const char* unitResetKindName(UnitResetKind k) {
  switch (k) {
    case UNIT_RESET_POWER_ON:  return "power-on";
    case UNIT_RESET_BROWNOUT:  return "brownout";
    case UNIT_RESET_WATCHDOG:  return "watchdog";
    case UNIT_RESET_REQUESTED: return "requested";
    case UNIT_RESET_EXTERNAL:  return "external";
    default:                   return "unknown";
  }
}
