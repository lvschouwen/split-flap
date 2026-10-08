#include "CrashContext.h"

#include <esp_attr.h>
#include <esp_system.h>

#include "HelpersSerialHandling.h"
#include "SettingsJson.h"  // appendJsonString
#include "WebEndpoints.h"  // webResetReasonName

static_assert(ESP_RST_POWERON == CRASH_RESET_POWERON &&
                  ESP_RST_SW == CRASH_RESET_SW &&
                  ESP_RST_PANIC == CRASH_RESET_PANIC &&
                  ESP_RST_INT_WDT == CRASH_RESET_INT_WDT &&
                  ESP_RST_TASK_WDT == CRASH_RESET_TASK_WDT &&
                  ESP_RST_WDT == CRASH_RESET_WDT &&
                  ESP_RST_BROWNOUT == CRASH_RESET_BROWNOUT &&
                  ESP_RST_CPU_LOCKUP == CRASH_RESET_CPU_LOCKUP,
              "CrashReset mirrors esp_reset_reason_t");

RTC_NOINIT_ATTR static CrashContext rtcCtx;

static bool haveReport = false;
static int reportReason = 0;
static CrashContext report;

void crashCtxBoot() {
  int reason = (int)esp_reset_reason();
  if (crashCtxValid(rtcCtx) && crashCtxWorthReporting(reason)) {
    report = rtcCtx;
    reportReason = reason;
    haveReport = true;
    SerialPrintf("crash: reset by %s — tasks at the time:\n",
                 webResetReasonName(reason));
    for (int i = 0; i < CRASH_CTX_SLOTS; i++) {
      SerialPrintf("crash:   %-8s %-12s arg 0x%02x for %lu ms\n",
                   crashSlotName(i), crashActName(report.slot[i].act),
                   (unsigned)report.slot[i].arg,
                   (unsigned long)crashCtxAgeMs(report, i));
    }
  }
  crashCtxArm(rtcCtx);
}

void crashCtxMark(int slot, uint8_t act, uint8_t arg) {
  crashCtxSet(rtcCtx, slot, act, arg, millis());
}

void crashCtxHeartbeat() { crashCtxTick(rtcCtx, millis()); }

String crashCtxReportJson() {
  if (!haveReport) return "{}";
  String out = "{\"reset\":";
  appendJsonString(out, webResetReasonName(reportReason));
  out += ",\"tasks\":[";
  for (int i = 0; i < CRASH_CTX_SLOTS; i++) {
    if (i > 0) out += ',';
    out += "{\"task\":";
    appendJsonString(out, crashSlotName(i));
    out += ",\"act\":";
    appendJsonString(out, crashActName(report.slot[i].act));
    out += ",\"arg\":";
    out += report.slot[i].arg;
    out += ",\"ageMs\":";
    out += String((unsigned long)crashCtxAgeMs(report, i));
    out += '}';
  }
  out += "]}";
  return out;
}
