#pragma once
// ESP-IDF Task Watchdog helpers (#314). Target-only glue — no pure logic, so
// no native test; verified on the bench (a hung subscribed task must reboot
// within ~30 s, and OTA/reflash must NOT false-reboot). Feed-inside, never
// unsubscribe: keeping the dog live during long ops is the whole point (a
// wedged I2C transaction must still trip it).
#include <esp_err.h>
#include <esp_task_wdt.h>

// Subscribe the CALLING task to the TWDT. Returns ESP_OK on success; the
// caller logs esp_err_to_name(err) otherwise. Never aborts.
inline esp_err_t wdtSubscribeSelf() { return esp_task_wdt_add(nullptr); }

// Feed the TWDT for the calling task. Call at each task loop top AND at
// progress points inside long ops so no inter-feed span approaches 30 s.
inline void wdtFeed() { esp_task_wdt_reset(); }
