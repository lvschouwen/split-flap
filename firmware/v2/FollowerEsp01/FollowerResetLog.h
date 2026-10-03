#pragma once
// FollowerResetLog.h — why this board restarted, and the restarts before it
// (#503). The SDK's reset info describes only the LATEST reset, so a crash
// followed by an operator reboot or a leader re-push leaves no trace. Each
// boot pushes the SDK's record into a small ring in RTC user memory, which
// survives crash and soft resets but not a power cycle — after one the ring
// starts over, and its single entry says so. Pure logic, natively tested by
// test_follower_resetlog; the RTC and SDK glue is in FollowerResetLog.cpp.

#include <stdint.h>
#include <stdio.h>

// Word-block offset into RTC user memory. FollowerRescue.h owns 3 words at
// 32; this record is 15 words.
#define FOLLOWER_RESETLOG_RTC_OFFSET 40
#define FOLLOWER_RESETLOG_ENTRIES 4
#define FOLLOWER_RESETLOG_MAGIC 0x474C5352UL  // "RSLG" LE
#define FOLLOWER_RESETLOG_CHECK_MASK 0x5A5A5A5AUL

struct FollowerResetEntry {
  uint32_t reasonCause = 0;  // SDK reset reason | exception cause << 8
  uint32_t epc1 = 0;         // program counter at the fault (exception/wdt)
  uint32_t excvaddr = 0;     // faulting address, for load/store exceptions
};

struct FollowerResetLogBlob {
  uint32_t magic = 0;
  uint32_t boots = 0;  // boots recorded since the last power cycle
  FollowerResetEntry e[FOLLOWER_RESETLOG_ENTRIES];  // [0] = this boot
  uint32_t check = 0;
};

inline uint32_t followerResetLogCheck(const FollowerResetLogBlob& b) {
  uint32_t x = b.magic ^ b.boots ^ FOLLOWER_RESETLOG_CHECK_MASK;
  for (int i = 0; i < FOLLOWER_RESETLOG_ENTRIES; i++) {
    x ^= b.e[i].reasonCause ^ b.e[i].epc1 ^ b.e[i].excvaddr;
  }
  return x;
}

inline bool followerResetLogValid(const FollowerResetLogBlob& b) {
  return b.magic == FOLLOWER_RESETLOG_MAGIC && b.check == followerResetLogCheck(b);
}

// Record this boot. Garbage or unset RTC memory (power cycle, first run of
// this firmware) starts a fresh ring instead of shifting the garbage along.
inline void followerResetLogPush(FollowerResetLogBlob& b, uint8_t reason,
                                 uint8_t exccause, uint32_t epc1,
                                 uint32_t excvaddr) {
  if (!followerResetLogValid(b)) b = FollowerResetLogBlob{};
  for (int i = FOLLOWER_RESETLOG_ENTRIES - 1; i > 0; i--) b.e[i] = b.e[i - 1];
  b.e[0].reasonCause = (uint32_t)reason | ((uint32_t)exccause << 8);
  b.e[0].epc1 = epc1;
  b.e[0].excvaddr = excvaddr;
  if (b.boots < 0xFFFFFFFFUL) b.boots++;
  b.magic = FOLLOWER_RESETLOG_MAGIC;
  b.check = followerResetLogCheck(b);
}

inline int followerResetLogCount(const FollowerResetLogBlob& b) {
  if (!followerResetLogValid(b)) return 0;
  return b.boots < FOLLOWER_RESETLOG_ENTRIES ? (int)b.boots
                                             : FOLLOWER_RESETLOG_ENTRIES;
}

// One entry as "<reason>:<exccause>:<epc1 hex>:<excvaddr hex>", the form
// /cluster/health serves (newest first). Kept terse: the reply is built on a
// board with a few KB of free heap.
inline int followerResetEntryFormat(const FollowerResetEntry& e, char* buf,
                                    size_t cap) {
  return snprintf(buf, cap, "%u:%u:%08lx:%08lx",
                  (unsigned)(e.reasonCause & 0xFF),
                  (unsigned)((e.reasonCause >> 8) & 0xFF),
                  (unsigned long)e.epc1, (unsigned long)e.excvaddr);
}

#ifdef ARDUINO
// --- glue (FollowerResetLog.cpp, ESP.rtcUserMemory + SDK reset info) -----------
void resetLogBootInit();  // record + log this boot; early in setup()
const FollowerResetLogBlob& resetLogGet();
#endif
