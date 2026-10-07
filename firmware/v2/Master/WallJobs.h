#pragma once
// WallJobs.h — the unit jobs an operator starts through POST /api/v2/action
// (#559/#566): their names, what each needs, and how a row's answer reads.
// Pure, natively tested by test_wall_jobs. The same job runs on the master's
// own units (a DisplayCommand) and on a row board's (an Op on the wall link);
// nothing here knows which.
//
//   {"name":"home","target":{"row":"<row id>","unit":3}}
//   {"name":"jog","target":{"row":"","unit":3},"args":{"steps":-4}}
//       target.row: "" or absent = the master's own row
// Value checks are the shared ones (MaintenancePolicy.h). Whether there is a
// running unit at the address is judged by the board that holds the units.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "MaintenancePolicy.h"
#include "wall_link.pb.h"

enum class WallJobUnit : uint8_t {
  None,      // a job on the whole row
  One,       // target.unit is required
  OneOrAll,  // without target.unit: every unit that needs it
};

// What a finished job hands back besides its outcome.
enum class WallJobData : uint8_t { None, Json, Bytes };

struct WallJobKind {
  const char* name;
  wl_OpCode opcode;
  WallJobUnit unit;
  const char* arg;  // the key under "args" this job reads; nullptr = none
  bool argRequired;
  WallJobData data;
};

// Every OpCode of the link once (gated by tests/test_wall_jobs_glue.py).
static const WallJobKind WALL_JOB_KINDS[] = {
    {"home", wl_OpCode_OPC_HOME, WallJobUnit::One, nullptr, false, WallJobData::None},
    {"identify", wl_OpCode_OPC_IDENTIFY, WallJobUnit::One, nullptr, false, WallJobData::None},
    {"jog", wl_OpCode_OPC_JOG, WallJobUnit::One, "steps", true, WallJobData::None},
    {"set-offset", wl_OpCode_OPC_SET_OFFSET, WallJobUnit::One, "offset", true, WallJobData::None},
    {"self-test", wl_OpCode_OPC_SELF_TEST, WallJobUnit::One, nullptr, false, WallJobData::Json},
    {"restart-unit", wl_OpCode_OPC_RESTART_UNIT, WallJobUnit::One, nullptr, false,
     WallJobData::None},
    {"reset-odometer", wl_OpCode_OPC_RESET_ODOMETER, WallJobUnit::One, nullptr, false,
     WallJobData::None},
    {"set-gates", wl_OpCode_OPC_SET_GATES, WallJobUnit::One, "gates", true, WallJobData::None},
    {"boot-info", wl_OpCode_OPC_BOOT_INFO, WallJobUnit::One, nullptr, false, WallJobData::Json},
    {"boot-dump", wl_OpCode_OPC_BOOT_DUMP, WallJobUnit::One, nullptr, false, WallJobData::Bytes},
    {"boot-update", wl_OpCode_OPC_BOOT_UPDATE, WallJobUnit::One, nullptr, false,
     WallJobData::None},
    // args.force: 1 = also when the unit is current (one unit only).
    {"update-units", wl_OpCode_OPC_UPDATE_UNITS, WallJobUnit::OneOrAll, "force", false,
     WallJobData::None},
    {"probe", wl_OpCode_OPC_PROBE, WallJobUnit::None, nullptr, false, WallJobData::None},
    // args.address: where the unit answers from now on. Its present address
    // stores what its switches give; "burn all" is this for every unit.
    {"set-address", wl_OpCode_OPC_SET_ADDRESS, WallJobUnit::One, "address", true,
     WallJobData::None},
    {"clear-address", wl_OpCode_OPC_CLEAR_ADDRESS, WallJobUnit::One, nullptr, false,
     WallJobData::None},
    // Every unit of the row finds home again, then the row's text returns.
    {"home-all", wl_OpCode_OPC_HOME_ALL, WallJobUnit::None, nullptr, false, WallJobData::None},
};

inline const WallJobKind* wallJobFind(const char* name) {
  for (const WallJobKind& kind : WALL_JOB_KINDS) {
    if (strcmp(kind.name, name) == 0) return &kind;
  }
  return nullptr;
}

inline const WallJobKind* wallJobFind(wl_OpCode opcode) {
  for (const WallJobKind& kind : WALL_JOB_KINDS) {
    if (kind.opcode == opcode) return &kind;
  }
  return nullptr;
}

// A job's number in the event record: a unit job's is its OpCode on the link,
// the jobs that change the wall count from 200. Fixed, like the OpCodes.
struct WallTableJob {
  const char* name;
  uint8_t number;
};
static const WallTableJob WALL_TABLE_JOBS[] = {
    {"pair", 200},
    {"release", 201},
    {"arrange", 202},
    {"update", 203},
};

// 0 for a name that is no job.
inline uint8_t wallJobNumber(const char* name) {
  if (const WallJobKind* kind = wallJobFind(name)) return (uint8_t)kind->opcode;
  for (const WallTableJob& job : WALL_TABLE_JOBS) {
    if (strcmp(job.name, name) == 0) return job.number;
  }
  return 0;
}

inline const char* wallJobNumberName(uint8_t number) {
  for (const WallTableJob& job : WALL_TABLE_JOBS) {
    if (job.number == number) return job.name;
  }
  if (const WallJobKind* kind = wallJobFind((wl_OpCode)number)) return kind->name;
  return "?";
}

// Fills `op` (all but its id) from what the operator asked for. Returns why
// not, nullptr when the job can be handed to the row.
inline const char* wallJobBuild(const WallJobKind& kind, bool haveUnit, long unit, bool haveArg,
                                long arg, wl_Op& op) {
  op = wl_Op_init_zero;
  op.opcode = kind.opcode;
  if (kind.unit == WallJobUnit::None) {
    if (haveUnit) return "this job is for the whole row: leave target.unit out";
  } else if (haveUnit) {
    // The range a unit address can have at all; which of them hold a unit
    // is the row's to say.
    if (unit < 1 || unit > 126) return "target.unit is the unit's address, 1 to 126";
    op.address = (uint32_t)unit;
  } else if (kind.unit == WallJobUnit::One) {
    return "target.unit is required: the unit's address";
  }
  if (kind.arg == nullptr) {
    if (haveArg) return "this job takes no args";
    return nullptr;
  }
  if (!haveArg) {
    if (kind.argRequired) return "this job needs its value under args";
    return nullptr;
  }
  MaintVerdict verdict;
  switch (kind.opcode) {
    case wl_OpCode_OPC_JOG: verdict = maintValidateJog(arg); break;
    case wl_OpCode_OPC_SET_OFFSET: verdict = maintValidateOffset(arg); break;
    case wl_OpCode_OPC_SET_GATES: verdict = maintValidateGates(arg); break;
    case wl_OpCode_OPC_SET_ADDRESS:
      // What a unit address can be at all; which of them this row can reach
      // and which are free is the row's to say.
      if (arg < 1 || arg > 126) return "args.address is the new address, 1 to 126";
      break;
    case wl_OpCode_OPC_UPDATE_UNITS:
      if (arg != 0 && arg != 1) return "args.force is 1 or 0";
      // Never a whole-row erase of units that are current.
      if (arg == 1 && !haveUnit) return "args.force needs target.unit: one unit at a time";
      break;
    default: break;
  }
  if (verdict.httpStatus != 200) return verdict.message;
  op.arg = (int32_t)arg;
  return nullptr;
}

// Worker task, every pass: closes a job on the master's own units once the
// display has its result (WallJobs.cpp).
void wallOwnJobTick();

// Why a row did not start a job (OpState.reason of OP_REFUSED).
inline const char* wallJobRefusalText(uint32_t refusal) {
  switch (refusal) {
    case wl_OpRefusal_REFUSAL_UNKNOWN_OP: return "the row's firmware has no such job";
    case wl_OpRefusal_REFUSAL_BUSY: return "another unit job holds the row";
    case wl_OpRefusal_REFUSAL_RESCUE: return "the row is in rescue mode and leaves its units alone";
    case wl_OpRefusal_REFUSAL_BAD_ADDRESS: return "no such unit address on that row";
    case wl_OpRefusal_REFUSAL_NO_UNIT: return "no running unit at that address";
    case wl_OpRefusal_REFUSAL_UNIT_PROTOCOL:
      return "the unit speaks another protocol version: update it";
    case wl_OpRefusal_REFUSAL_BAD_ARG: return "the row refused the job's value";
    case wl_OpRefusal_REFUSAL_NO_MEMORY: return "the row is out of memory for this job";
    case wl_OpRefusal_REFUSAL_ADDRESS_TAKEN:
      return "another unit already answers at that address";
    default: return "the row refused the job";
  }
}

// How a job that ran ended, in the words of the job results: the outcome
// and, where there is one, the reason. A failure without an outcome is a
// result the board no longer had.
inline void wallJobOutcomeText(char* out, size_t cap, bool ok, uint32_t outcome, uint32_t reason) {
  const char* why = maintReasonName((MaintReason)reason);
  if (ok) {
    snprintf(out, cap, "%s", why[0] != 0 ? why : "ok");
  } else if (outcome == (uint32_t)MaintOutcome::Pending) {
    snprintf(out, cap, "the result was lost on the board");
  } else if (why[0] != 0) {
    snprintf(out, cap, "%s: %s", maintOutcomeName((MaintOutcome)outcome), why);
  } else {
    snprintf(out, cap, "%s", maintOutcomeName((MaintOutcome)outcome));
  }
}
