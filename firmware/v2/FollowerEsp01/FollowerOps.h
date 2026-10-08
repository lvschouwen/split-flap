#pragma once
// FollowerOps.h — what is row-only in the unit-job layer (#298), natively
// tested by test_follower_ops. The validators, the job / self-test result
// vocabulary and the reflash plan are the shared MaintenancePolicy.h and
// ReflashPlan.h; this adds the superloop's single staged slot (no display
// command queue — ONE job at a time stamps the result slot) and the unit
// facts buffer sizing.

#include "MaintenancePolicy.h"
#include "ReflashPlan.h"
#include "UnitHealth.h"

// --- staged op (superloop single slot) ---------------------------------------------

enum class FollowerOpKind : uint8_t {
  None = 0,
  WriteOffset,
  Jog,
  Home,
  Identify,
  ResetOdometer,
  SelfTest,
  RebootToBootloader,
  SetGates,
  ReflashUnit,  // #513: reflash one unit; addr = the target, arg 1 = forced.
                // addr 0 = every unit that needs it
  BootUpdate,   // #499: in-system twiboot update (reads info, drives stages)
  BootInfo,     // #499: read-only boot report; result in the BootInfoSlot
  BootDump,     // #522: read the unit's twiboot image over I2C
  Probe,        // rescan the row; done once the scan has run
  SetAddress,   // arg = the address the unit stores; judged by the rescan
  ClearAddress, // back to its switches; judged by the rescan
  HomeAll,      // every unit finds home again, then the row's text returns
};

struct MaintResult {
  uint32_t seq = 0;
  MaintOutcome outcome = MaintOutcome::Pending;
  MaintReason reason = MaintReason::None;
};

// --- unit facts buffer (#519) ---------------------------------------------------
// Sized for the row this board drives, not for a 16-unit worst case: 9.8 KB on
// an 82 KB chip for a document that is 1.6 KB on a 5-unit row. The figure is
// the shared worst case (UNIT_FACTS_DOC_CAP in UnitHealth.h).
inline size_t followerHealthBufCap(int width, int maxUnits) {
  if (width < 0) width = 0;
  if (width > maxUnits) width = maxUnits;
  return UNIT_FACTS_DOC_CAP(width);
}
