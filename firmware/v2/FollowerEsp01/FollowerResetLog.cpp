// FollowerResetLog.cpp — RTC-memory and SDK glue for the reset history (#503).
// Ring layout + contract in FollowerResetLog.h.

#include "FollowerResetLog.h"

#include <Arduino.h>
#include <user_interface.h>  // struct rst_info

#include "FollowerConfig.h"
#include "FollowerEvents.h"
#include "wall_link.pb.h"  // RowEventCode

static FollowerResetLogBlob resetLog;

void resetLogBootInit() {
  ESP.rtcUserMemoryRead(FOLLOWER_RESETLOG_RTC_OFFSET, (uint32_t*)&resetLog,
                        sizeof(resetLog));
  const rst_info* info = ESP.getResetInfoPtr();
  followerResetLogPush(resetLog, (uint8_t)info->reason,
                       (uint8_t)info->exccause, info->epc1, info->excvaddr);
  ESP.rtcUserMemoryWrite(FOLLOWER_RESETLOG_RTC_OFFSET, (uint32_t*)&resetLog,
                         sizeof(resetLog));
  // The master's event record gets this start and its cause (#570).
  followerEvents().put(wl_RowEventCode_ROW_EVT_STARTED, 0, resetLog.e[0].reasonCause,
                       resetLog.boots, millis() / 1000);
  SerialPrint(F("Reset: "));
  SerialPrintln(ESP.getResetInfo());
  // The restarts before this one: the log is where the history is read.
  const int count = followerResetLogCount(resetLog);
  for (int i = 1; i < count; i++) {
    char entry[32];
    followerResetEntryFormat(resetLog.e[i], entry, sizeof(entry));
    SerialPrint(F("Reset before that: "));
    SerialPrintln(entry);
  }
}
