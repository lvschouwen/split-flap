// Low-heap watch glue (#504, rules in HeapWatchPolicy.h): the internal heap
// sampled once a second on netTask, the RTC_NOINIT restart record, the
// graceful restart with its cause.
#pragma once

#include <Arduino.h>

#include "HeapWatchPolicy.h"

// setup(), single-threaded: validates the record a restart left behind.
void heapWatchBoot();

// Once the log is up: how many low-heap restarts since power-on, if any.
void heapWatchLogReport();

// netTask each loop, self-throttled to once a second; never blocks.
void heapWatchTick();

// GET /api/v2/board/<master> "heapWatch".
String heapWatchJson();
