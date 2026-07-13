#pragma once
// SystemStats.h — the System tab's sampler service (#245). netTask ticks
// the sampler (self-throttled to SYSTEM_STATS_INTERVAL_S); the ring lives
// behind a mutex because GET /system/stats renders from the async_tcp task.
// Pure ring/JSON logic in SystemStatsPolicy.h (natively tested).

#include <stddef.h>

// setup(), before tasksInit() starts netTask.
void systemStatsInit();

// netTask only: samples vitals every SYSTEM_STATS_INTERVAL_S. Radio reads
// (WiFi.RSSI) belong to netTask; I2C/MQTT/NTP counters are cross-task-safe
// 32-bit reads (see their accessors).
void systemStatsTick();

// Any task (async web handler): renders current + history JSON into buf.
// Returns the would-be length like snprintf; callers reject >= cap.
size_t systemStatsJson(char* buf, size_t cap);
