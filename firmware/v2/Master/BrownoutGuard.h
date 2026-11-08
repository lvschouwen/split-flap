// Brownout-loop guard glue (#504, rules in BrownoutGuardPolicy.h): the NVS
// record of brownouts in a row, the display hold it latches and the clear.
#pragma once

#include <Arduino.h>

#include "BrownoutGuardPolicy.h"

// setup(), single-threaded, before any task: counts the reset this boot
// started from and latches the hold at the limit.
void brownoutGuardBoot();

// Once the log is up: the streak, and the hold when there is one.
void brownoutGuardLogReport();

// netTask each loop: ends the streak once this boot has lived the window,
// and carries out a staged clear (NVS is written here only).
void brownoutGuardTick();

// Any task: whether this boot is held, and the brownouts in a row it counted.
bool brownoutGuardHeld();
uint8_t brownoutGuardStreak();

// Any task (the API action): asks netTask to clear the hold. False when
// nothing is held.
bool brownoutGuardRequestClear();

// displayTask only, before its start-up homes: returns at once unless this
// boot is held; held, it waits (feeding the task watchdog) until the hold is
// cleared, and caps the units moving at once for the rest of the boot.
void brownoutGuardHoldDisplay();

// GET /api/v2/board/<master> "brownoutGuard".
String brownoutGuardJson();
