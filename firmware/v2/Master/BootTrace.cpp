#include "BootTrace.h"

#include <Preferences.h>
#include <esp_system.h>
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

void bootTraceLogReport() {
  int failed = bootTraceFailedStreak(trace);
  if (failed == 0) return;
  SerialPrintf("boot: %d previous boot(s) never came online:\n", failed);
  for (int i = trace.count - 1 - failed; i < trace.count - 1; i++) {
    SerialPrintf("boot:   reset by %s, stopped at %s\n",
                 webResetReasonName(trace.e[i].resetReason),
                 bootStageName(trace.e[i].stage));
  }
}

void bootTraceMarkStage(uint8_t stage) {
  if (traceMutex == nullptr) return;
  xSemaphoreTake(traceMutex, portMAX_DELAY);
  if (bootTraceMark(trace, stage)) persistLocked();
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
    out += '}';
  }
  out += ']';
  return out;
}
