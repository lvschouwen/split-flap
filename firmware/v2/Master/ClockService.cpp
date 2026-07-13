#include "ClockService.h"

#include <Arduino.h>
#include <esp_sntp.h>
#include <esp_timer.h>

#include <atomic>

#include "HelpersSerialHandling.h"

// v1 timezoneServer parity: compile-time const, no settings knob.
static const char* NTP_SERVER = "pool.ntp.org";

// NTP sync age for the System tab (#245). The notification callback runs in
// the SNTP/LWIP task; any task reads the age accessor — std::atomic per the
// codebase's cross-task idiom. 0 = never synced (a sync landing in
// boot-second zero still records 1).
static std::atomic<uint32_t> lastSyncUptimeS{0};

static void onSntpSync(struct timeval*) {
  uint32_t nowS = (uint32_t)(esp_timer_get_time() / 1000000LL);
  lastSyncUptimeS.store(nowS > 0 ? nowS : 1);
}

int32_t clockNtpAgeS() {
  uint32_t last = lastSyncUptimeS.load();
  if (last == 0) return -1;
  uint32_t nowS = (uint32_t)(esp_timer_get_time() / 1000000LL);
  return (int32_t)(nowS - last);
}

void clockServiceApplyTz(const MasterSettings& settings) {
  // Register before (re)starting SNTP so the very first sync is recorded;
  // idempotent across repeated TZ applies.
  sntp_set_time_sync_notification_cb(onSntpSync);
  // loadSettings sanitizes the timezone; the fallback mirrors v1's chain
  // ending in "UTC0" all the same.
  const char* tz = settings.timezonePosix.length() > 0
                       ? settings.timezonePosix.c_str()
                       : "UTC0";
  configTzTime(tz, NTP_SERVER);
  SerialPrintln("NTP sync (tz=" + String(tz) + ", server=" + NTP_SERVER + ")");
}
