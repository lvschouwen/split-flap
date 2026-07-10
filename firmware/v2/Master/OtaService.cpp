#include "OtaService.h"

#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "FactorySlot.h"
#include "HelpersSerialHandling.h"

// Snapshotted once in otaServiceInit() (single-threaded setup); `confirmed`
// is the only field that changes afterwards (netTask writes, async /settings
// handler reads — hence the mutex).
static bool pendingAtBoot = false;
static bool rolledBack = false;
static bool confirmed = false;
static SemaphoreHandle_t otaStateMutex = nullptr;

struct OtaStateLock {
  OtaStateLock() { xSemaphoreTake(otaStateMutex, portMAX_DELAY); }
  ~OtaStateLock() { xSemaphoreGive(otaStateMutex); }
  OtaStateLock(const OtaStateLock&) = delete;
  OtaStateLock& operator=(const OtaStateLock&) = delete;
};

// Strong override of the core's weak hook (esp32-hal-misc.c, C linkage):
// returning true skips initArduino()'s auto-confirm so a broken image that
// crashes before the netif comes up is rolled back by the bootloader.
extern "C" bool verifyRollbackLater() { return true; }

void otaServiceInit() {
  otaStateMutex = xSemaphoreCreateMutex();
  if (otaStateMutex == nullptr) {
    Serial.println(F("FATAL: otaStateMutex allocation failed"));
    abort();  // panic into the coredump partition, same as the other inits
  }

  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
    pendingAtBoot = (state == ESP_OTA_IMG_PENDING_VERIFY);
  }
  rolledBack = esp_ota_get_last_invalid_partition() != nullptr;

  if (pendingAtBoot) {
    Serial.printf("ota: image on %s is PENDING_VERIFY — confirm armed on netif-up\n",
                  running->label);
  } else if (rolledBack) {
    Serial.println(F("ota: bootloader ROLLED BACK a failed image — running the previous slot"));
  }
}

void otaHealthConfirm() {
  bool doConfirm = false;
  {
    OtaStateLock lock;
    if (pendingAtBoot && !confirmed) {
      confirmed = true;
      doConfirm = true;
    }
  }
  if (!doConfirm) return;
  // Flash write (otadata) outside the lock: /settings readers must not
  // stall behind it.
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    SerialPrintln(F("OTA image confirmed (netif up) — rollback cancelled"));
  } else {
    SerialPrintln("OTA confirm FAILED: " + String(esp_err_to_name(err)));
  }
}

OtaVerdict otaVerdictSnapshot() {
  bool pendingNow, confirmedNow, rolledBackNow;
  {
    OtaStateLock lock;
    pendingNow = pendingAtBoot && !confirmed;
    confirmedNow = confirmed;
    rolledBackNow = rolledBack;
  }
  return synthesizeOtaVerdict(rolledBackNow, pendingNow, confirmedNow);
}

String otaDebugJson() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  const esp_partition_t* invalid = esp_ota_get_last_invalid_partition();
  OtaVerdict v = otaVerdictSnapshot();

  String out;
  out.reserve(192);
  out += "{\"running\":\"";
  out += running ? running->label : "?";
  out += "\",\"next\":\"";
  out += next ? next->label : "?";
  out += "\",\"lastInvalid\":";
  if (invalid) {
    out += '"';
    out += invalid->label;
    out += '"';
  } else {
    out += "null";
  }
  out += ",\"lastFlashResult\":\"";
  out += v.lastFlashResult;  // fixed vocabulary, no escaping needed
  out += "\",\"otaReverted\":";
  out += v.otaReverted ? "true" : "false";
  out += ",\"factoryValid\":";  // rescue image installed? (#195 bench aid)
  out += factorySlotImageValid() ? "true" : "false";
  out += '}';
  return out;
}
