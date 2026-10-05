#pragma once
// FollowerPrefs.h — glue for the operator preferences record (#513). The
// record and its defaults are pure in FollowerSettings.h; this owns the
// EEPROM read at boot and the write, which is staged by the web handler and
// performed by loop() (a flash write does not belong in the async context).

#include "FollowerSettings.h"

void prefsInit();                       // after clusterInit()'s EEPROM.begin
bool prefsReflashOnBoot();              // the persisted value
void prefsStageReflashOnBoot(bool on);  // handler context
FollowerFallback prefsFallback();       // the persisted value
void prefsStageFallback(FollowerFallback fallback);
// loop(): persist a staged change. Commits are spaced at least
// PREFS_COMMIT_MIN_GAP_MS apart (each one erases the EEPROM sector, which also
// holds the cluster membership); `force` skips the spacing for the write that
// must land before a restart.
void prefsLoopTick(bool force = false);
