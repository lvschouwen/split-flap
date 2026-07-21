#pragma once
// TasksInternal.h — shared seams of the Tasks TU family (#352): Tasks.cpp
// (IPC primitives, task lifecycle, core-0 domain wrappers), DisplayTask.cpp
// (core-1 display domain), ClockTask.cpp (1 Hz mode ticker). Include from
// these .cpp files ONLY — the WebEndpointsInternal.h rule: everything here
// is family-internal; the public contract stays in Tasks.h.

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "Tasks.h"

// The display command queue (defined in Tasks.cpp). DisplayTask.cpp drains
// it (displayTaskMain + the reflash-job stale-drain); producers go through
// the public displayEnqueue().
extern QueueHandle_t displayQueue;

// Single-writer snapshot publish (displayTask only — the #187 contract).
void snapshotPublish(const DisplaySnapshot& next);

// The width-override value handed to displayApplyUnitFacts: -1 (force width
// 0) while headless (#331), else the #289 unit-count override (0 = auto).
int effectiveWidthOverride();

// True while the #289 unit-count override pins the width (boot log line).
bool tasksUnitCountOverridePinned();

// Task entry points started by tasksInit().
void displayTaskMain(void*);
void clockTaskMain(void*);
