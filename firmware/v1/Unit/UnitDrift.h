#pragma once
// Pure drift-detection logic for the unit's drum-position tracking
// (#263/#264) — position accumulation mod one revolution, hall-edge
// deviation folding, the event/threshold policy, the hall-corrected
// physical-letter estimate and the SFP_CMD_GET_DIAG wire encode. Natively
// tested by test_drift. The hall sampling, stepper and ISR glue live in the
// .ino files.
//
// Model: drumPosition counts stepper steps past the hall marker's entering
// (1->0) edge, mod stepsPerRev. Every drum move advances it through the
// stepCounted() funnel; every observed edge (calibrate's search AND the
// per-step watch in stepFlaps) resyncs it to 0 — the edge IS physical
// truth. A resync that had to move the counter further than the threshold
// is a measured drift event: the belief (displayedLetter) ran away from
// the drum. Letter 0 sits at edge + calOffset, so the physical letter is
// derived from (drumPosition - calOffset) mod rev, rounded to the nearest
// flap so a step or two of accumulator error never flips to a neighbor.

#include <stdint.h>

// ~half a flap (2038/45/2). Wider than the hall edge's mechanical
// repeatability, narrower than a real one-flap slip. One-line tunable if
// the bench shows false positives.
#define DRIFT_THRESHOLD_STEPS 23

#define DRIFT_LETTER_UNKNOWN 0xFF

// GET_DIAG reply flags (byte 1).
#define DRIFT_FLAG_PENDING        (1 << 0)  // auto re-home armed
#define DRIFT_FLAG_POSITION_KNOWN (1 << 1)  // synced to an edge at least once

// SFP_CMD_GET_DIAG wire reply: physical letter, flags, event count, last
// drift magnitude (int8 steps, clamped), reserved, XOR-of-payload ^ mask.
// Same masked-checksum rationale as the odometer reply (#231): an old unit
// answers the unknown opcode with 1-byte status + bus padding — all-0x00 /
// all-0xFF garbage must not validate (#106 class).
#define DRIFT_REPLY_LEN            6
#define DRIFT_REPLY_CHECKSUM_MASK  0xB7

struct DriftState {
  uint16_t drumPosition;   // steps past the hall edge, 0..stepsPerRev-1
  bool positionKnown;      // false until the first edge sync
  uint8_t driftEvents;     // saturating, since boot
  int16_t lastDriftSteps;  // signed magnitude of the last counted event
  bool driftPending;       // set by the caller when an event wants a re-home
};

inline void driftReset(DriftState& s) {
  s.drumPosition = 0;
  s.positionKnown = false;
  s.driftEvents = 0;
  s.lastDriftSteps = 0;
  s.driftPending = false;
}

// Advance the position by a (possibly negative — jog) move, mod one rev.
inline void driftAdvance(DriftState& s, long steps, uint16_t stepsPerRev) {
  long pos = (long)s.drumPosition + steps;
  pos %= (long)stepsPerRev;
  if (pos < 0) pos += stepsPerRev;
  s.drumPosition = (uint16_t)pos;
}

// Fold a raw position (where the counter stood when the edge fired) into a
// signed deviation in [-(rev-1)/2 .. rev/2]: small positive = belief ran
// ahead of the drum (missed steps), small negative = drum ran ahead of
// belief (external nudge).
inline int16_t driftFoldDeviation(uint16_t position, uint16_t stepsPerRev) {
  if (position <= stepsPerRev / 2) return (int16_t)position;
  return (int16_t)((int32_t)position - (int32_t)stepsPerRev);
}

// Marks the position as freshly synced to the edge (calibrate's marker
// found). Also the test seam for "a sync happened earlier".
inline void driftMarkSynced(DriftState& s) {
  s.drumPosition = 0;
  s.positionKnown = true;
}

// One observed hall entering edge: measure the deviation against the
// expectation (position back at 0), count an event when it exceeds the
// threshold, and resync to the edge either way. Returns true on a counted
// event so the caller can arm its re-home. The first-ever edge (position
// never synced) only syncs — there is no expectation to deviate from, and
// boot homing must not log a phantom event.
inline bool driftObserveEdge(DriftState& s, uint16_t stepsPerRev,
                             uint16_t thresholdSteps) {
  bool hadExpectation = s.positionKnown;
  int16_t deviation = driftFoldDeviation(s.drumPosition, stepsPerRev);
  driftMarkSynced(s);
  if (!hadExpectation) return false;
  int16_t magnitude = deviation < 0 ? (int16_t)-deviation : deviation;
  if (magnitude <= (int16_t)thresholdSteps) return false;
  if (s.driftEvents < 0xFF) s.driftEvents++;
  s.lastDriftSteps = deviation;
  return true;
}

// Hall-corrected physical letter estimate: nearest flap to the drum's
// tracked position, letter 0 at edge + calOffset. DRIFT_LETTER_UNKNOWN
// until the first sync. uint32 arithmetic: worst product is
// (rev-1) * flaps + rev/2 ~= 92k.
inline uint8_t driftPhysicalLetter(const DriftState& s, int16_t calOffset,
                                   uint16_t stepsPerRev, uint8_t flapAmount) {
  if (!s.positionKnown) return DRIFT_LETTER_UNKNOWN;
  long fromZero = (long)s.drumPosition - (long)calOffset;
  fromZero %= (long)stepsPerRev;
  if (fromZero < 0) fromZero += stepsPerRev;
  uint32_t letter =
      ((uint32_t)fromZero * flapAmount + stepsPerRev / 2) / stepsPerRev;
  return (uint8_t)(letter % flapAmount);
}

inline void driftEncodeDiagReply(uint8_t physicalLetter, uint8_t flags,
                                 uint8_t driftEvents, int16_t lastDriftSteps,
                                 uint8_t buf[DRIFT_REPLY_LEN]) {
  int16_t clamped = lastDriftSteps;
  if (clamped > 127) clamped = 127;
  if (clamped < -127) clamped = -127;
  buf[0] = physicalLetter;
  buf[1] = flags;
  buf[2] = driftEvents;
  buf[3] = (uint8_t)(int8_t)clamped;
  buf[4] = 0;  // reserved
  buf[5] = (uint8_t)((buf[0] ^ buf[1] ^ buf[2] ^ buf[3] ^ buf[4]) ^
                     DRIFT_REPLY_CHECKSUM_MASK);
}
