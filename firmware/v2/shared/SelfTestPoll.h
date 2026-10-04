#pragma once
// SelfTestPoll.h — how a row master waits for a unit's self-test (#265) and
// decides what its replies mean. Pure: each tree reads the unit and feeds the
// reading in, on its own schedule (the S3 blocks displayTask, the ESP-01 polls
// from loop()). Natively tested by test_self_test_poll.
//
// START_SELF_TEST only sets a flag in the unit's TWI interrupt; its loop picks
// the flag up after whatever move is in flight. Until then GET_SELF_TEST still
// answers with the PREVIOUS test's terminal result, so a terminal reading is
// accepted only once it is provably this test's: RUNNING was seen, or the
// reading differs from the first one read after the start.

#include <stdint.h>

#include "MaintenancePolicy.h"     // SelfTestSlot, SelfTestOutcome
#include "UnitProtocolHelpers.h"  // UnitSelfTestReading

// The diagnostic is ~2 revolutions at homing speed (~12-15 s) but can queue
// behind a slow in-flight move, so the window is generous — and it re-arms
// once RUNNING is first observed, so the worst-case wait is ~2x this.
#define SELF_TEST_TIMEOUT_MS 45000UL
#define SELF_TEST_POLL_MS 500UL
// This many unreadable replies with none ever valid = firmware predating the
// opcode.
#define SELF_TEST_UNSUPPORTED_POLLS 3

struct SelfTestPoll {
  bool sawRunning = false;
  bool everValid = false;
  UnitSelfTestReading baseline;  // first valid reading: the pre-test content
  uint8_t badPolls = 0;
  uint32_t windowStartMs = 0;
};

inline void selfTestPollBegin(SelfTestPoll& p, uint32_t nowMs) {
  p = SelfTestPoll{};
  p.windowStartMs = nowMs;
}

inline bool selfTestReadingDiffers(const UnitSelfTestReading& a,
                                   const UnitSelfTestReading& b) {
  return a.state != b.state || a.stepsPerRev != b.stepsPerRev ||
         a.hallWindowSteps != b.hallWindowSteps || a.revTimeMs != b.revTimeMs ||
         a.reason != b.reason;
}

// Folds one poll in. Returns Pending while the test is undecided; any other
// value is final and, for Ok / UnitFailed, `slot` carries the measurements —
// a failure keeps whatever the unit measured before giving up (#404).
inline SelfTestOutcome selfTestPollObserve(SelfTestPoll& p, bool readOk,
                                           const UnitSelfTestReading& r,
                                           uint32_t nowMs, SelfTestSlot& slot) {
  bool timedOut = (uint32_t)(nowMs - p.windowStartMs) >= SELF_TEST_TIMEOUT_MS;
  if (!readOk) {
    if (!p.everValid && ++p.badPolls >= SELF_TEST_UNSUPPORTED_POLLS) {
      return SelfTestOutcome::Unsupported;
    }
    return timedOut ? SelfTestOutcome::Timeout : SelfTestOutcome::Pending;
  }
  if (!p.everValid) {
    p.everValid = true;
    p.baseline = r;
  }
  if (r.state == SELFTEST_STATE_RUNNING) {
    if (!p.sawRunning) {
      // The test provably started — time the unit spent finishing a prior
      // move must not eat the test's own budget.
      p.sawRunning = true;
      p.windowStartMs = nowMs;
      return SelfTestOutcome::Pending;
    }
    return timedOut ? SelfTestOutcome::Timeout : SelfTestOutcome::Pending;
  }
  bool fresh = p.sawRunning || selfTestReadingDiffers(r, p.baseline);
  if (r.state == SELFTEST_STATE_NEVER || !fresh) {
    return timedOut ? SelfTestOutcome::Timeout : SelfTestOutcome::Pending;
  }
  slot.stepsPerRev = r.stepsPerRev;
  slot.hallWindowSteps = r.hallWindowSteps;
  slot.revTimeMs = r.revTimeMs;
  slot.unitReason = r.reason;
  return r.state == SELFTEST_STATE_OK ? SelfTestOutcome::Ok
                                      : SelfTestOutcome::UnitFailed;
}
