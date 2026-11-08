#include "BootTrace.h"

#include <Preferences.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "HelpersSerialHandling.h"
#include "SettingsJson.h"  // appendJsonString
#include "WebEndpoints.h"  // webResetReasonName

static const char* kNamespace = "sfboot";  // shared with RebootCause.cpp
static const char* kTraceKey = "trace";

static BootTrace trace;
static SemaphoreHandle_t traceMutex = nullptr;

static void persistLocked() {
  uint8_t blob[BOOT_TRACE_BLOB_LEN];
  bootTraceEncode(trace, blob);
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/false)) return;
  prefs.putBytes(kTraceKey, blob, sizeof(blob));
  prefs.end();
}

void bootTraceInit() {
  traceMutex = xSemaphoreCreateMutex();
  uint8_t blob[BOOT_TRACE_BLOB_LEN] = {0};
  Preferences prefs;
  if (prefs.begin(kNamespace, /*readOnly=*/true)) {
    size_t n = prefs.getBytes(kTraceKey, blob, sizeof(blob));
    prefs.end();
    if (!bootTraceDecode(blob, n, trace)) trace = BootTrace{};
  }
  bootTraceBegin(trace, (uint8_t)esp_reset_reason());
  persistLocked();  // single-threaded here: no tasks yet
}

// " alive >= N min" (#628), or nothing when the entry predates the field.
static String aliveText(const BootTraceEntry& e) {
  if (e.aliveMin == BOOT_TRACE_ALIVE_UNKNOWN) return String();
  return String(F(", alive >= ")) + String(e.aliveMin) + F(" min");
}

void bootTraceLogReport() {
  if (trace.count >= 2) {
    // The boot before this one, however it ended: a hang that a power pull
    // ended reads "online" here, its alive time bounds when it stopped.
    const BootTraceEntry& prev = trace.e[trace.count - 2];
    SerialPrintf("boot: previous boot reached %s%s, then %s\n", bootStageName(prev.stage),
                 aliveText(prev).c_str(),
                 webResetReasonName(trace.e[trace.count - 1].resetReason));
  }
  int failed = bootTraceFailedStreak(trace);
  if (failed == 0) return;
  SerialPrintf("boot: %d previous boot(s) never came online:\n", failed);
  for (int i = trace.count - 1 - failed; i < trace.count - 1; i++) {
    SerialPrintf("boot:   reset by %s, stopped at %s%s\n",
                 webResetReasonName(trace.e[i].resetReason),
                 bootStageName(trace.e[i].stage), aliveText(trace.e[i]).c_str());
  }
}

void bootTraceMarkStage(uint8_t stage) {
  if (traceMutex == nullptr) return;
  xSemaphoreTake(traceMutex, portMAX_DELAY);
  if (bootTraceMark(trace, stage)) persistLocked();
  xSemaphoreGive(traceMutex);
}

void bootTraceAliveLoop() {
  if (traceMutex == nullptr) return;
  static uint16_t lastStep = 0;  // netTask only; bootTraceBegin stored 0
  uint64_t uptimeMs = (uint64_t)esp_timer_get_time() / 1000ULL;  // no 49-day wrap
  if (bootTraceAliveStep(uptimeMs) == lastStep) return;  // between steps: no lock
  // Never wait: a stage mark holds the lock for its own write; retry next loop.
  if (xSemaphoreTake(traceMutex, 0) != pdTRUE) return;
  if (bootTraceAliveTick(trace, uptimeMs)) persistLocked();
  lastStep = trace.e[trace.count - 1].aliveMin;
  xSemaphoreGive(traceMutex);
}

String bootTraceJson() {
  BootTrace copy;
  if (traceMutex != nullptr) {
    xSemaphoreTake(traceMutex, portMAX_DELAY);
    copy = trace;
    xSemaphoreGive(traceMutex);
  }
  String out = "[";
  for (int i = 0; i < copy.count; i++) {
    if (i > 0) out += ',';
    out += "{\"reset\":";
    appendJsonString(out, webResetReasonName(copy.e[i].resetReason));
    out += ",\"stage\":";
    appendJsonString(out, bootStageName(copy.e[i].stage));
    if (copy.e[i].aliveMin != BOOT_TRACE_ALIVE_UNKNOWN) {  // absent = unknown
      out += ",\"aliveMin\":";
      out += (unsigned)copy.e[i].aliveMin;
    }
    out += '}';
  }
  out += ']';
  return out;
}
