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
    case BOOT_STATE_PREV_NEW:
      needStage2 = true;
      break;
    default:
      return {false, false, BOOT_PLAN_UNKNOWN_STATE};
  }
  // Both stages need SPM writes to boot-section pages, so a lock that is
  // readable and closed refuses any update path. An unreadable lock proceeds
  // (bootEffectiveLockByte, #518).
  if (!bootLockPermitsBootWrite(bootEffectiveLockByte(info))) {
    return {false, false, BOOT_PLAN_LOCK_REFUSED};
  }
  return {needStage1, needStage2, BOOT_PLAN_PROCEED};
}

// --- reading what the unit says while a stage runs (#516) -------------------------

// What a result byte means to the operator. BOOT_FAIL_NONE covers both "no
// result yet" and the success codes.
enum BootUpdateFailure : uint8_t {
  BOOT_FAIL_NONE = 0,
  BOOT_FAIL_BUSY,    // drum moving or not homed — home it and retry
  BOOT_FAIL_LOCK,    // lock bits read closed
  BOOT_FAIL_STATE,   // the unit's boot section is not in the state the stage needs
  BOOT_FAIL_VERIFY,  // the unit wrote and its own read-back did not match
};

inline BootUpdateFailure bootResultFailure(uint8_t lastResult) {
  switch (lastResult) {
    case BOOT_RESULT_REFUSED_BUSY:  return BOOT_FAIL_BUSY;
    case BOOT_RESULT_REFUSED_LOCK:  return BOOT_FAIL_LOCK;
    case BOOT_RESULT_REFUSED_STATE: return BOOT_FAIL_STATE;
    case BOOT_RESULT_VERIFY_FAILED: return BOOT_FAIL_VERIFY;
    default:                        return BOOT_FAIL_NONE;
  }
}

enum BootPollVerdict : uint8_t {
  BOOT_POLL_WAIT = 0,  // nothing conclusive yet — keep polling
  BOOT_POLL_DONE,      // the unit reports the new image
  BOOT_POLL_FAILED,    // the unit reports a refusal or a failed verify
};

// One report read while stage 2 is expected to run. `resultBeforeSend` is the
// result byte read before the stage was requested: the unit keeps its last
// result until the next attempt finishes, so a failure code equal to that one
// may be left over from an earlier attempt and proves nothing about this one.
// The state is the authority either way.
inline BootPollVerdict bootStage2Poll(const BootUpdateReport& now,
                                      uint8_t resultBeforeSend) {
  if (now.state == BOOT_STATE_NEW) return BOOT_POLL_DONE;
  if (bootResultFailure(now.lastResult) != BOOT_FAIL_NONE &&
      now.lastResult != resultBeforeSend) {
    return BOOT_POLL_FAILED;
  }
  return BOOT_POLL_WAIT;
}

// Did the unit start stage 1? An accepted request takes it off the bus within
// milliseconds and keeps it off for over a second (page write, watchdog reset,
// bootloader window); a refusing unit keeps answering. `answers()` performs one
// report read and returns whether a valid report came back; `pause(ms)` waits.
// A single failed read is confirmed 50 ms later before it counts — one
// corrupted reply from a unit that refused must not send the driver down the
// "it is rebooting" path.
//
// Only valid when the unit was idle when the request was sent: a unit busy in
// a move holds the request and keeps answering until the move ends.
template <typename Answers, typename Pause>
inline bool bootStage1WentOffBus(Answers&& answers, Pause&& pause) {
  static const uint16_t gaps[2] = {200, 300};
  for (uint16_t gap : gaps) {
    pause(gap);
    if (answers()) continue;
    pause(50);
    if (!answers()) return true;
  }
  return false;
}
