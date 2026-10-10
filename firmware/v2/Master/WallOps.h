#pragma once
// WallOps.h — the jobs an operator started through POST /api/v2/action and
// can ask about with GET /api/v2/op/{id} (#559/#566): pairing a row, releasing
// one, and the unit jobs. Pure, natively tested by test_wall_ops; WallState
// holds the one instance under its mutex.
//
// The master names every job with its own id. Finished jobs are kept until
// their place is needed; a running job is never pushed out, so a full table
// refuses the next one instead.

#include <stdint.h>
#include <string.h>

#define WALL_OPS_KEPT 16
#define WALL_OP_DETAIL_MAX 96
// WallOp.row of a job on the master's own units (which need not be in the
// rows table at all).
#define WALL_OP_OWN_ROW (-2)

enum class WallOpPhase : uint8_t { Running, Done, Failed };

struct WallOp {
  uint32_t id = 0;  // 0 = this place is empty
  WallOpPhase phase = WallOpPhase::Running;
  // The row it runs on: its index in the rows table, WALL_OP_OWN_ROW, or -1
  // for a job that is not about one row.
  int8_t row = -1;
  uint8_t unit = 0;  // the unit's bus address, 0 = not about one unit
  char name[24] = {0};
  char detail[WALL_OP_DETAIL_MAX] = {0};  // Done: the result; Failed: the reason
};

struct WallOps {
  WallOp ops[WALL_OPS_KEPT];
  uint32_t nextId = 1;

  // The new job's id, 0 when every place holds a running job.
  uint32_t begin(const char* name, int row, uint8_t unit = 0) {
    WallOp* place = nullptr;
    for (WallOp& op : ops) {
      if (op.id == 0) {
        place = &op;
        break;
      }
      if (op.phase == WallOpPhase::Running) continue;
      if (place == nullptr || op.id < place->id) place = &op;
    }
    if (place == nullptr) return 0;
    *place = WallOp{};
    place->id = nextId++;
    place->row = (int8_t)row;
    place->unit = unit;
    strncpy(place->name, name, sizeof(place->name) - 1);
    return place->id;
  }

  // False when the job is unknown or already finished.
  bool finish(uint32_t id, bool ok, const char* detail) {
    WallOp* op = findMutable(id);
    if (op == nullptr || op->phase != WallOpPhase::Running) return false;
    op->phase = ok ? WallOpPhase::Done : WallOpPhase::Failed;
    strncpy(op->detail, detail, sizeof(op->detail) - 1);
    return true;
  }

  // Fails every job still running on a row; returns how many.
  int failRow(int row, const char* reason) {
    int failed = 0;
    for (WallOp& op : ops) {
      if (op.id != 0 && op.row == row && op.phase == WallOpPhase::Running) {
        finish(op.id, false, reason);
        failed++;
      }
    }
    return failed;
  }

  // Fails every job still running on a row board: the rows table changed, and
  // with it what a row's number means.
  int failRowBoards(const char* reason) {
    int failed = 0;
    for (WallOp& op : ops) {
      if (op.id != 0 && op.row >= 0 && op.phase == WallOpPhase::Running) {
        finish(op.id, false, reason);
        failed++;
      }
    }
    return failed;
  }

  // One unit job at a time per row.
  bool runningOn(int row) const {
    for (const WallOp& op : ops) {
      if (op.id != 0 && op.row == row && op.phase == WallOpPhase::Running) return true;
    }
    return false;
  }

  // Is a job of this name running on this row?
  bool runningOn(int row, const char* name) const {
    for (const WallOp& op : ops) {
      if (op.id != 0 && op.row == row && op.phase == WallOpPhase::Running &&
          strcmp(op.name, name) == 0) {
        return true;
      }
    }
    return false;
  }

  // Is a job of this name running on any row board?
  bool runningOnARowBoard(const char* name) const {
    for (const WallOp& op : ops) {
      if (op.id != 0 && op.row >= 0 && op.phase == WallOpPhase::Running &&
          strcmp(op.name, name) == 0) {
        return true;
      }
    }
    return false;
  }

  // Where a job is kept, for what is held beside the table; -1 when unknown.
  int placeOf(uint32_t id) const {
    if (id == 0) return -1;
    for (int i = 0; i < WALL_OPS_KEPT; i++) {
      if (ops[i].id == id) return i;
    }
    return -1;
  }

  const WallOp* find(uint32_t id) const { return const_cast<WallOps*>(this)->findMutable(id); }

 private:
  WallOp* findMutable(uint32_t id) {
    if (id == 0) return nullptr;
    for (WallOp& op : ops) {
      if (op.id == id) return &op;
    }
    return nullptr;
  }
};
