#pragma once
// UnitUpdateJob.h — what a row master does around the flash loop so that one
// job brings a row's units fully up to date: application firmware
// (ReflashPlan.h) and then the boot section (BootUpdateOp.h). Both row
// masters run it; each tree supplies the bus and its log wording.
//
// Rotations are the cost being managed. A reset loses a unit's drum position,
// so a reflashed unit must home once — that one turn is unavoidable. Nothing
// here adds another: the row is held idle rather than homed before the job,
// the boot sweep runs AFTER the post-flash home (stage 2 needs a homed unit
// and does not move it), and a unit that is already homed is not homed again.
//
// Natively tested by test_unit_update_job.

#include <stdint.h>

#include "BootIntegrity.h"
#include "BootUpdateOp.h"
#include "ReflashPlan.h"
#include "UnitHealth.h"

// How long the job waits for a row still finishing a move before it sends the
// first unit into its bootloader. Bounded: a unit that never reports idle is
// a unit that may need this very reflash, so the job goes ahead after it.
#define UNIT_UPDATE_QUIET_MS 8000UL

// The sketch-mode units of a row, for the idle wait. out[] >= width entries.
inline int unitUpdateCollectSketchUnits(const UnitFacts* units, int width,
                                        int base, uint8_t* out) {
  int n = 0;
  for (int i = 0; i < width; i++) {
    if (units[i].state == 1) out[n++] = (uint8_t)(base + i);
  }
  return n;
}

// Units whose boot section this job should look at: running the bundled
// application (an older one may not know the boot report or its states) and
// not already judged to carry the current image. An unread verdict is
// included — the op reads the report itself and leaves a current unit alone.
// out[] >= width entries.
inline int unitUpdateCollectBootTargets(const UnitFacts* units, int width,
                                        int base, uint8_t* out) {
  int n = 0;
  for (int i = 0; i < width; i++) {
    const UnitFacts& u = units[i];
    if (u.state != 1 || u.fwStatus != 0) continue;
    if (u.bootVerdict == BOOT_INTEGRITY_OK) continue;
    out[n++] = (uint8_t)(base + i);
  }
  return n;
}

struct BootSweepEnd {
  bool cancelled = false;  // the tree asked to stop
  bool halted = false;     // consecutive failures stopped the sweep
};

// Runs the boot update on each target in turn and keeps the progress object
// current. A unit already on the current image counts as neither done nor
// failed. Two failures back to back end the sweep, as in the flash loop: a
// boot image that will not go on is not tried on the rest of the row.
//
//   bool       stopRequested()               checked before each unit
//   MaintGrade bootUpdate(uint8_t addr)      bootUpdateRun with the tree's hooks
//   void       progressChanged()             publish the progress object
//   void       sweepHalted(uint8_t consecutiveFailures, int unitsLeftUntouched)
template <typename Hooks>
inline BootSweepEnd unitUpdateRunBootSweep(Hooks& h, const uint8_t* targets,
                                           int total,
                                           ReflashProgress& progress) {
  BootSweepEnd end;
  uint8_t consecutiveFailures = 0;
  for (int k = 0; k < total; k++) {
    if (h.stopRequested()) {
      end.cancelled = true;
      break;
    }
    progress.state = ReflashState::BootUpdate;
    progress.currentAddr = targets[k];
    h.progressChanged();

    MaintGrade grade = h.bootUpdate(targets[k]);
    if (grade.outcome != MaintOutcome::Ok) {
      progress.bootFailed++;
      if (consecutiveFailures < 0xFF) consecutiveFailures++;
    } else {
      consecutiveFailures = 0;
      if (grade.reason != MaintReason::BootAlreadyNew) progress.bootDone++;
    }
    h.progressChanged();

    if (reflashShouldHalt(consecutiveFailures)) {
      h.sweepHalted(consecutiveFailures, total - (k + 1));
      end.halted = true;
      break;
    }
  }
  progress.currentAddr = 0;
  return end;
}
