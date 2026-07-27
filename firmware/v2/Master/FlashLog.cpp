// FlashLog.cpp (#206) — glue for FlashLog.h; decisions live in
// FlashLogPolicy.h. Bench-tier (LittleFS + FreeRTOS; not native-buildable).

#include "FlashLog.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <time.h>

#include "BuildVersion.h"
#include "ClockPolicy.h"
#include "FlashLogPolicy.h"
#include "HelpersSerialHandling.h"
#include "LargeAlloc.h"

static const char* LOG_PATH = "/log.txt";
static const char* LOG_PREV_PATH = "/log.prev.txt";

// Staging state, guarded by stageMutex. `available` is written once in
// flashLogInit() (single-threaded setup context) and read-only afterwards.
static bool available = false;
static SemaphoreHandle_t stageMutex = nullptr;
static char* stageBuf = nullptr;      // producers append here
static char* flushBuf = nullptr;      // netTask's copy-out, written lock-free
static FlashLogStageState stage;
static bool clearRequested = false;
static uint32_t lastFlushMs = 0;

const char* flashLogCurrentPath() { return LOG_PATH; }
const char* flashLogPreviousPath() { return LOG_PREV_PATH; }
bool flashLogAvailable() { return available; }

void flashLogInit() {
  static bool attempted = false;
  if (attempted) return;
  attempted = true;

  // Mount by partition label; format on first use. basePath is LittleFS's
  // default — web responses use the FS object, not VFS paths.
  // Failure diagnostics go through SerialPrintln so they reach the web-log
  // ring too — "why did my flash log never appear" must be debuggable
  // WITHOUT a USB cable. Safe here: `available` is still false, so the
  // flash-log tee inside webLogAppend is a no-op (no recursion).
  if (!LittleFS.begin(true, "/littlefs", 10, "storage")) {
    SerialPrintln(F("FlashLog: storage mount failed — flash log disabled"));
    return;
  }
  stageMutex = xSemaphoreCreateMutex();
  stageBuf = (char*)largeAlloc(FLASH_LOG_STAGE_CAP);
  flushBuf = (char*)largeAlloc(FLASH_LOG_STAGE_CAP);
  if (stageMutex == nullptr || stageBuf == nullptr || flushBuf == nullptr) {
    SerialPrintln(F("FlashLog: allocation failed — flash log disabled"));
    return;
  }

  // Boot marker straight to flash (setup context, nothing races yet): a
  // reboot is findable even if the boot then wedges before the first tick.
  File f = LittleFS.open(LOG_PATH, FILE_APPEND, true);
  if (f) {
    f.printf("\n===== boot rev=%s reset=%d heap=%u =====\n", GIT_REV,
             (int)esp_reset_reason(), (unsigned)ESP.getFreeHeap());
    f.close();
  }
  lastFlushMs = millis();
  available = true;
}

void flashLogStage(const char* data, size_t len) {
  if (!available || data == nullptr || len == 0) return;
  // Stamp text is computed OUTSIDE the lock (time/millis are lock-free);
  // wall clock once NTP synced, else milliseconds since boot.
  char stamp[LOG_STAMP_MAX];
  time_t now = time(nullptr);
  if (clockIsTimeSynced(now)) {
    struct tm local;
    localtime_r(&now, &local);
    logStampClock(stamp, (unsigned)local.tm_hour, (unsigned)local.tm_min,
                  (unsigned)local.tm_sec);
  } else {
    logStampBoot(stamp, millis());
  }

  xSemaphoreTake(stageMutex, portMAX_DELAY);
  static LogLineStamper stamper;  // guarded by stageMutex like the buffer
  size_t consumed = 0;
  stage.used += logStamperApply(stamper, stamp, data, len,
                                stageBuf + stage.used,
                                FLASH_LOG_STAGE_CAP - stage.used, consumed);
  stage.dropped += (uint32_t)(len - consumed);
  xSemaphoreGive(stageMutex);
}

void flashLogRequestClear() {
  if (!available) return;
  xSemaphoreTake(stageMutex, portMAX_DELAY);
  clearRequested = true;
  xSemaphoreGive(stageMutex);
}

void flashLogTick(bool force) {
  if (!available) return;

  // Copy staged bytes out under the lock; every filesystem call below runs
  // lock-free so producers never wait on flash latency.
  size_t len = 0;
  uint32_t droppedNow = 0;
  bool doClear = false;
  xSemaphoreTake(stageMutex, portMAX_DELAY);
  doClear = clearRequested;
  clearRequested = false;
  if (doClear) {
    // Staged-but-unflushed bytes predate the clear — discard with it.
    stage.used = 0;
    stage.dropped = 0;
  } else if (force || flashLogShouldFlush(stage.used, FLASH_LOG_STAGE_CAP,
                                          millis() - lastFlushMs,
                                          FLASH_LOG_FLUSH_INTERVAL_MS)) {
    len = stage.used;
    droppedNow = stage.dropped;
    memcpy(flushBuf, stageBuf, len);
    stage.used = 0;
    stage.dropped = 0;
  }
  xSemaphoreGive(stageMutex);

  if (doClear) {
    LittleFS.remove(LOG_PATH);
    LittleFS.remove(LOG_PREV_PATH);
    lastFlushMs = millis();
    return;
  }
  if (len == 0 && droppedNow == 0) return;

  File f = LittleFS.open(LOG_PATH, FILE_APPEND, true);
  if (!f) return;  // staged data already consumed; next lines still flow
  // Day boundary marker (netTask-private state, no lock needed). Local
  // date via the configTzTime'd TZ; unsynced clock (pre-NTP) writes none.
  static uint32_t lastMarkedDay = 0;
  time_t now = time(nullptr);
  uint32_t nowDay = 0;
  if (clockIsTimeSynced(now)) {
    struct tm local;
    localtime_r(&now, &local);
    nowDay = (uint32_t)(local.tm_year + 1900) * 10000 +
             (uint32_t)(local.tm_mon + 1) * 100 + (uint32_t)local.tm_mday;
  }
  if (flashLogDayMarkerDue(lastMarkedDay, nowDay)) {
    f.printf("\n===== %04u-%02u-%02u =====\n", nowDay / 10000,
             (nowDay / 100) % 100, nowDay % 100);
    lastMarkedDay = nowDay;
  }
  if (len > 0) f.write((const uint8_t*)flushBuf, len);
  if (droppedNow > 0) {
    f.printf("\n[flashlog: %lu bytes dropped]\n", (unsigned long)droppedNow);
  }
  size_t size = f.size();
  f.close();
  lastFlushMs = millis();

  if (flashLogShouldRotate(size)) {
    LittleFS.remove(LOG_PREV_PATH);
    LittleFS.rename(LOG_PATH, LOG_PREV_PATH);
  }
}


// --- Print sink (adopted from the removed web log) --------------------------
FlashLogPrinter flashLogPrinter;

size_t FlashLogPrinter::write(uint8_t b) {
  char c = (char)b;
  flashLogStage(&c, 1);
  return 1;
}

size_t FlashLogPrinter::write(const uint8_t* buffer, size_t size) {
  flashLogStage((const char*)buffer, size);
  return size;
}
