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

// Same masked-XOR rationale as the diag/odometer replies: old-firmware
// padding garbage must not validate.
#define SELFTEST_REPLY_LEN            9
#define SELFTEST_REPLY_CHECKSUM_MASK  0x5C

struct SelfTestResult {
  uint8_t state;
  uint16_t stepsPerRev;
  uint16_t hallWindowSteps;
  uint16_t revTimeMs;
};

// 9 bytes: state, then the three uint16 measurements LE, reserved 0, XOR
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
  buf[7] = 0;  // reserved
  uint8_t x = 0;
  for (uint8_t i = 0; i < SELFTEST_REPLY_LEN - 1; i++) x ^= buf[i];
  buf[8] = (uint8_t)(x ^ SELFTEST_REPLY_CHECKSUM_MASK);
}
