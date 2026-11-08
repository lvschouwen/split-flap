#pragma once
// FollowerLinkOps.h — what a unit job from the wall link means on this row
// (#564), pure and natively tested by test_follower_link_ops. Every check is
// the shared one the master's own row takes (MaintenancePolicy.h,
// ReflashPlan.h); this
// only says which checks a job code takes and names the refusal.

#include <stdio.h>

#include "FollowerOps.h"
#include "wall_link.pb.h"

struct FollowerLinkOpPlan {
  wl_OpRefusal refusal = wl_OpRefusal_REFUSAL_NONE;
  FollowerOpKind kind = FollowerOpKind::None;
  uint8_t addr = 0;
  long arg = 0;
};

// The shared address check, for an address that arrives as a number.
inline wl_OpRefusal followerLinkCheckUnit(uint32_t address, const UnitFacts* units, int maxUnits) {
  char raw[12];
  snprintf(raw, sizeof(raw), "%lu", (unsigned long)address);
  int parsed = 0;
  switch (maintValidateAddress(raw, units, maxUnits, parsed).httpStatus) {
    case 200: return wl_OpRefusal_REFUSAL_NONE;
    case 404: return wl_OpRefusal_REFUSAL_NO_UNIT;
    case 409: return wl_OpRefusal_REFUSAL_UNIT_PROTOCOL;
    default:  return wl_OpRefusal_REFUSAL_BAD_ADDRESS;
  }
}

inline FollowerLinkOpPlan followerLinkRefused(wl_OpRefusal why) {
  FollowerLinkOpPlan plan;
  plan.refusal = why;
  return plan;
}

inline FollowerLinkOpPlan followerLinkPlanned(FollowerOpKind kind, uint32_t address, long arg) {
  FollowerLinkOpPlan plan;
  plan.kind = kind;
  plan.addr = (uint8_t)address;
  plan.arg = arg;
  return plan;
}

// The jobs on one running unit, with the check each argument takes. False
// for a code that is not one of them.
inline bool followerLinkUnitJob(uint32_t opcode, long arg, FollowerOpKind& kind,
                                MaintVerdict& argVerdict) {
  argVerdict = MaintVerdict();
  switch (opcode) {
    case wl_OpCode_OPC_HOME:           kind = FollowerOpKind::Home; return true;
    case wl_OpCode_OPC_IDENTIFY:       kind = FollowerOpKind::Identify; return true;
    case wl_OpCode_OPC_JOG:
      kind = FollowerOpKind::Jog;
      argVerdict = maintValidateJog(arg);
      return true;
    case wl_OpCode_OPC_SET_OFFSET:
      kind = FollowerOpKind::WriteOffset;
      argVerdict = maintValidateOffset(arg);
      return true;
    case wl_OpCode_OPC_SELF_TEST:      kind = FollowerOpKind::SelfTest; return true;
    case wl_OpCode_OPC_RESET_ODOMETER: kind = FollowerOpKind::ResetOdometer; return true;
    case wl_OpCode_OPC_SET_GATES:
      kind = FollowerOpKind::SetGates;
      argVerdict = maintValidateGates(arg);
      return true;
    case wl_OpCode_OPC_BOOT_INFO:      kind = FollowerOpKind::BootInfo; return true;
    case wl_OpCode_OPC_BOOT_DUMP:      kind = FollowerOpKind::BootDump; return true;
    case wl_OpCode_OPC_BOOT_UPDATE:    kind = FollowerOpKind::BootUpdate; return true;
    case wl_OpCode_OPC_CLEAR_ADDRESS:  kind = FollowerOpKind::ClearAddress; return true;
    default:                           return false;
  }
}

inline FollowerLinkOpPlan followerLinkPlanUnitJob(FollowerOpKind kind, MaintVerdict argVerdict,
                                                  uint32_t address, long arg,
                                                  const UnitFacts* units, int maxUnits) {
  const wl_OpRefusal unit = followerLinkCheckUnit(address, units, maxUnits);
  if (unit != wl_OpRefusal_REFUSAL_NONE) return followerLinkRefused(unit);
  if (argVerdict.httpStatus != 200) return followerLinkRefused(wl_OpRefusal_REFUSAL_BAD_ARG);
  return followerLinkPlanned(kind, address, arg);
}

// Range only, no running-unit check: this is the way back for a unit whose
// protocol this build does not speak.
inline FollowerLinkOpPlan followerLinkPlanRestartUnit(uint32_t address) {
  if (address < 1 || address > 126) return followerLinkRefused(wl_OpRefusal_REFUSAL_BAD_ADDRESS);
  return followerLinkPlanned(FollowerOpKind::RebootToBootloader, address, 0);
}

inline FollowerLinkOpPlan followerLinkPlanUpdateUnits(uint32_t address, long arg, int maxUnits) {
  if (arg != 0 && arg != 1) return followerLinkRefused(wl_OpRefusal_REFUSAL_BAD_ARG);
  if (address == 0) {
    // Never a whole-row erase of units that are current.
    if (arg != 0) return followerLinkRefused(wl_OpRefusal_REFUSAL_BAD_ARG);
  } else if (!reflashAddressInRange((long)address, SFP_I2C_ADDRESS_BASE, maxUnits)) {
    return followerLinkRefused(wl_OpRefusal_REFUSAL_BAD_ADDRESS);
  }
  return followerLinkPlanned(FollowerOpKind::ReflashUnit, address, arg);
}

inline FollowerLinkOpPlan followerLinkPlanSetAddress(uint32_t address, long arg,
                                                     const UnitFacts* units, int maxUnits) {
  const wl_OpRefusal unit = followerLinkCheckUnit(address, units, maxUnits);
  if (unit != wl_OpRefusal_REFUSAL_NONE) return followerLinkRefused(unit);
  switch (maintValidateSetAddressTarget(arg, (int)address, units, maxUnits).httpStatus) {
    case 200: return followerLinkPlanned(FollowerOpKind::SetAddress, address, arg);
    case 409: return followerLinkRefused(wl_OpRefusal_REFUSAL_ADDRESS_TAKEN);
    default:  return followerLinkRefused(wl_OpRefusal_REFUSAL_BAD_ARG);
  }
}

inline FollowerLinkOpPlan followerLinkPlanOp(uint32_t opcode, uint32_t address, long arg,
                                             const UnitFacts* units, int maxUnits) {
  FollowerOpKind kind = FollowerOpKind::None;
  MaintVerdict argVerdict;
  if (followerLinkUnitJob(opcode, arg, kind, argVerdict)) {
    return followerLinkPlanUnitJob(kind, argVerdict, address, arg, units, maxUnits);
  }
  switch (opcode) {
    case wl_OpCode_OPC_RESTART_UNIT: return followerLinkPlanRestartUnit(address);
    case wl_OpCode_OPC_UPDATE_UNITS: return followerLinkPlanUpdateUnits(address, arg, maxUnits);
    case wl_OpCode_OPC_PROBE:        return followerLinkPlanned(FollowerOpKind::Probe, 0, 0);
    case wl_OpCode_OPC_SET_ADDRESS:  return followerLinkPlanSetAddress(address, arg, units, maxUnits);
    case wl_OpCode_OPC_HOME_ALL:     return followerLinkPlanned(FollowerOpKind::HomeAll, 0, 0);
    default:                         return followerLinkRefused(wl_OpRefusal_REFUSAL_UNKNOWN_OP);
  }
}

inline wl_OpPhase followerLinkPhaseFor(MaintOutcome outcome) {
  if (outcome == MaintOutcome::Pending) return wl_OpPhase_OP_RUNNING;
  return outcome == MaintOutcome::Ok ? wl_OpPhase_OP_OK : wl_OpPhase_OP_FAILED;
}

// How much of a result of `total` bytes the piece at `offset` carries.
inline uint32_t followerLinkPieceLen(uint32_t total, uint32_t offset, uint32_t pieceMax) {
  if (offset >= total) return 0;
  const uint32_t left = total - offset;
  return left < pieceMax ? left : pieceMax;
}
