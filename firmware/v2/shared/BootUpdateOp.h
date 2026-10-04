#pragma once
// BootUpdateOp.h — the in-system twiboot update of one unit (#499), as one
// sequence both row masters run. Reads the unit's boot report, decides which
// stages it needs (BootUpdatePlan.h), drives them and verifies. Stage 1
// installs do_spm into the empty page 7 and ends in a WDT reset that passes
// through twiboot; stage 2 rewrites pages 0-6 with TWI dark for ~100 ms.
// Typical wall time ~10 s; every wait at its timeout is about 47 s.
//
// A function template over the tree's hooks, so no tree restates the order,
// the timeouts or the grading. Natively tested by test_boot_update_op.
//
//   bool     readBootInfo(uint8_t addr, BootUpdateReport& out)
//   bool     waitIdle(uint8_t addr, uint32_t timeoutMs)   false = still moving
//   int      sendStage(uint8_t addr, uint8_t stage)       0 = ACKed
//   int      home(uint8_t addr)                           0 = ACKed
//   bool     isHomed(uint8_t addr)   a FRESH read: homed since its last boot
//   void     unitLeftSketch(uint8_t addr)   reads are stale, hold the probes
//   void     holdProbes()                   keep runtime probes off the unit
//   void     pause(uint32_t ms)
//   uint32_t nowMs()
//   void     reshow()                       put the row's last frame back
//   void     note(BootUpdateStep, MaintReason, const BootUpdateReport&)

#include <stdint.h>

#include "BootUpdatePlan.h"
#include "MaintenancePolicy.h"
#include "UnitTimings.h"

#define BOOT_UPDATE_STAGE2_SETTLE_MS 300UL
#define BOOT_UPDATE_STAGE2_POLL_MS 5000UL
// A request sent mid-move is held by the unit until the move ends and would
// then run behind this op's back, so the drum settles first.
#define BOOT_UPDATE_IDLE_MS 8000UL

// The stage 1 start probes run inside the window in which runtime probes are
// held off. That is safe only because a report request is not one of the
// first bytes the bootloader pins itself on (0x00..0x02): it answers it by
// leaving for the sketch.
static_assert(SFP_CMD_GET_BOOT_INFO > 0x02,
              "GET_BOOT_INFO would pin twiboot — the stage 1 start probes "
              "would then hold a unit in its bootloader");

// Where the op is when it notes something; MaintReason::None = no failure.
enum class BootUpdateStep : uint8_t {
  ReadInfo = 0,
  Plan,
  Settle,
  Stage1,
  Stage2,
  Done,
};

template <typename Hooks>
inline MaintGrade bootUpdateRun(Hooks& h, uint8_t addr) {
  BootUpdateReport info;
  // Once the unit was reset or homed the row no longer shows its frame.
  bool moved = false;
  auto finish = [&](BootUpdateStep step, MaintOutcome outcome,
                    MaintReason why) {
    if (moved) h.reshow();
    h.note(step, why, info);
    return MaintGrade{outcome, why};
  };
  auto failed = [&](BootUpdateStep step, MaintReason why) {
    return finish(step, MaintOutcome::PostconditionFail, why);
  };

  if (!h.readBootInfo(addr, info)) {
    return failed(BootUpdateStep::ReadInfo, MaintReason::BootInfoReadFail);
  }
  BootUpdatePlan plan = bootUpdateDecide(info);
  switch (plan.terminal) {
    case BOOT_PLAN_PROCEED:
      break;
    case BOOT_PLAN_ALREADY_NEW:
      return finish(BootUpdateStep::Plan, MaintOutcome::Ok,
                    MaintReason::BootAlreadyNew);
    case BOOT_PLAN_LOCK_REFUSED:
      return failed(BootUpdateStep::Plan, MaintReason::BootLockRefused);
    default:
      return failed(BootUpdateStep::Plan, MaintReason::BootStateUnknown);
  }
  if (!h.waitIdle(addr, BOOT_UPDATE_IDLE_MS)) {
    return failed(BootUpdateStep::Settle, MaintReason::BootUnitBusy);
  }

  if (plan.needStage1) {
    if (h.sendStage(addr, 1) != 0) {
      return finish(BootUpdateStep::Stage1, MaintOutcome::WireFail,
                    MaintReason::None);
    }
    h.unitLeftSketch(addr);
    bool started = bootStage1WentOffBus(
        [&]() {
          BootUpdateReport still;
          if (!h.readBootInfo(addr, still)) return false;
          info = still;
          return true;
        },
        [&](uint16_t ms) { h.pause(ms); });
    if (!started) {
      // Nothing moved and nothing was written: report what the unit said.
      return failed(BootUpdateStep::Stage1,
                    maintReasonForBootFailure(
                        bootResultFailure(info.lastResult),
                        MaintReason::BootNotStarted));
    }
    moved = true;
    h.waitIdle(addr, UNIT_RETURN_TIMEOUT_MS);
    if (h.home(addr) == 0) h.waitIdle(addr, UNIT_HOME_TIMEOUT_MS);
    if (!h.readBootInfo(addr, info)) {
      return failed(BootUpdateStep::Stage1, MaintReason::BootUnitLost);
    }
    if (info.state != BOOT_STATE_PAGE7_INSTALLED) {
      return failed(BootUpdateStep::Stage1, MaintReason::BootVerifyFailed);
    }
    h.note(BootUpdateStep::Stage1, MaintReason::None, info);
  }

  if (plan.needStage2) {
    if (!plan.needStage1 && !h.isHomed(addr)) {
      // Resuming a unit that already carries page 7: nothing above homed it,
      // and an unhomed unit refuses the stage (#516). One that is homed is
      // left where it stands — a home is a full turn of the drum, and stage 2
      // does not move it.
      moved = true;
      if (h.home(addr) == 0) h.waitIdle(addr, UNIT_HOME_TIMEOUT_MS);
      if (!h.readBootInfo(addr, info)) {
        return failed(BootUpdateStep::Stage2, MaintReason::BootUnitLost);
      }
    }
    const uint8_t resultBeforeSend = info.lastResult;
    if (h.sendStage(addr, 2) != 0) {
      return finish(BootUpdateStep::Stage2, MaintOutcome::WireFail,
                    MaintReason::None);
    }
    h.pause(BOOT_UPDATE_STAGE2_SETTLE_MS);
    BootPollVerdict verdict = BOOT_POLL_WAIT;
    bool anyRead = false;
    uint32_t pollStart = h.nowMs();
    while ((uint32_t)(h.nowMs() - pollStart) < BOOT_UPDATE_STAGE2_POLL_MS) {
      if (h.readBootInfo(addr, info)) {
        anyRead = true;
        verdict = bootStage2Poll(info, resultBeforeSend);
        if (verdict != BOOT_POLL_WAIT) break;
      }
      h.pause(100);
    }
    if (verdict != BOOT_POLL_DONE) {
      // No report at all is a lost unit, not a failed verify; otherwise the
      // unit's own result names the cause.
      return failed(BootUpdateStep::Stage2,
                    anyRead ? maintReasonForBootFailure(
                                  bootResultFailure(info.lastResult),
                                  MaintReason::BootVerifyFailed)
                            : MaintReason::BootUnitLost);
    }
  }

  h.holdProbes();
  return finish(BootUpdateStep::Done, MaintOutcome::Ok, MaintReason::None);
}
