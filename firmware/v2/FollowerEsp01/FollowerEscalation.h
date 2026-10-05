#pragma once
// FollowerEscalation.h — when the ESP-01 row gives up on healing in place and
// restarts itself (#503). A restart only cures faults on this board's side,
// but it is the one step left that needs no operator.
//
//   bus dead for 10 min, on a row that has had units answer this boot
//   largest free heap block below the floor for 5 min (logged after 1)
//
// Deliberately NOT here: WiFi down and a silent leader. Both are almost
// always outside this board (an access point, a leader without power), a
// restart cannot cure them, and it costs what still works — the row's own
// clock fallback, its synced time, the log ring — for a boot that then sits
// in the join/portal cycle. A row that never saw a unit this boot is not
// "dead" either: nothing is plugged in, or the fault survived the restart.
//
// At most one such restart per ESCALATION_MIN_INTERVAL_MIN of uptime. The
// uptime since the last one is carried in RTC user memory, which a power
// cycle clears — after one the board may escalate at once.
//
// Pure logic, natively tested (test_follower_escalation); the RTC, heap and
// restart glue is FollowerEscalation.cpp.

#include <stdint.h>

#define ESCALATION_BUS_DEAD_MS (10UL * 60UL * 1000UL)
// A handler asks for its block plus 1.5 KB before it allocates; below this
// the board answers 503 to almost everything and a throwing allocation is
// one request away.
#define ESCALATION_LOW_BLOCK_BYTES 4096U
#define ESCALATION_LOW_HEAP_LOG_MS (60UL * 1000UL)
#define ESCALATION_LOW_HEAP_MS (5UL * 60UL * 1000UL)
#define ESCALATION_MIN_INTERVAL_MIN (6U * 60U)

// Word-block offset into RTC user memory: FollowerRescue.h owns 32..34,
// FollowerResetLog.h 40..54; this record is 4 words.
#define FOLLOWER_ESCALATION_RTC_OFFSET 56
#define FOLLOWER_ESCALATION_MAGIC 0x31435345UL  // "ESC1" LE
#define FOLLOWER_ESCALATION_CHECK_MASK 0xA5C3A5C3UL

enum class EscalationCause : uint8_t {
  None = 0,
  BusDead,
  LowHeap,
};

inline const char* escalationCauseName(uint8_t c) {
  switch ((EscalationCause)c) {
    case EscalationCause::BusDead: return "bus-dead";
    case EscalationCause::LowHeap: return "low-heap";
    default:                       return "none";
  }
}

// --- what survives the restart -------------------------------------------------

struct EscalationRecord {
  uint32_t magic = 0;
  uint32_t minutesSince = 0;  // uptime since the last escalation, saturating
  uint32_t countAndCause = 0; // count << 8 | cause of the last one
  uint32_t check = 0;
};

inline uint32_t escalationRecordCheck(const EscalationRecord& r) {
  return r.magic ^ r.minutesSince ^ r.countAndCause ^
         FOLLOWER_ESCALATION_CHECK_MASK;
}

inline bool escalationRecordValid(const EscalationRecord& r) {
  return r.magic == FOLLOWER_ESCALATION_MAGIC &&
         r.check == escalationRecordCheck(r);
}

inline void escalationRecordSeal(EscalationRecord& r) {
  r.magic = FOLLOWER_ESCALATION_MAGIC;
  r.check = escalationRecordCheck(r);
}

inline uint32_t escalationCount(const EscalationRecord& r) {
  return escalationRecordValid(r) ? r.countAndCause >> 8 : 0;
}

inline uint8_t escalationLastCause(const EscalationRecord& r) {
  return escalationRecordValid(r) ? (uint8_t)(r.countAndCause & 0xFF) : 0;
}

// May this board restart itself now? No record (power cycle, first run) or a
// record with none taken yet: yes.
inline bool escalationAllowed(const EscalationRecord& r) {
  if (escalationCount(r) == 0) return true;
  return r.minutesSince >= ESCALATION_MIN_INTERVAL_MIN;
}

// One more minute of uptime.
inline void escalationRecordMinute(EscalationRecord& r) {
  if (!escalationRecordValid(r)) r = EscalationRecord{};
  if (r.minutesSince < 0xFFFFFFFFUL) r.minutesSince++;
  escalationRecordSeal(r);
}

inline void escalationRecordTaken(EscalationRecord& r, EscalationCause cause) {
  uint32_t count = escalationCount(r);
  if (count < 0x00FFFFFFUL) count++;
  r.minutesSince = 0;
  r.countAndCause = (count << 8) | (uint8_t)cause;
  escalationRecordSeal(r);
}

// --- the decision ---------------------------------------------------------------

struct EscalationInput {
  uint32_t nowMs = 0;
  bool busDead = false;          // the row-wide recovery has the bus as dead
  uint32_t busDeadSinceMs = 0;
  bool unitsSeenThisBoot = false;  // at least one unit has answered
  uint32_t largestFreeBlock = 0xFFFFFFFFUL;
};

struct EscalationState {
  bool lowHeap = false;
  uint32_t lowHeapSinceMs = 0;
  bool lowHeapLogged = false;
};

struct EscalationVerdict {
  EscalationCause cause = EscalationCause::None;
  bool logLowHeap = false;  // once per low-heap episode
};

// About once a second. Reports the first condition past its limit — low heap
// before a dead bus, since it is the one that takes the board down by itself.
// Whether a restart then happens is escalationAllowed()'s decision and the
// caller's (never on the way out, nor in the rescue beacon).
inline EscalationVerdict escalationStep(EscalationState& st,
                                        const EscalationInput& in) {
  EscalationVerdict v;
  if (in.largestFreeBlock >= ESCALATION_LOW_BLOCK_BYTES) {
    st.lowHeap = false;
    st.lowHeapLogged = false;
  } else if (!st.lowHeap) {
    st.lowHeap = true;
    st.lowHeapSinceMs = in.nowMs;
  }
  if (st.lowHeap && !st.lowHeapLogged &&
      in.nowMs - st.lowHeapSinceMs >= ESCALATION_LOW_HEAP_LOG_MS) {
    st.lowHeapLogged = true;
    v.logLowHeap = true;
  }

  if (st.lowHeap && in.nowMs - st.lowHeapSinceMs >= ESCALATION_LOW_HEAP_MS) {
    v.cause = EscalationCause::LowHeap;
  } else if (in.busDead && in.unitsSeenThisBoot &&
             in.nowMs - in.busDeadSinceMs >= ESCALATION_BUS_DEAD_MS) {
    v.cause = EscalationCause::BusDead;
  }
  return v;
}

// --- glue (FollowerEscalation.cpp) -----------------------------------------------

// setup(): load the record a restart left behind.
void escalationBootInit();
// loop(): one policy step a second, one minute of uptime a minute.
void escalationTick();
const EscalationRecord& escalationRecordGet();
