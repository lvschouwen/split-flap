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

inline FollowerLinkOpPlan followerLinkPlanOp(uint32_t opcode, uint32_t address, long arg,
                                             const UnitFacts* units, int maxUnits) {
  FollowerLinkOpPlan plan;
  auto refuse = [&plan](wl_OpRefusal why) {
    plan.refusal = why;
    plan.kind = FollowerOpKind::None;
    return plan;
  };
  // A job on one running unit, with the check its argument takes.
  auto onUnit = [&](FollowerOpKind kind, MaintVerdict argVerdict) {
    const wl_OpRefusal unit = followerLinkCheckUnit(address, units, maxUnits);
    if (unit != wl_OpRefusal_REFUSAL_NONE) return refuse(unit);
    if (argVerdict.httpStatus != 200) return refuse(wl_OpRefusal_REFUSAL_BAD_ARG);
    plan.kind = kind;
    plan.addr = (uint8_t)address;
    plan.arg = arg;
    return plan;
  };
  const MaintVerdict noArg;
  switch (opcode) {
    case wl_OpCode_OPC_HOME:           return onUnit(FollowerOpKind::Home, noArg);
    case wl_OpCode_OPC_IDENTIFY:       return onUnit(FollowerOpKind::Identify, noArg);
    case wl_OpCode_OPC_JOG:            return onUnit(FollowerOpKind::Jog, maintValidateJog(arg));
    case wl_OpCode_OPC_SET_OFFSET:
      return onUnit(FollowerOpKind::WriteOffset, maintValidateOffset(arg));
    case wl_OpCode_OPC_SELF_TEST:      return onUnit(FollowerOpKind::SelfTest, noArg);
    case wl_OpCode_OPC_RESET_ODOMETER: return onUnit(FollowerOpKind::ResetOdometer, noArg);
    case wl_OpCode_OPC_SET_GATES:
      return onUnit(FollowerOpKind::SetGates, maintValidateGates(arg));
    case wl_OpCode_OPC_BOOT_INFO:      return onUnit(FollowerOpKind::BootInfo, noArg);
    case wl_OpCode_OPC_BOOT_DUMP:      return onUnit(FollowerOpKind::BootDump, noArg);
    case wl_OpCode_OPC_BOOT_UPDATE:    return onUnit(FollowerOpKind::BootUpdate, noArg);
    case wl_OpCode_OPC_RESTART_UNIT:
      // Range only, no running-unit check: this is the way back for a unit
      // whose protocol this build does not speak.
      if (address < 1 || address > 126) return refuse(wl_OpRefusal_REFUSAL_BAD_ADDRESS);
      plan.kind = FollowerOpKind::RebootToBootloader;
      plan.addr = (uint8_t)address;
      return plan;
    case wl_OpCode_OPC_UPDATE_UNITS:
      if (arg != 0 && arg != 1) return refuse(wl_OpRefusal_REFUSAL_BAD_ARG);
      if (address == 0) {
        // Never a whole-row erase of units that are current.
        if (arg != 0) return refuse(wl_OpRefusal_REFUSAL_BAD_ARG);
      } else if (!reflashAddressInRange((long)address, SFP_I2C_ADDRESS_BASE, maxUnits)) {
        return refuse(wl_OpRefusal_REFUSAL_BAD_ADDRESS);
      }
      plan.kind = FollowerOpKind::ReflashUnit;
      plan.addr = (uint8_t)address;
      plan.arg = arg;
      return plan;
    case wl_OpCode_OPC_PROBE:
      plan.kind = FollowerOpKind::Probe;
      return plan;
    case wl_OpCode_OPC_SET_ADDRESS: {
      const wl_OpRefusal unit = followerLinkCheckUnit(address, units, maxUnits);
      if (unit != wl_OpRefusal_REFUSAL_NONE) return refuse(unit);
      switch (maintValidateSetAddressTarget(arg, (int)address, units, maxUnits).httpStatus) {
        case 200: break;
        case 409: return refuse(wl_OpRefusal_REFUSAL_ADDRESS_TAKEN);
        default:  return refuse(wl_OpRefusal_REFUSAL_BAD_ARG);
      }
      plan.kind = FollowerOpKind::SetAddress;
      plan.addr = (uint8_t)address;
      plan.arg = arg;
      return plan;
    }
    case wl_OpCode_OPC_CLEAR_ADDRESS:  return onUnit(FollowerOpKind::ClearAddress, noArg);
    case wl_OpCode_OPC_HOME_ALL:
      plan.kind = FollowerOpKind::HomeAll;
      return plan;
    default:
      return refuse(wl_OpRefusal_REFUSAL_UNKNOWN_OP);
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
