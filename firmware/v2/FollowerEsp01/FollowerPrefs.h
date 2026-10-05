#pragma once
// FollowerPrefs.h — glue for the operator preferences record (#513). The
// record and its defaults are pure in FollowerSettings.h; this owns the
// EEPROM read at boot and the write, which is staged when the master's
// Config arrives and performed by loop().

#include "FollowerSettings.h"

void prefsInit();                       // after clusterInit()'s EEPROM.begin
bool prefsReflashOnBoot();              // the persisted value
void prefsStageReflashOnBoot(bool on);
FollowerFallback prefsFallback();       // the persisted value
void prefsStageFallback(FollowerFallback fallback);
// loop(): persist a staged change. Commits are spaced at least
// PREFS_COMMIT_MIN_GAP_MS apart (each one erases the EEPROM sector, which also
// holds the pairing); `force` skips the spacing for the write that
// must land before a restart.
void prefsLoopTick(bool force = false);
