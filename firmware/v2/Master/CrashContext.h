// Crash context glue (#504, policy in CrashContextPolicy.h): the RTC_NOINIT
// instance each task marks, and the report the boot after a crash serves.
#pragma once

#include <Arduino.h>

#include "CrashContextPolicy.h"

// Top of setup(), before any task runs: if this boot follows a crash and the
// RTC record is intact, keep it as the report; then re-arm.
void crashCtxBoot();

// Once the log is up (after flashLogInit()): the report, when there is one.
void crashCtxLogReport();

// Any task, its own slot only. Cheap: a few stores to RTC memory.
void crashCtxMark(int slot, uint8_t act, uint8_t arg = 0);

// netTask each loop: advances the clock "age" is measured against.
void crashCtxHeartbeat();

// /settings "crashContext": {} when the last reset was not a crash.
String crashCtxReportJson();
