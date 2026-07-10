#pragma once
// ClockService.h — timezone + SNTP application (#192), the impure side of
// the clock slice (the ticker brain lives in ClockPolicy.h).

#include "Settings.h"

// v1 applyTimezoneAndNtp() parity: configTzTime(settings TZ, pool.ntp.org).
// Call sites: setup() after loadSettings (TZ correct from first boot line;
// SNTP starts and syncs once a netif exists), WifiService startOnline()
// (v1 applies after join — the re-init also kicks an immediate sync), and
// the settings drain when a POST changed the timezone (#48: no reboot).
// Cross-task safety: newlib's TZ globals are lock-guarded on ESP-IDF, so
// applying from netTask while clockTask runs localtime_r is safe.
void clockServiceApplyTz(const MasterSettings& settings);
