// WallJobs.cpp — the end of a unit job on the master's own units (#559/#566).
// The web side starts it as a DisplayCommand and notes its seq
// (wallOwnJobSet); the worker task watches the display's result slots for
// that seq here and closes the operator's job, so a job on the own row and
// one on a row board are asked about the same way (GET /api/v2/op/{id}).
// The job table is WallJobs.h.
#include "WallJobs.h"

#include <Arduino.h>

#include "BootDump.h"
#include "BootInfo.h"
#include "Tasks.h"
#include "WallState.h"

static_assert(BOOT_SECTION_LEN <= WALL_OP_DATA_MAX, "a boot section must fit a job's result");

void wallOwnJobTick() {
  WallOwnJob job;
  if (!wallOwnJobGet(job)) return;
  // Worker task only; too large for its stack.
  static DisplaySnapshot snap;
  snap = displaySnapshotGet();
  const OpResultState state = opResultQuery(snap.lastMaint, job.seq);
  if (state == OpResultState::Pending) {
    // The display runs one command at a time: the job's own time, and as
    // long again as a row board gets to take one, for what was queued first.
    if (!wallLinkElapsed(millis(), job.startedMs, wallJobRunMs(job.op) + WALL_JOB_HAND_OVER_MS)) {
      return;
    }
    wallOpFinish(job.opId, false, "no result from the display in time");
    wallOwnJobClear(job.opId);
    return;
  }
  if (state == OpResultState::Expired) {
    wallOpFinish(job.opId, false, "the result was overwritten by a later job");
    wallOwnJobClear(job.opId);
    return;
  }
  bool ok = snap.lastMaint.outcome == MaintOutcome::Ok;
  char text[sizeof(((WallOp*)0)->detail)];
  wallJobOutcomeText(text, sizeof(text), ok, (uint32_t)snap.lastMaint.outcome,
                     (uint32_t)snap.lastMaint.reason);
  if (job.op.opcode == wl_OpCode_OPC_SELF_TEST) {
    char json[128];
    buildSelfTestJson(json, sizeof(json), snap.lastSelfTest, job.seq);
    wallOpDataPut(job.opId, 0, (const uint8_t*)json, strlen(json));
  } else if (job.op.opcode == wl_OpCode_OPC_BOOT_INFO) {
    char json[BOOT_INFO_JSON_CAP];
    const size_t n = buildBootInfoJson(json, sizeof(json), snap.lastBootInfo, job.seq);
    wallOpDataPut(job.opId, 0, (const uint8_t*)json, n);
  } else if (job.op.opcode == wl_OpCode_OPC_BOOT_DUMP && ok) {
    static uint8_t bytes[BOOT_SECTION_LEN];
    if (displayBootDumpCopy(job.seq, bytes)) {
      wallOpDataPut(job.opId, 0, bytes, sizeof(bytes));
    } else {
      ok = false;
      snprintf(text, sizeof(text), "the dump was overwritten by a later one");
    }
  }
  wallOpFinish(job.opId, ok, text);
  wallOwnJobClear(job.opId);
}
