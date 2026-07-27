// Owner of the board's runtime display content — see ContentState.h for why
// this is a module of its own rather than part of an endpoint layer.

#include "ContentState.h"

#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "FlashLog.h"
#include "HelpersSerialHandling.h"

namespace {

// Bound once by contentStateInit(); readers check both before touching
// either, so a call that races init degrades to an empty snapshot instead
// of dereferencing null.
MasterSettings* liveSettings = nullptr;
SettingsStore* liveStore = nullptr;

String currentInputText;
DisplaySource currentInputSource = DisplaySource::Unknown;

bool pendingReboot = false;
uint32_t rebootRequestedAtMs = 0;

SemaphoreHandle_t stateMutex = nullptr;

struct StateLock {
  StateLock() { xSemaphoreTake(stateMutex, portMAX_DELAY); }
  ~StateLock() { xSemaphoreGive(stateMutex); }
  StateLock(const StateLock&) = delete;
  StateLock& operator=(const StateLock&) = delete;
};

bool ready() {
  return stateMutex != nullptr && liveSettings != nullptr &&
         liveStore != nullptr;
}

// A reboot waits this long after being asked for, so whatever asked has a
// chance to answer its caller and the flash log can be flushed. It used to
// exist so an HTTP response could go out before the restart; it is kept
// because the same courtesy applies to any protocol.
constexpr uint32_t REBOOT_HOLD_MS = 400;

}  // namespace

void contentStateInit(MasterSettings& settings, SettingsStore& store) {
  if (stateMutex == nullptr) stateMutex = xSemaphoreCreateMutex();
  liveSettings = &settings;
  liveStore = &store;
}

DisplayContent contentSnapshot() {
  DisplayContent c;
  if (!ready()) return c;
  StateLock lock;
  c.deviceMode = liveSettings->deviceMode;
  c.inputText = currentInputText;
  c.inputTextSource = currentInputSource;
  c.alignment = liveSettings->alignment;
  c.flapSpeed = liveSettings->flapSpeed;
  return c;
}

bool contentSetMode(const String& mode) {
  if (!ready() || mode.length() == 0) return false;
  StateLock lock;
  if (liveSettings->deviceMode == mode) return false;
  liveSettings->deviceMode = mode;
  saveDeviceMode(*liveStore, mode);
  return true;
}

bool contentSetSpeed(int speed) {
  if (!ready()) return false;
  StateLock lock;
  if (liveSettings->flapSpeed == speed) return false;
  liveSettings->flapSpeed = speed;
  saveFlapSpeed(*liveStore, speed);
  return true;
}

bool contentSetAlignment(const String& alignment) {
  if (!ready() || alignment.length() == 0) return false;
  StateLock lock;
  if (liveSettings->alignment == alignment) return false;
  liveSettings->alignment = alignment;
  saveAlignment(*liveStore, alignment);
  return true;
}

void contentSetText(const String& text, DisplaySource source) {
  if (stateMutex == nullptr) return;
  StateLock lock;
  currentInputText = text;
  currentInputSource = source;
}

void contentRequestReboot() {
  if (stateMutex == nullptr) return;
  StateLock lock;
  pendingReboot = true;
  rebootRequestedAtMs = millis();
}

void contentStateTick() {
  if (stateMutex == nullptr) return;
  bool due = false;
  {
    StateLock lock;
    due = pendingReboot &&
          (int32_t)(millis() - rebootRequestedAtMs) >= (int32_t)REBOOT_HOLD_MS;
  }
  if (!due) return;
  SerialPrintln(F("Rebooting..."));
  Serial.flush();
  flashLogTick(true);  // catch the reboot line itself
  ESP.restart();
}

String contentTimezone() {
  if (!ready()) return String();
  StateLock lock;
  return liveSettings->timezonePosix;
}

const char* contentResetReasonString() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "Power on";
    case ESP_RST_EXT:       return "External reset";
    case ESP_RST_SW:        return "Software reset";
    case ESP_RST_PANIC:     return "Exception/panic";
    case ESP_RST_INT_WDT:   return "Interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "Task watchdog";
    case ESP_RST_WDT:       return "Other watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
    case ESP_RST_BROWNOUT:  return "Brownout";
    case ESP_RST_SDIO:      return "SDIO reset";
    default:                return "Unknown";
  }
}
