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

enum class WallOpPhase : uint8_t { Running, Done, Failed };

struct WallOp {
  uint32_t id = 0;  // 0 = this place is empty
  WallOpPhase phase = WallOpPhase::Running;
  int8_t row = -1;  // the row it runs on, -1 = none (index in the rows table)
  char name[16] = {0};
  char detail[96] = {0};  // Done: the result; Failed: the reason
};

struct WallOps {
  WallOp ops[WALL_OPS_KEPT];
  uint32_t nextId = 1;

  // The new job's id, 0 when every place holds a running job.
  uint32_t begin(const char* name, int row) {
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
