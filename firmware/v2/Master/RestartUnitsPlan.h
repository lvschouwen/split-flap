#pragma once
// RestartUnitsPlan.h — a restart of every unit of the wall, one unit at a
// time (#624). Pure, natively tested by test_restart_units; the worker task
// runs it (RestartUnits.cpp) through the unit jobs the API has: a
// `restart-unit` job per unit, the next one only once the one before has
// ended and had its time to boot and find home, so one unit at a time draws
// its inrush. A unit's since-boot supply minimum starts over with its
// restart; that is what the job is for.
#include <stdint.h>
#include <stdio.h>

#include "ClusterLayout.h"   // CLUSTER_MAX_MEMBERS
#include "UnitTimings.h"     // UNIT_HOME_TIMEOUT_MS
#include "WallLinkPolicy.h"  // WALL_JOB_UNIT_MS, WALL_JOB_HAND_OVER_MS

// The master's own units, which need not be in the rows table at all
// (WALL_OP_OWN_ROW in WallOps.h).
#define RESTART_UNITS_OWN_ROW (-2)
#define RESTART_UNITS_BOARDS_MAX (CLUSTER_MAX_MEMBERS + 1)
// After a restart the unit sits in its bootloader, boots and finds home; the
// next unit waits that long, the time a home search is given.
#define RESTART_UNITS_SETTLE_MS UNIT_HOME_TIMEOUT_MS
// How long one unit's job may take to be started or to end before it counts
// as failed and the next unit is taken: as long as a unit job is given on
// either board (WallJobs.cpp, the link's job table), so a job that ends late
// is not given up on while it still runs.
#define RESTART_UNITS_WAIT_MAX_MS (WALL_JOB_UNIT_MS + WALL_JOB_HAND_OVER_MS)

enum class RestartUnitsStep : uint8_t { Idle, Start, Wait, Settle, Done };

struct RestartUnitsBoard {
  int8_t row = -1;    // index in the rows table, or RESTART_UNITS_OWN_ROW
  uint8_t width = 0;  // its units, addressed 1..width
};

struct RestartUnitsPlan {
  RestartUnitsStep step = RestartUnitsStep::Idle;
  uint32_t opId = 0;  // the wall job
  RestartUnitsBoard boards[RESTART_UNITS_BOARDS_MAX];
  uint8_t boardCount = 0;
  uint8_t board = 0;  // the cursor: the board and the unit (0-based) under way
  uint8_t unit = 0;
  uint32_t unitOp = 0;  // the unit job under way, 0 = none
  uint32_t sinceMs = 0;  // when the step began
  uint8_t done = 0;
  uint8_t failed = 0;
  bool stopped = false;  // ended before every unit had its turn (the gates closed)
};

static inline bool restartUnitsElapsed(uint32_t nowMs, uint32_t sinceMs, uint32_t spanMs) {
  return (uint32_t)(nowMs - sinceMs) >= spanMs;
}

// The next unit, or the end.
static inline void restartUnitsAdvance(RestartUnitsPlan& plan, uint32_t nowMs) {
  plan.unitOp = 0;
  plan.sinceMs = nowMs;
  while (plan.board < plan.boardCount) {
    if (plan.unit + 1 < plan.boards[plan.board].width) {
      plan.unit++;
      plan.step = RestartUnitsStep::Start;
      return;
    }
    plan.board++;
    plan.unit = 0;
    if (plan.board < plan.boardCount && plan.boards[plan.board].width > 0) {
      plan.step = RestartUnitsStep::Start;
      return;
    }
  }
  plan.step = RestartUnitsStep::Done;
}

inline void restartUnitsBegin(RestartUnitsPlan& plan, uint32_t opId, const RestartUnitsBoard* boards,
                              int count, uint32_t nowMs) {
  plan = RestartUnitsPlan();
  plan.opId = opId;
  for (int i = 0; i < count && plan.boardCount < RESTART_UNITS_BOARDS_MAX; i++) {
    if (boards[i].width == 0) continue;
    plan.boards[plan.boardCount++] = boards[i];
  }
  plan.sinceMs = nowMs;
  plan.step = plan.boardCount > 0 ? RestartUnitsStep::Start : RestartUnitsStep::Done;
}

// The unit whose turn it is, while one is.
inline bool restartUnitsCurrent(const RestartUnitsPlan& plan, int& row, uint8_t& address) {
  if (plan.step == RestartUnitsStep::Idle || plan.step == RestartUnitsStep::Done) return false;
  row = plan.boards[plan.board].row;
  address = (uint8_t)(plan.unit + 1);
  return true;
}

inline void restartUnitsStarted(RestartUnitsPlan& plan, uint32_t unitOp, uint32_t nowMs) {
  plan.unitOp = unitOp;
  plan.sinceMs = nowMs;
  plan.step = RestartUnitsStep::Wait;
}

// The row would not take the job: the unit counts as failed, the next is taken.
inline void restartUnitsStartRefused(RestartUnitsPlan& plan, uint32_t nowMs) {
  plan.failed++;
  restartUnitsAdvance(plan, nowMs);
}

// The row runs another job: wait for it, within the limit. False once the
// wait is over and the unit has been given up on.
inline bool restartUnitsStartBusy(RestartUnitsPlan& plan, uint32_t nowMs) {
  if (!restartUnitsElapsed(nowMs, plan.sinceMs, RESTART_UNITS_WAIT_MAX_MS)) return true;
  restartUnitsStartRefused(plan, nowMs);
  return false;
}

// The wall is taken by a unit update or a release update: the units left
// are not restarted; what was done stays done.
inline void restartUnitsStop(RestartUnitsPlan& plan) {
  plan.stopped = true;
  plan.unitOp = 0;
  plan.step = RestartUnitsStep::Done;
}

// The unit's job ended. A unit that took the restart gets its time to boot
// and find home; one that did not needs none.
inline void restartUnitsJobEnded(RestartUnitsPlan& plan, bool ok, uint32_t nowMs) {
  if (ok) {
    plan.done++;
    plan.sinceMs = nowMs;
    plan.step = RestartUnitsStep::Settle;
    return;
  }
  plan.failed++;
  restartUnitsAdvance(plan, nowMs);
}

// Every pass: the settle time passing, a job that never ends.
inline void restartUnitsTick(RestartUnitsPlan& plan, uint32_t nowMs) {
  switch (plan.step) {
    case RestartUnitsStep::Settle:
      if (restartUnitsElapsed(nowMs, plan.sinceMs, RESTART_UNITS_SETTLE_MS)) {
        restartUnitsAdvance(plan, nowMs);
      }
      break;
    case RestartUnitsStep::Wait:
      if (restartUnitsElapsed(nowMs, plan.sinceMs, RESTART_UNITS_WAIT_MAX_MS)) {
        plan.failed++;
        restartUnitsAdvance(plan, nowMs);
      }
      break;
    default:
      break;
  }
}

inline void restartUnitsSummary(char* out, size_t cap, const RestartUnitsPlan& plan) {
  if (plan.stopped) {
    snprintf(out, cap, "stopped for a firmware update after %u units", (unsigned)plan.done);
  } else if (plan.done == 0 && plan.failed == 0) {
    snprintf(out, cap, "no units to restart");
  } else if (plan.failed == 0) {
    snprintf(out, cap, "%u units restarted", (unsigned)plan.done);
  } else {
    snprintf(out, cap, "%u units restarted, %u failed", (unsigned)plan.done, (unsigned)plan.failed);
  }
}
