#pragma once
// WallWatch — the one place that judges the wall (#559/#570). Once a second
// the worker task turns what the master knows (its own units from the display
// snapshot, every row board from WallState) into verdicts (UnitVerdict.h,
// BoardVerdict.h), publishes them for the API and Home Assistant, and records
// what changed since the last look in the event record (WallWatchPolicy.h).
//
// Readers take a copy; nobody else judges, so the page, the sensor and the
// record cannot disagree.

#include "BoardVerdict.h"
#include "ClusterLayout.h"  // CLUSTER_MAX_MEMBERS

// The master's own row is always judged, also when the rows table is empty.
#define WALL_VERDICT_BOARDS (CLUSTER_MAX_MEMBERS + 1)

struct WallVerdictBoard {
  bool own = false;
  int8_t row = -1;  // its index in the rows table; -1 = the own row of a master on its own
  BoardVerdict verdict;
  uint8_t units = 0;  // how many of `unit` are judged; 0 = its units are not known
  UnitVerdict unit[UNITS_AMOUNT];
};

struct WallVerdicts {
  bool valid = false;  // false until the first look
  // The rows table the `row` numbers belong to (wallStateRowsGeneration()).
  uint32_t rowsGeneration = 0;
  VerdictLevel wall = VerdictLevel::Working;  // the worst board's
  uint8_t count = 0;
  WallVerdictBoard boards[WALL_VERDICT_BOARDS];

  // The master's own row / the board at a place in the rows table.
  const WallVerdictBoard* own() const {
    for (int i = 0; i < count; i++) {
      if (boards[i].own) return &boards[i];
    }
    return nullptr;
  }
  const WallVerdictBoard* ofRow(int row) const {
    for (int i = 0; i < count; i++) {
      if (boards[i].row == row) return &boards[i];
    }
    return nullptr;
  }
};

// setup(), before tasksInit().
void wallWatchInit();

// Worker task only: self-throttled to one look a second.
void wallWatchTick();

// Any task: a copy of the latest verdicts into `out`. False (and `out`
// untouched) before the first look, or when they are of another rows table
// than `rowsGeneration`.
bool wallVerdictsGet(WallVerdicts& out, uint32_t rowsGeneration);
