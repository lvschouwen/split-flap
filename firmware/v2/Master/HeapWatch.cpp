#include "HeapWatch.h"

#include <Update.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>

#include <atomic>

#include "HelpersSerialHandling.h"
#include "ReflashPlan.h"  // reflashInProgress
#include "ReleaseUpdate.h"
#include "Tasks.h"         // displaySnapshotGet
#include "WallState.h"     // wallUnitUpdateRunning
#include "WebEndpoints.h"  // webRequestReboot

RTC_NOINIT_ATTR static HeapWatchRecord record;

// netTask only, after setup().
static HeapWatchState state;
static bool restartRequested = false;
// Read by the web task.
static std::atomic<uint32_t> restartsNow{0};
static std::atomic<uint32_t> lowForS{0};

void heapWatchBoot() {
  heapWatchRecordBoot(record);
  restartsNow.store(heapWatchRestarts(record));
}

void heapWatchLogReport() {
  const uint32_t n = heapWatchRestarts(record);
  if (n == 0) return;
  SerialPrintf("heap watch: %lu low-heap restart(s) since power-on, the last "
               "%lu min of uptime ago\n",
               (unsigned long)n, (unsigned long)record.minutesSince);
}

// Only looked at once the restart is due: it copies the display snapshot.
static bool restartWouldCutAJob() {
  return Update.isRunning() || releaseUpdateRunning() || wallUnitUpdateRunning() ||
         reflashInProgress(displaySnapshotGet().reflash);
}

static void act(const HeapWatchVerdict& v, uint32_t freeNow) {
  if (v.logLow) {
    SerialPrintf("heap watch: internal free heap %lu B below %u B for %lu s — "
                 "restart after %lu s\n",
                 (unsigned long)freeNow, HEAP_WATCH_LOW_BYTES,
                 (unsigned long)(HEAP_WATCH_LOG_MS / 1000UL),
                 (unsigned long)(HEAP_WATCH_RESTART_MS / 1000UL));
  }
  if (v.logRecovered) {
    SerialPrintf("heap watch: internal free heap back to %lu B\n", (unsigned long)freeNow);
  }
  if (v.logWithheld) {
    SerialPrintln(heapWatchAllowed(record)
                      ? F("heap watch: restart due, waiting for the running job")
                      : F("heap watch: restart due, but one ran less than 6 h ago — staying up"));
  }
  if (!v.restart) return;
  restartRequested = true;
  heapWatchRecordTaken(record);
  restartsNow.store(heapWatchRestarts(record));
  char cause[96];
  snprintf(cause, sizeof(cause),
           "low heap: internal free %lu B (lowest %lu) below %u B for %lu s (#504)",
           (unsigned long)freeNow, (unsigned long)state.lowest, HEAP_WATCH_LOW_BYTES,
           (unsigned long)(HEAP_WATCH_RESTART_MS / 1000UL));
  SerialPrintf("heap watch: restarting — %s\n", cause);
  webRequestReboot(cause);  // the drain stamps the cause; next boot reports it
}

void heapWatchTick() {
  static uint32_t nextMs = 0;
  static uint32_t nextMinuteMs = 60000UL;
  const uint32_t now = millis();
  if (restartRequested || (int32_t)(now - nextMs) < 0) return;
  nextMs = now + 1000;
  if ((int32_t)(now - nextMinuteMs) >= 0) {
    nextMinuteMs += 60000UL;
    heapWatchRecordMinute(record);
  }
  HeapWatchInput in;
  in.nowMs = now;
  in.freeInternal = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  in.restartAllowed = heapWatchAllowed(record);
  if (in.freeInternal < HEAP_WATCH_LOW_BYTES && state.low &&
      now - state.lowSinceMs >= HEAP_WATCH_RESTART_MS) {
    in.restartBlocked = restartWouldCutAJob();
  }
  const HeapWatchVerdict v = heapWatchStep(state, in);
  lowForS.store(state.low ? (now - state.lowSinceMs) / 1000UL : 0);
  act(v, in.freeInternal);
}

String heapWatchJson() {
  String out = "{\"lowBytes\":";
  out += String((unsigned)HEAP_WATCH_LOW_BYTES);
  out += ",\"lowForS\":";
  out += String((unsigned long)lowForS.load());
  out += ",\"restartAfterS\":";
  out += String((unsigned long)(HEAP_WATCH_RESTART_MS / 1000UL));
  out += ",\"restarts\":";
  out += String((unsigned long)restartsNow.load());
  out += '}';
  return out;
}
