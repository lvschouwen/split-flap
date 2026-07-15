#include "SystemStats.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_timer.h>

#include "ClockService.h"
#include "HelpersSerialHandling.h"
#include "MqttService.h"
#include "SystemStatsPolicy.h"
#include "UnitBus.h"
#include "WebEndpoints.h"  // webResetReasonString()

// CPU load per core from FreeRTOS run-time stats (sdkconfig has
// CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y with the esp_timer clock):
// idle-task runtime delta vs wall-clock delta between samples. Counters are
// uint32 microseconds — wrap-safe via unsigned subtraction (SystemStatsPolicy
// contract).
static uint32_t prevIdle[2] = {0, 0};
static uint32_t prevTotalUs = 0;
static bool cpuPrimed = false;

static SystemStatsSampler sampler;
static SemaphoreHandle_t ringMutex = nullptr;
static uint32_t nextSampleMs = 0;

void systemStatsInit() {
  ringMutex = xSemaphoreCreateMutex();
  if (ringMutex == nullptr) {
    SerialPrintln(F("FATAL: systemStats mutex allocation failed"));
    abort();
  }
}

void systemStatsTick() {
  uint32_t nowMs = millis();
  if ((int32_t)(nowMs - nextSampleMs) < 0) return;
  nextSampleMs = nowMs + SYSTEM_STATS_FAST_INTERVAL_S * 1000UL;

  SystemSample s;
  s.rssi = WiFi.status() == WL_CONNECTED ? (int16_t)WiFi.RSSI() : 0;
  s.freeHeap = ESP.getFreeHeap();
  s.maxAlloc = ESP.getMaxAllocHeap();
  s.psramFree = ESP.getFreePsram();
  s.tempC10 = (int16_t)(temperatureRead() * 10.0f);  // die temp, not ambient

  uint32_t idle0 = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(0);
  uint32_t idle1 = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(1);
  uint32_t totalUs = (uint32_t)esp_timer_get_time();
  if (cpuPrimed) {
    uint32_t window = totalUs - prevTotalUs;
    s.cpu0 = cpuLoadPercent(idle0 - prevIdle[0], window);
    s.cpu1 = cpuLoadPercent(idle1 - prevIdle[1], window);
  }
  prevIdle[0] = idle0;
  prevIdle[1] = idle1;
  prevTotalUs = totalUs;
  cpuPrimed = true;

  xSemaphoreTake(ringMutex, portMAX_DELAY);
  systemStatsIntake(sampler, s);  // dual-rate: latest 1 s, ring 5 s (#251)
  xSemaphoreGive(ringMutex);
}

size_t systemStatsJson(char* buf, size_t cap) {
  SystemNow now;
  now.uptimeS = (uint32_t)(esp_timer_get_time() / 1000000LL);
  now.minFreeHeap = ESP.getMinFreeHeap();
  now.i2cTx = unitBusTxCount();
  now.i2cErr = unitBusErrCount();
  now.mqttDrops = mqttDropCount();
  now.ntpAgeS = clockNtpAgeS();
  snprintf(now.resetReason, sizeof(now.resetReason), "%s",
           webResetReasonString());

  xSemaphoreTake(ringMutex, portMAX_DELAY);
  size_t n = buildSystemStatsJson(buf, cap, sampler, now);
  xSemaphoreGive(ringMutex);
  return n;
}

size_t systemStatsNowJson(char* buf, size_t cap) {
  SystemNow now;
  now.uptimeS = (uint32_t)(esp_timer_get_time() / 1000000LL);
  now.minFreeHeap = ESP.getMinFreeHeap();
  now.i2cTx = unitBusTxCount();
  now.i2cErr = unitBusErrCount();
  now.mqttDrops = mqttDropCount();
  now.ntpAgeS = clockNtpAgeS();
  snprintf(now.resetReason, sizeof(now.resetReason), "%s",
           webResetReasonString());

  xSemaphoreTake(ringMutex, portMAX_DELAY);
  size_t n = buildSystemNowJson(buf, cap, sampler.latest, now);
  xSemaphoreGive(ringMutex);
  return n;
}
