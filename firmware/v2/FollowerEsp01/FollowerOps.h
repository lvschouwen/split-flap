#pragma once
// FollowerOps.h — what is row-only in the unit-job layer (#298), natively
// tested by test_follower_ops. The validators, the job / self-test result
// vocabulary and the reflash plan are the shared MaintenancePolicy.h and
// ReflashPlan.h; this adds the superloop's single staged slot (no display
// command queue — ONE job at a time stamps the result slot) and the unit
// facts buffer sizing.

#include "MaintenancePolicy.h"
#include "ReflashPlan.h"

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
};

struct MaintResult {
  uint32_t seq = 0;
  MaintOutcome outcome = MaintOutcome::Pending;
  MaintReason reason = MaintReason::None;
};

// --- unit facts buffer (#519) ---------------------------------------------------
// Sized for the row this board drives, not for a 16-unit worst case: 8 KB on
// an 82 KB chip for a document that is 1.6 KB on a 5-unit row. The per-unit
// figure is the saturated worst case of buildUnitHealthJson (every key family
// present, every value at its widest);
// test_health_json_follower_worst_case_fits_local_buf holds it to that for
// every width.
#define FOLLOWER_HEALTH_BASE_BYTES     448  // headline + wear + reflash splices
#define FOLLOWER_HEALTH_PER_UNIT_BYTES 552

inline size_t followerHealthBufCap(int width, int maxUnits) {
  if (width < 0) width = 0;
  if (width > maxUnits) width = maxUnits;
  return (size_t)FOLLOWER_HEALTH_BASE_BYTES +
         (size_t)width * FOLLOWER_HEALTH_PER_UNIT_BYTES;
}
