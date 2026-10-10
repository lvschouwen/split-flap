#pragma once
// HomeWait.h — how a row master waits for a unit's home search and what the
// unit then says of it (#605). Pure: each tree reads the unit's status and
// feeds it in, on its own schedule (the S3 blocks displayTask, the ESP-01
// polls from loop()). Natively tested by test_home_wait.
//
// SFP_CMD_HOME is acknowledged before anything turns: the unit sets a flag in
// its TWI interrupt and its loop starts the search after whatever move is in
// flight. The acknowledgement says nothing about the search, so the job ends
// on the unit's own status once the drum has stood still.
//
// Between the end of a move and the start of the search queued behind it the
// unit reads idle for about 100 ms, so one idle reading does not end the wait:
// the drum has to read idle for HOME_WAIT_IDLE_MS.
//
// A unit refuses SFP_CMD_HOME for 30 s after a failed search
// (Unit/UnitHomePolicy.h) and keeps reporting that failure: the job fails the
// same way, which is what the unit's state is.

#include <stdint.h>

#include "MaintenancePolicy.h"  // MaintGrade
#include "UnitHealth.h"         // UNIT_FLAG_*

// A failed search turns the drum three times at homing speed (~20 s) and may
// queue behind a move in flight.
#define HOME_WAIT_TIMEOUT_MS 45000UL
#define HOME_WAIT_POLL_MS 250UL
#define HOME_WAIT_IDLE_MS 500UL

enum class HomeWaitOutcome : uint8_t {
  Pending = 0,
  Found,     // the drum stands still and the unit calls itself homed
  NotFound,  // the drum stands still and the unit reports a failed search
  NotEnded,  // still turning when the wait ran out
  NoAnswer,  // no status read came through in the whole wait
};

struct HomeWait {
  uint32_t startMs = 0;
  uint32_t idleSinceMs = 0;
  bool idle = false;
  bool everRead = false;
};

inline void homeWaitBegin(HomeWait& w, uint32_t nowMs) {
  w = HomeWait{};
  w.startMs = nowMs;
}

// What a unit's status flags say of its last search: it failed, or the unit
// has no home position.
inline bool homeFlagsSayNotFound(uint8_t flags) {
  return (flags & UNIT_FLAG_LAST_HOME_FAILED) != 0 ||
         (flags & UNIT_FLAG_HOMED) == 0;
}

// Folds one status read in. Pending while the search is undecided; any other
// value is final.
inline HomeWaitOutcome homeWaitObserve(HomeWait& w, bool readOk, uint8_t flags,
                                       uint32_t nowMs) {
  if (readOk) {
    w.everRead = true;
    if ((flags & UNIT_FLAG_MOVING) != 0) {
      w.idle = false;
    } else {
      if (!w.idle) {
        w.idle = true;
        w.idleSinceMs = nowMs;
      }
      if ((uint32_t)(nowMs - w.idleSinceMs) >= HOME_WAIT_IDLE_MS) {
        return homeFlagsSayNotFound(flags) ? HomeWaitOutcome::NotFound
                                           : HomeWaitOutcome::Found;
      }
    }
  } else {
    // Silence is not a drum standing still.
    w.idle = false;
  }
  if ((uint32_t)(nowMs - w.startMs) >= HOME_WAIT_TIMEOUT_MS) {
    return w.everRead ? HomeWaitOutcome::NotEnded : HomeWaitOutcome::NoAnswer;
  }
  return HomeWaitOutcome::Pending;
}

inline MaintGrade maintGradeHome(HomeWaitOutcome outcome) {
  switch (outcome) {
    case HomeWaitOutcome::Found:
      return {MaintOutcome::Ok, MaintReason::None};
    case HomeWaitOutcome::NotFound:
      return {MaintOutcome::PostconditionFail, MaintReason::HomeNotFound};
    case HomeWaitOutcome::NotEnded:
      return {MaintOutcome::PostconditionFail, MaintReason::HomeNotEnded};
    default:
      return {MaintOutcome::WireFail, MaintReason::None};
  }
}

// Every unit of a row was sent home: the job failed when any unit that
// answered reports a failed search. A unit that does not answer is the
// verdicts' to report, not this job's.
struct HomeAllTally {
  uint8_t notFound = 0;
};

inline void homeAllTallyAdd(HomeAllTally& t, bool readOk, uint8_t flags) {
  if (readOk && homeFlagsSayNotFound(flags) && t.notFound < 0xFF) t.notFound++;
}

inline MaintGrade maintGradeHomeAll(const HomeAllTally& t) {
  if (t.notFound == 0) return {MaintOutcome::Ok, MaintReason::None};
  return {MaintOutcome::PostconditionFail, MaintReason::HomeNotFound};
}
