// RestartUnits.cpp — see RestartUnits.h. One unit at a time: the next
// restart-unit job starts only when the one before has ended and the unit had
// its time to boot and find home (RestartUnitsPlan.h), so one unit's inrush
// is never another's.
#include "RestartUnits.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <memory>

#include "DisplayCommand.h"
#include "HelpersSerialHandling.h"
#include "ReflashPlan.h"
#include "ReleaseUpdate.h"
#include "RestartUnitsPlan.h"
#include "Tasks.h"
#include "WallJobs.h"
#include "WallState.h"

namespace {

SemaphoreHandle_t mutex = nullptr;
struct Lock {
  Lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mutex); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

// Under the mutex: begun by a web or MQTT task, stepped by the worker.
RestartUnitsPlan plan;
// The rows table the plan was made from: a job for a row board is handed
// over with it, and refused when the table changed meanwhile.
uint32_t generation = 0;

const WallJobKind& restartKind() {
  static const WallJobKind* kind = wallJobFind("restart-unit");
  return *kind;
}

// Starts the unit job of the unit whose turn it is. Worker task, under the mutex.
void startCurrent(uint32_t nowMs) {
  int row;
  uint8_t address;
  if (!restartUnitsCurrent(plan, row, address)) return;
  wl_Op op;
  if (wallJobBuild(restartKind(), true, address, false, 0, op) != nullptr) {
    restartUnitsStartRefused(plan, nowMs);
    return;
  }
  const int tableRow = row == RESTART_UNITS_OWN_ROW ? WALL_OP_OWN_ROW : row;
  bool rowBusy = false;
  op.op_id = wallJobBegin(restartKind().name, tableRow, rowBusy, address);
  if (op.op_id == 0) {
    if (rowBusy) {
      restartUnitsStartBusy(plan, nowMs);
    } else {
      restartUnitsStartRefused(plan, nowMs);
    }
    return;
  }
  if (tableRow == WALL_OP_OWN_ROW) {
    const uint32_t seq = displayNextMaintSeq();
    if (!displayEnqueue(makeRebootToBootloaderCommand(seq, address))) {
      wallOpFinish(op.op_id, false, "the display queue was full");
      restartUnitsStartRefused(plan, nowMs);
      return;
    }
    WallOwnJob job;
    job.opId = op.op_id;
    job.seq = seq;
    job.startedMs = nowMs;
    job.op = op;
    wallOwnJobSet(job);
  } else if (!wallJobStage(tableRow, op, generation)) {
    wallOpFinish(op.op_id, false, "the row could not be handed the job");
    restartUnitsStartRefused(plan, nowMs);
    return;
  }
  restartUnitsStarted(plan, op.op_id, nowMs);
}

}  // namespace

void restartUnitsInit() { mutex = xSemaphoreCreateMutex(); }

bool restartUnitsRunning() {
  Lock lock;
  return plan.step != RestartUnitsStep::Idle;
}

// A unit update or a release update has the wall: no unit is restarted
// under it, and the campaign does not start over one.
static bool gatesClosed() {
  return reflashInProgress(displaySnapshotGet().reflash) || wallUnitUpdateRunning() ||
         releaseUpdateRunning();
}

const char* restartUnitsStart(uint32_t& op, int& status) {
  status = 409;
  if (gatesClosed()) return "a firmware update is running, retry when it has finished";
  // The boards and their units at this moment: the own row, then every row
  // board that is connected and not in rescue mode. Read before the lock,
  // and off this task's stack (a web or MQTT task's).
  RestartUnitsBoard boards[RESTART_UNITS_BOARDS_MAX];
  int count = 0;
  const uint8_t ownWidth = (uint8_t)displaySnapshotGet().displayWidth;
  if (ownWidth > 0) boards[count++] = {RESTART_UNITS_OWN_ROW, ownWidth};
  const uint32_t gen = wallStateRowsGeneration();
  std::unique_ptr<WallSnapshot> wall(new WallSnapshot(wallStateGet()));
  for (int i = 0; i < wall->rows.count && count < RESTART_UNITS_BOARDS_MAX; i++) {
    if (wallRowIsOwn(wall->rows.rows[i])) continue;
    const WallRowLink& link = wall->link[i];
    if (!link.contact.connected || !link.contact.helloSeen || link.rescue) continue;
    if (wall->rows.rows[i].width == 0) continue;
    boards[count++] = {(int8_t)i, wall->rows.rows[i].width};
  }
  wall.reset();
  // One campaign at a time: the check, the job and the plan under one lock,
  // so two starters cannot both begin one (the second's job would be left
  // running for good).
  Lock lock;
  if (plan.step != RestartUnitsStep::Idle) return "a restart of the units is running";
  status = 503;
  op = wallOpBegin("restart-units", -1);
  if (op == 0) return "too many jobs are running";
  generation = gen;
  restartUnitsBegin(plan, op, boards, count, millis());
  SerialPrintf("restart-units: %d boards, one unit at a time\n", count);
  status = 0;
  return nullptr;
}

void restartUnitsTick() {
  Lock lock;
  const uint32_t nowMs = millis();
  switch (plan.step) {
    case RestartUnitsStep::Idle:
      return;
    case RestartUnitsStep::Start:
      // The gates are looked at before every unit: a unit update or a release
      // update that began meanwhile ends the campaign, with what was done.
      if (gatesClosed()) {
        restartUnitsStop(plan);
        return;
      }
      startCurrent(nowMs);
      return;
    case RestartUnitsStep::Wait: {
      WallOp unitOp;
      if (!wallOpGet(plan.unitOp, unitOp)) {
        // Finished and already pushed out of the job table by later jobs:
        // how it ended is not known, the row is free.
        restartUnitsJobEnded(plan, false, nowMs);
      } else if (unitOp.phase != WallOpPhase::Running) {
        restartUnitsJobEnded(plan, unitOp.phase == WallOpPhase::Done, nowMs);
      } else {
        restartUnitsTick(plan, nowMs);
      }
      return;
    }
    case RestartUnitsStep::Settle:
      restartUnitsTick(plan, nowMs);
      return;
    case RestartUnitsStep::Done: {
      char text[WALL_OP_DETAIL_MAX];
      restartUnitsSummary(text, sizeof(text), plan);
      wallOpFinish(plan.opId, plan.failed == 0, text);
      SerialPrintf("restart-units: %s\n", text);
      plan = RestartUnitsPlan();
      return;
    }
  }
}
