// Boot guard glue (#281, rules in BootGuardPolicy.h): the RTC_NOINIT count of
// crashes in a row, the switch to the rescue image at the limit, and the
// report of both.
#pragma once

#include <Arduino.h>

#include "BootGuardPolicy.h"

// Top of setup(), before anything that can crash: counts the reset this boot
// started from. At the limit, and only when the rescue image verifies, it
// erases otadata, notes the trip in NVS and restarts — it does not return.
// With no rescue image to start it logs that and lets the boot go on.
void bootGuardBoot();

// netTask each loop: forgives the count once this boot has run long enough.
void bootGuardTick();

// /settings "bootGuard": the count now, the limit, how often the guard has
// sent this board to its rescue image and what the last time was about.
String bootGuardJson();
