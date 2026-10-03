#pragma once

#include "BootUpdateReport.h"

// Pure decision logic for the master-side boot-update driver (#499). Given a
// unit's boot-info report, returns which stages (if any) are needed, or a
// terminal reason why the update cannot proceed. Both the S3 master and the
// ESP-01 follower call this, eliminating the duplicated state-machine that the
// reviewer flagged. Natively tested (test_boot_update_plan).

enum BootUpdateTerminal : uint8_t {
  BOOT_PLAN_PROCEED = 0,
  BOOT_PLAN_ALREADY_NEW,
  BOOT_PLAN_LOCK_REFUSED,
  BOOT_PLAN_UNKNOWN_STATE,
};

struct BootUpdatePlan {
  bool needStage1;
  bool needStage2;
  BootUpdateTerminal terminal;
};

inline BootUpdatePlan bootUpdateDecide(const BootUpdateReport& info) {
  // State check first: a NEW unit exits before the lock check, so an
  // operator is never told "lock refused" for a unit that needs no update.
  bool needStage1 = false;
  bool needStage2 = false;
  switch (info.state) {
    case BOOT_STATE_NEW:
      return {false, false, BOOT_PLAN_ALREADY_NEW};
    case BOOT_STATE_OLD:
      needStage1 = true;
      needStage2 = true;
      break;
    case BOOT_STATE_PAGE7_INSTALLED:
    case BOOT_STATE_TRAMPOLINE:
      needStage2 = true;
      break;
    default:
      return {false, false, BOOT_PLAN_UNKNOWN_STATE};
  }
  // Both stages need SPM writes to boot-section pages, so the lock must
  // permit it for any update path.
  if (!bootLockPermitsBootWrite(info.lockByte)) {
    return {false, false, BOOT_PLAN_LOCK_REFUSED};
  }
  return {needStage1, needStage2, BOOT_PLAN_PROCEED};
}
