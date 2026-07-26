#pragma once
// MaintenancePolicy.h — pure validation, wire encoding and postcondition
// classification for the calibration/provisioning ops (#204), natively
// tested by test_maintenance_policy.
//
// The validators are v1's parseCalibrationAddress/endpoint checks as a
// seam. The occupancy check deliberately runs TWICE per set-address: at the
// web boundary against the handler's snapshot copy (fast 409 for the user)
// and in displayTask against its live facts right before the EEPROM burn
// (authoritative — the queue delay makes the web-side view stale by
// design). Postcondition classifiers grade the compound address ops after
// their reprobe: "ok" means the expected bus state was OBSERVED, not merely
// that the EEPROM write ACKed.

#ifdef UNIT_TEST
  #include <cstdint>
  #include <cstdlib>
#else
  #include <Arduino.h>
#endif

#include "SplitFlapProtocol.h"
#include "UnitHealth.h"

// The validators below spell the lower I2C bound as literal 1; they only
// stay correct while the wire contract's base agrees.
static_assert(SFP_I2C_ADDRESS_BASE == 1,
              "maintenance validators assume address base 1");

// Verdict for a web-boundary check: httpStatus 200 = pass, else the HTTP
// error to send with `message` as the body (v1's response texts).
struct MaintVerdict {
  int httpStatus = 200;
  const char* message = "";
};

// v1 parseCalibrationAddress: raw query param (nullptr = missing) → 1..126
// I2C range → within the managed window → sketch-running unit. On pass,
// `outAddr` holds the parsed address. strtol base 0 keeps v1's hex support.
inline MaintVerdict maintValidateAddress(const char* raw,
                                         const UnitFacts* units, int maxUnits,
                                         int& outAddr) {
  if (raw == nullptr) {
    return {400, "Missing 'address' query param"};
  }
  char* end = nullptr;
  long parsed = strtol(raw, &end, 0);
  if (end == raw) {
    return {400, "Address must be a number"};
  }
  if (parsed < 1 || parsed > 126) {
    return {400, "Address must be 1..126"};
  }
  int unitIndex = (int)parsed - SFP_I2C_ADDRESS_BASE;
  if (unitIndex < 0 || unitIndex >= maxUnits || units[unitIndex].state != 1) {
    return {404, "No sketch-running unit at that address"};
  }
  // #405: a unit reporting a wire contract we do not speak is present but not
  // drivable, so every single-unit op behind this gate is refused. Without it
  // an operator could still fire the most consequential one of all —
  // SET_I2C_ADDRESS, an unverifiable address burn whose recovery is a physical
  // trip — at a unit whose reply layout we cannot even parse.
  //
  // POST /unit/reboot deliberately does NOT come through here (it range-checks
  // itself), which is what keeps ENTER_BOOTLOADER reachable and a mismatched
  // unit always recoverable by reflash.
  if (!unitDrivable(units[unitIndex])) {
    return {409, "Unit reports an unrecognised protocol version — reflash it"};
  }
  outAddr = (int)parsed;
  return {};
}

// ±one drum revolution (v1 #171): the unit clamps the same bound in its
// SET_OFFSET path — past it, its post-homing rotation outruns the 8 s
// watchdog. Rejecting here gives the calibration UI a real error instead of
// a silently-dropped write.
inline MaintVerdict maintValidateOffset(long value) {
  if (value < -SFP_OFFSET_LIMIT_STEPS || value > SFP_OFFSET_LIMIT_STEPS) {
    return {400, "Offset must be within +/-2038 steps (one revolution)"};
  }
  return {};
}

// Jog rides the wire as one signed byte.
inline MaintVerdict maintValidateJog(long steps) {
  if (steps < -127 || steps > 127) {
    return {400, "Steps must be -127..127"};
  }
  return {};
}

// Feature gates ride the wire as one byte (#409). WHICH bits are legal is
// deliberately not checked here: the vocabulary belongs to the unit's
// firmware, which refuses any bit it has no code for, and the write's
// read-back grades that refusal as a postcondition failure. A master that
// second-guessed the vocabulary would start rejecting gates that a newer unit
// firmware understands perfectly well.
inline MaintVerdict maintValidateGates(long gates) {
  if (gates < 0 || gates > 255) {
    return {400, "Gates must be a 0..255 bit mask"};
  }
  return {};
}

// Set-address target: bounded to the managed window (an address above it
// would strand the unit beyond all over-I2C management — v1 rule), and the
// target must not answer on the bus unless it IS the unit's current address
// (the bulk-migration case is always allowed).
inline MaintVerdict maintValidateSetAddressTarget(long target, int currentAddr,
                                                  const UnitFacts* units,
                                                  int maxUnits) {
  if (target < 1 || target > maxUnits) {
    return {400, "Address must be within the managed unit range"};
  }
  if (target != currentAddr &&
      units[(int)target - SFP_I2C_ADDRESS_BASE].state != 0) {
    return {409, "Target address is already occupied on the bus"};
  }
  return {};
}

// --- wire-byte encoders ---------------------------------------------------------
// Kept pure so the negative encodings UnitBus puts on the bus are asserted
// natively (v1 encoded these inline in ServiceFlapFunctions.ino).

inline void maintEncodeOffsetLE(int16_t value, uint8_t out[2]) {
  out[0] = (uint8_t)((uint16_t)value & 0xFF);
  out[1] = (uint8_t)(((uint16_t)value >> 8) & 0xFF);
}

inline uint8_t maintEncodeJogByte(int steps) {
  if (steps > 127) steps = 127;
  if (steps < -127) steps = -127;
  return (uint8_t)(int8_t)steps;
}

// --- execution outcomes (the /unit/op-result vocabulary) ------------------------

enum class MaintOutcome : uint8_t {
  Pending = 0,         // slot default; a real result always overwrites it
  Ok,                  // wire ACK + (for compound ops) postcondition observed
  WireFail,            // Wire transaction failed / unit did not ACK
  ExecValidationFail,  // displayTask recheck refused the op (stale web view)
  PostconditionFail,   // burn ACKed but the reprobe contradicts the intent
};

enum class MaintReason : uint8_t {
  None = 0,
  UnitMissingAfterReprobe,  // expected responder absent from the rescan
  TargetAddressOccupied,    // pre-burn exec recheck found the target taken
};

// After SetAddress burn + settle + reprobe: the unit must answer in sketch
// mode at its new address.
inline MaintOutcome classifySetAddressOutcome(const UnitFacts* facts,
                                              int maxUnits, int target,
                                              MaintReason& reason) {
  int idx = target - SFP_I2C_ADDRESS_BASE;
  if (idx >= 0 && idx < maxUnits && facts[idx].state == 1) {
    reason = MaintReason::None;
    return MaintOutcome::Ok;
  }
  reason = MaintReason::UnitMissingAfterReprobe;
  return MaintOutcome::PostconditionFail;
}

// After ClearAddress + settle + reprobe the unit rejoins at its DIP-derived
// address, which the master cannot know — the observable postcondition is
// that no unit vanished. A drop means DIP collision or DIP beyond the
// managed window (recoverable by DIP switches + power cycle; the UI warns).
inline MaintOutcome classifyClearAddressOutcome(int countBefore,
                                                int countAfter,
                                                MaintReason& reason) {
  if (countAfter >= countBefore) {
    reason = MaintReason::None;
    return MaintOutcome::Ok;
  }
  reason = MaintReason::UnitMissingAfterReprobe;
  return MaintOutcome::PostconditionFail;
}
