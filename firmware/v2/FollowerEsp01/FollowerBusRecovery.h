#pragma once
// Pure row-wide I2C bus-death detector + recovery backoff (#488). The
// ESP8266 core's bit-banged Twi::write_start() refuses to START while SDA
// reads low and never clocks the bus itself, so a Nano left mid-byte by a
// glitch holds SDA forever and every later transaction fails — the row
// freezes until a power cycle. Wire.status() clocks SDA free; this header
// decides WHEN to call it. Glue (Wire calls, logging, reshow) lives in
// FollowerBus.cpp; follower-only — the S3 master rebuilds its IDF driver
// per failed read instead (UnitBus.cpp).
//
// Signal: the round-robin heartbeat's GET_STATUS liveness of drivable units
// only — never the bulk busPollHealth() sweeps, which would fold six reads
// in milliseconds and defeat the debounce below.
// One unit failing while others answer is a unit fault, never a bus fault:
// the run resets on any good read, and a trip needs failures from at least
// two distinct units (one on a 1-wide row, where they are indistinguishable
// and a rate-limited recovery is harmless).

#include <stdint.h>

// Consecutive failed liveness reads (any drivable unit) before the bus is
// declared dead. At the 3 s heartbeat tick: ~18 s from death to detection.
#define BUS_DEAD_MIN_FAILS 6
// First retry waits BASE after the first attempt, doubling per attempt up to
// MAX, so a bus a slave holds for good costs one clock-out per 5 min.
#define BUS_RECOVERY_BACKOFF_BASE_MS 5000UL
#define BUS_RECOVERY_BACKOFF_MAX_MS 300000UL

enum class BusRecoveryEvent : uint8_t { None, WentDead, Recovered };

struct BusRecoveryState {
  uint32_t failMask = 0;         // unit indexes failed since the last good read
  uint8_t consecutiveFails = 0;  // saturating
  bool dead = false;
  uint32_t deadSinceMs = 0;
  uint32_t nextAttemptMs = 0;
  uint8_t attemptsThisEpisode = 0;  // saturating; drives the backoff
  uint32_t attempts = 0;            // since boot
  uint32_t episodes = 0;            // since boot
  uint32_t recovered = 0;           // episodes closed by a good read
  uint32_t lastDeadMs = 0;          // duration of the last closed episode
  int8_t lastStatus = -1;           // Wire.status() of the last attempt; -1 never
};

inline uint32_t busRecoveryBackoffMs(uint8_t attemptsSoFar) {
  uint32_t ms = BUS_RECOVERY_BACKOFF_BASE_MS;
  for (uint8_t i = 0; i < attemptsSoFar && ms < BUS_RECOVERY_BACKOFF_MAX_MS;
       i++) {
    ms *= 2;
  }
  return ms > BUS_RECOVERY_BACKOFF_MAX_MS ? BUS_RECOVERY_BACKOFF_MAX_MS : ms;
}

inline uint8_t busRecoveryPopcount(uint32_t v) {
  uint8_t n = 0;
  for (; v; v &= v - 1) n++;
  return n;
}

// Folds one liveness read. rowWidth = probed row width (not the drivable
// count: a wide row down to one drivable unit must not read that unit's own
// fault as a dead bus); <= 0 means there is no row to judge.
inline BusRecoveryEvent busRecoveryObserve(BusRecoveryState& s, int unitIndex,
                                           bool ok, uint32_t nowMs,
                                           int rowWidth) {
  if (unitIndex < 0 || unitIndex >= 32 || rowWidth <= 0) {
    return BusRecoveryEvent::None;
  }
  if (ok) {
    s.failMask = 0;
    s.consecutiveFails = 0;
    if (!s.dead) return BusRecoveryEvent::None;
    s.dead = false;
    s.attemptsThisEpisode = 0;
    s.recovered++;
    s.lastDeadMs = nowMs - s.deadSinceMs;
    return BusRecoveryEvent::Recovered;
  }
  s.failMask |= (1UL << unitIndex);
  if (s.consecutiveFails < 0xFF) s.consecutiveFails++;
  if (s.dead) return BusRecoveryEvent::None;
  const uint8_t distinctNeeded = rowWidth >= 2 ? 2 : 1;
  if (s.consecutiveFails < BUS_DEAD_MIN_FAILS ||
      busRecoveryPopcount(s.failMask) < distinctNeeded) {
    return BusRecoveryEvent::None;
  }
  s.dead = true;
  s.deadSinceMs = nowMs;
  s.nextAttemptMs = nowMs;
  s.episodes++;
  return BusRecoveryEvent::WentDead;
}

// A row whose probe found no unit at all is a dead bus too: a follower row is
// never legitimately empty, and a slave-held SDA NACKs every probe address, so
// the liveness path above has nothing to poll. Opens an episode (idempotent);
// a re-probe that finds units closes it through busRecoveryObserve(ok=true).
inline void busRecoveryNoteEmptyRow(BusRecoveryState& s, uint32_t nowMs) {
  if (s.dead) return;
  s.dead = true;
  s.deadSinceMs = nowMs;
  s.nextAttemptMs = nowMs;
  s.episodes++;
}

inline bool busRecoveryDue(const BusRecoveryState& s, uint32_t nowMs) {
  return s.dead && (int32_t)(nowMs - s.nextAttemptMs) >= 0;
}

// Records a clock-out attempt and its Wire.status() verdict.
inline void busRecoveryNoteAttempt(BusRecoveryState& s, uint32_t nowMs,
                                   uint8_t wireStatus) {
  s.nextAttemptMs = nowMs + busRecoveryBackoffMs(s.attemptsThisEpisode);
  if (s.attemptsThisEpisode < 0xFF) s.attemptsThisEpisode++;
  s.attempts++;
  s.lastStatus = (int8_t)wireStatus;
}
