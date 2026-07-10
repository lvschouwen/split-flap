#include "ClockService.h"

#include <Arduino.h>

#include "HelpersSerialHandling.h"

// v1 timezoneServer parity: compile-time const, no settings knob.
static const char* NTP_SERVER = "pool.ntp.org";

void clockServiceApplyTz(const MasterSettings& settings) {
  // loadSettings sanitizes the timezone; the fallback mirrors v1's chain
  // ending in "UTC0" all the same.
  const char* tz = settings.timezonePosix.length() > 0
                       ? settings.timezonePosix.c_str()
                       : "UTC0";
  configTzTime(tz, NTP_SERVER);
  SerialPrintln("NTP sync (tz=" + String(tz) + ", server=" + NTP_SERVER + ")");
}
