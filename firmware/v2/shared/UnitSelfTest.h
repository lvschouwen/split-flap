#pragma once
// Pure self-test result state + SFP_CMD_GET_SELF_TEST wire encode (#265),
// natively tested by test_drift. The diagnostic revolution itself (hall
// sync, measured rev, timing) is motion glue in UnitMotion.ino — bench
// tier.
//
// The test measures three mechanical health numbers on demand:
//   stepsPerRev     actual steps between two hall entering edges vs the
//                   nominal 2038 — a growing gap means missed steps under
//                   no-load homing speed (tight gear train, weak motor)
//   hallWindowSteps how many steps the hall reads 0 across the revolution —
//                   a shrinking window means a weakening magnet or a
//                   misaligned KY-003
//   revTimeMs       wall time of the measured revolution — sanity check of
//                   the commanded speed (stalls show up here)

#include <stdint.h>

#define SELFTEST_STATE_NEVER   0
#define SELFTEST_STATE_RUNNING 1
#define SELFTEST_STATE_OK      2
#define SELFTEST_STATE_FAILED  3

// Which of runSelfTest()'s three failure modes fired (#404). They used to
// collapse into a bare SELFTEST_STATE_FAILED, and on a physical wall they mean
// completely different repairs:
//
//   HALL_STUCK   phase 1a could not step OUT of the hall window within
//                3x STEPS — a magnet sitting against the sensor, or a sensor
//                stuck low.
//   HALL_NEVER   phase 1b never saw the hall go low within 3x STEPS — a magnet
//                that has fallen off, a dead KY-003, or broken wiring.
//   REV_INCOMPLETE
//                phase 2 exceeded 2x STEPS without the entering edge coming
//                back — a drum that slips or binds part-way round.
//
// Address 15 was diagnosed by inference instead: run the test four times,
// watch the odometer advance ~3 revolutions per attempt, match that against
// the 3x STEPS guard, then compare against a healthy control. Several minutes
// of remote work to reach a conclusion the unit had at the moment it gave up.
#define SELFTEST_REASON_NONE           0
#define SELFTEST_REASON_HALL_STUCK     1
#define SELFTEST_REASON_HALL_NEVER     2
#define SELFTEST_REASON_REV_INCOMPLETE 3

// Same masked-XOR rationale as the diag/odometer replies: old-firmware
// padding garbage must not validate. The reason rides byte 7, which was
// reserved — so the reply length is unchanged.
#define SELFTEST_REPLY_LEN            9
#define SELFTEST_REPLY_CHECKSUM_MASK  0x5C

// Default-initialised like the other packet structs (UnitExtDiag, UnitVitals,
// UnitLifetime): a stack-declared result must never encode an indeterminate
// reason onto the wire, where it would read as a real repair instruction.
struct SelfTestResult {
  uint8_t state = SELFTEST_STATE_NEVER;
  uint16_t stepsPerRev = 0;
  uint16_t hallWindowSteps = 0;
  uint16_t revTimeMs = 0;
  uint8_t reason = SELFTEST_REASON_NONE;
};

// 9 bytes: state, the three uint16 measurements LE, the failure reason, XOR
// checksum ^ mask.
inline void selfTestEncodeReply(const SelfTestResult& r,
                                uint8_t buf[SELFTEST_REPLY_LEN]) {
  buf[0] = r.state;
  buf[1] = (uint8_t)(r.stepsPerRev & 0xFF);
  buf[2] = (uint8_t)((r.stepsPerRev >> 8) & 0xFF);
  buf[3] = (uint8_t)(r.hallWindowSteps & 0xFF);
  buf[4] = (uint8_t)((r.hallWindowSteps >> 8) & 0xFF);
  buf[5] = (uint8_t)(r.revTimeMs & 0xFF);
  buf[6] = (uint8_t)((r.revTimeMs >> 8) & 0xFF);
  buf[7] = r.reason;
  uint8_t x = 0;
  for (uint8_t i = 0; i < SELFTEST_REPLY_LEN - 1; i++) x ^= buf[i];
  buf[8] = (uint8_t)(x ^ SELFTEST_REPLY_CHECKSUM_MASK);
}

inline const char* selfTestReasonName(uint8_t reason) {
  switch (reason) {
    case SELFTEST_REASON_HALL_STUCK:     return "hall-stuck";
    case SELFTEST_REASON_HALL_NEVER:     return "hall-never";
    case SELFTEST_REASON_REV_INCOMPLETE: return "rev-incomplete";
    default:                             return "none";
  }
}
