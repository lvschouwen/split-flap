// FollowerResetLog.cpp — RTC-memory and SDK glue for the reset history (#503).
// Ring layout + contract in FollowerResetLog.h.

#include "FollowerResetLog.h"

#include <Arduino.h>
#include <user_interface.h>  // struct rst_info

#include "FollowerConfig.h"

static FollowerResetLogBlob resetLog;

void resetLogBootInit() {
  ESP.rtcUserMemoryRead(FOLLOWER_RESETLOG_RTC_OFFSET, (uint32_t*)&resetLog,
                        sizeof(resetLog));
  const rst_info* info = ESP.getResetInfoPtr();
  followerResetLogPush(resetLog, (uint8_t)info->reason,
                       (uint8_t)info->exccause, info->epc1, info->excvaddr);
  ESP.rtcUserMemoryWrite(FOLLOWER_RESETLOG_RTC_OFFSET, (uint32_t*)&resetLog,
                         sizeof(resetLog));
  SerialPrint(F("Reset: "));
  SerialPrintln(ESP.getResetInfo());
}

const FollowerResetLogBlob& resetLogGet() { return resetLog; }
