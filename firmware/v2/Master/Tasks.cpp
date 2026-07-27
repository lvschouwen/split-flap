#include "Tasks.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>

// #289 dummy mode: the settings-stored unit-count override, seeded by
// tasksInit() and updated live by the settings drain (netTask). displayTask
// reads it at every fold; 0 = auto (probe-derived width).
static std::atomic<int> unitWidthOverride{0};

// #331 headless mode: true while deviceRole is a headless role. It forces
// displayWidth 0 (the board renders nothing, no phantom row), overriding the
// probe ceiling AND the #289 override — passed to displayApplyUnitFacts as
// the -1 sentinel via effectiveWidthOverride(). Seeded by tasksInit(),
// pushed live by the settings drain (netTask).
static std::atomic<bool> deviceRoleHeadless{false};
// #412: false suppresses the boot auto-install so the fleet can be converged
// unit by unit. Read once by displayTask at boot; pushed live by the settings
// drain so a mid-session change lands without a reboot.
static std::atomic<bool> reflashOnBootEnabled{true};

void tasksSetUnitCountOverride(int count) {
  unitWidthOverride.store(count, std::memory_order_relaxed);
}

void tasksSetDeviceRole(const String& role) {
  deviceRoleHeadless.store(isHeadlessRole(role), std::memory_order_relaxed);
}

void tasksSetReflashOnBoot(bool enabled) {
  reflashOnBootEnabled.store(enabled, std::memory_order_relaxed);
}

bool tasksReflashOnBoot() {
  return reflashOnBootEnabled.load(std::memory_order_relaxed);
}

// The width-override value handed to displayApplyUnitFacts: -1 (force width 0)
// while headless, else the #289 unit-count override (0 = auto).
int effectiveWidthOverride() {
  return deviceRoleHeadless.load(std::memory_order_relaxed)
             ? -1
             : unitWidthOverride.load(std::memory_order_relaxed);
}

bool tasksUnitCountOverridePinned() {
  return unitWidthOverride.load(std::memory_order_relaxed) > 0;
}

#include "HeadlessPolicy.h"
#include "HelpersSerialHandling.h"
#include "MqttService.h"
#include "StatusLed.h"
#include "SystemStats.h"
#include "TaskWatchdog.h"
#include "ContentState.h"
#include "FlashLog.h"
#include "WifiService.h"

// --- static RTOS allocation (memory policy rule 1) ---------------------------
// Stacks and queue storage are static arrays in internal SRAM: deterministic
// footprint, visible in the map file, no heap fragmentation from task churn.
// Sizes are the spec's table; the heartbeat prints each task's high-water
// mark so the numbers get validated (and trimmed) on real hardware.

// 4096 crashed displayTask on real hardware (split-flap-c8a746) inside the
// boot probe: unitBusProbe's per-unit SerialPrintf -> Print::printf ->
// newlib vsnprintf, faulting in the context-switch path (an interrupt frame
// pushed onto an exhausted stack), coredump backtrace uncorrupted. vsnprintf
// is the deepest chain displayTask runs per unit, and runReflashJob rides the
// same task, so the #205 fleet reflash pays that peak too. The heartbeat HWM
// column stays the trim-down evidence.
static constexpr uint32_t DISPLAY_TASK_STACK = 16384;
// 2048 leaves only ~124 B HWM on real hardware — newlib's first
// tzset/localtime parse of the POSIX TZ string runs deep in the ticker.
static constexpr uint32_t CLOCK_TASK_STACK = 4096;
// netTask is the heaviest domain task: wifi + flashLog + stats
// follower + SSE + system-stats all share one loop. 4096 overflowed the
// canary on real hardware (split-flap-c8a746) inside flashLogTick's
// LittleFS.open → fopen → esp_flash_read, whose cross-core cache-disable
// IPC is the deepest chain netTask ever runs. The figure was set when the
// SSE and cluster work sat on this task too; both are gone, so it is now
// generous — the heartbeat HWM column is the evidence for trimming it.
static constexpr uint32_t NET_TASK_STACK = 8192;
// espMqttClient internals + the 512 B discovery build buffers (#224); the
// heartbeat's HWM column is the trim-down evidence.
static constexpr uint32_t MQTT_TASK_STACK = 6144;

static constexpr UBaseType_t DISPLAY_TASK_PRIORITY = 3;  // flap timing wins
static constexpr UBaseType_t DOMAIN_TASK_PRIORITY = 1;   // everything else

static constexpr BaseType_t DISPLAY_CORE = 1;  // with loopTask (heartbeat)
static constexpr BaseType_t NETWORK_CORE = 0;  // with WiFi/LWIP/AsyncTCP

static constexpr UBaseType_t DISPLAY_QUEUE_DEPTH = 16;
static constexpr UBaseType_t MQTT_INBOX_DEPTH = 8;

static StaticTask_t displayTaskBuf, clockTaskBuf, netTaskBuf, mqttTaskBuf;
static StackType_t displayTaskStack[DISPLAY_TASK_STACK];
static StackType_t clockTaskStack[CLOCK_TASK_STACK];
static StackType_t netTaskStack[NET_TASK_STACK];
static StackType_t mqttTaskStack[MQTT_TASK_STACK];

static StaticQueue_t displayQueueBuf;
static uint8_t displayQueueStorage[DISPLAY_QUEUE_DEPTH * sizeof(DisplayCommand)];
QueueHandle_t displayQueue = nullptr;  // shared with DisplayTask.cpp (TasksInternal.h)

static StaticQueue_t mqttInboxBuf;
static uint8_t mqttInboxStorage[MQTT_INBOX_DEPTH * sizeof(MqttInboxMessage)];
static QueueHandle_t mqttInbox = nullptr;

static TaskHandle_t displayTaskHandle, clockTaskHandle, netTaskHandle,
    mqttTaskHandle;

// --- display snapshot (single writer: displayTask) ---------------------------

static DisplaySnapshot snapshot;
static SemaphoreHandle_t snapshotMutex = nullptr;

void snapshotPublish(const DisplaySnapshot& next) {
  xSemaphoreTake(snapshotMutex, portMAX_DELAY);
  snapshot = next;
  xSemaphoreGive(snapshotMutex);
}

DisplaySnapshot displaySnapshotGet() {
  DisplaySnapshot copy;
  if (snapshotMutex == nullptr) return copy;  // pre-init: defaults
  xSemaphoreTake(snapshotMutex, portMAX_DELAY);
  copy = snapshot;
  xSemaphoreGive(snapshotMutex);
  return copy;
}

bool displayEnqueue(const DisplayCommand& cmd) {
  if (displayQueue == nullptr) return false;
  return xQueueSend(displayQueue, &cmd, 0) == pdTRUE;
}

bool displayQueueFull() {
  if (displayQueue == nullptr) return true;
  return uxQueueSpacesAvailable(displayQueue) == 0;
}

// Maintenance-op sequence (#204): ++ first so the first real seq is 1
// (MaintResult's 0 stays "nothing executed yet"). The 0-skip also keeps the
// contract intact across a uint32 wrap — unreachable in this device's
// lifetime, free to guard anyway.
static std::atomic<uint32_t> maintSeqCounter{0};
uint32_t displayNextMaintSeq() {
  uint32_t seq = ++maintSeqCounter;
  if (seq == 0) seq = ++maintSeqCounter;
  return seq;
}

bool mqttInboxPost(const MqttInboxMessage& msg) {
  if (mqttInbox == nullptr) return false;
  return xQueueSend(mqttInbox, &msg, 0) == pdTRUE;
}
#include "TasksInternal.h"

// --- core 0: network domain ----------------------------------------------------

// WiFi join/portal supervision (#188) + the web staging drain (settings
// posts, pending reboot) that #186 ran from loop().
struct NetTaskContext {
  MasterSettings* settings;
  SettingsStore* store;
};

static void netTaskMain(void* arg) {
  SerialPrintf("netTask up on core %d\n", xPortGetCoreID());
  auto* ctx = static_cast<NetTaskContext*>(arg);
  if (esp_err_t e = wdtSubscribeSelf(); e != ESP_OK)
    SerialPrintf("wdt: net subscribe -> %s\n", esp_err_to_name(e));
  for (;;) {
    wdtFeed();
    wifiServiceTick();
    contentStateTick();  // staged reboot, performed once the log is flushed
    flashLogTick();      // netTask is the sole flash writer
    statusLedTick();
    systemStatsTick();  // #245/#251: self-throttled, 1 s fast + 5 s ring
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// MQTT client lifecycle (#224): drain the inbox, then tick the service
// (client pump, reconnect/backoff, publishers, notification dwell). The
// 10 ms cadence matches netTask; the service must be initialised in setup()
// before tasksInit() starts this task.
static void mqttTaskMain(void*) {
  SerialPrintf("mqttTask up on core %d\n", xPortGetCoreID());
  MqttInboxMessage msg;
  if (esp_err_t e = wdtSubscribeSelf(); e != ESP_OK)
    SerialPrintf("wdt: mqtt subscribe -> %s\n", esp_err_to_name(e));
  for (;;) {
    wdtFeed();
    while (xQueueReceive(mqttInbox, &msg, 0) == pdTRUE) {
      mqttServiceHandleInbox(msg);
    }
    mqttServiceTick();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// --- lifecycle -----------------------------------------------------------------

void tasksInit(MasterSettings& settings, SettingsStore& store) {
  unitWidthOverride.store(settings.unitCountOverride,
                          std::memory_order_relaxed);
  // #412: seed the boot auto-install brake BEFORE displayTask starts — the
  // gate is read once during its boot sequence, so a stored false that is not
  // seeded here would let the fleet converge on exactly the reboot the
  // operator set it to prevent.
  reflashOnBootEnabled.store(settings.reflashOnBoot, std::memory_order_relaxed);
  deviceRoleHeadless.store(isHeadlessRole(settings.deviceRole),
                           std::memory_order_relaxed);  // #331
  snapshotMutex = xSemaphoreCreateMutex();
  if (snapshotMutex == nullptr) {
    // Boot-time OOM: taking a null handle is UB, so fail loudly instead —
    // abort() panics into the coredump partition. (The static queues below
    // cannot fail: their storage is compile-time arrays.)
    Serial.println(F("FATAL: snapshotMutex allocation failed"));
    abort();
  }
  displayQueue = xQueueCreateStatic(DISPLAY_QUEUE_DEPTH, sizeof(DisplayCommand),
                                    displayQueueStorage, &displayQueueBuf);
  mqttInbox = xQueueCreateStatic(MQTT_INBOX_DEPTH, sizeof(MqttInboxMessage),
                                 mqttInboxStorage, &mqttInboxBuf);

  // netTask's context outlives it (task never exits); static, not heap.
  static NetTaskContext netCtx{&settings, &store};

  displayTaskHandle = xTaskCreateStaticPinnedToCore(
      displayTaskMain, "display", DISPLAY_TASK_STACK, nullptr,
      DISPLAY_TASK_PRIORITY, displayTaskStack, &displayTaskBuf, DISPLAY_CORE);
  clockTaskHandle = xTaskCreateStaticPinnedToCore(
      clockTaskMain, "clock", CLOCK_TASK_STACK, nullptr, DOMAIN_TASK_PRIORITY,
      clockTaskStack, &clockTaskBuf, DISPLAY_CORE);
  netTaskHandle = xTaskCreateStaticPinnedToCore(
      netTaskMain, "net", NET_TASK_STACK, &netCtx, DOMAIN_TASK_PRIORITY,
      netTaskStack, &netTaskBuf, NETWORK_CORE);
  mqttTaskHandle = xTaskCreateStaticPinnedToCore(
      mqttTaskMain, "mqtt", MQTT_TASK_STACK, nullptr, DOMAIN_TASK_PRIORITY,
      mqttTaskStack, &mqttTaskBuf, NETWORK_CORE);
}

void tasksHeartbeatReport() {
  Serial.printf(
      "[%8lu ms] heap %u KB free (min %u KB), psram %u KB free | stack HWM: "
      "display %u, clock %u, net %u, mqtt %u, loop %u\n",
      (unsigned long)millis(), ESP.getFreeHeap() / 1024,
      ESP.getMinFreeHeap() / 1024, ESP.getFreePsram() / 1024,
      (unsigned)uxTaskGetStackHighWaterMark(displayTaskHandle),
      (unsigned)uxTaskGetStackHighWaterMark(clockTaskHandle),
      (unsigned)uxTaskGetStackHighWaterMark(netTaskHandle),
      (unsigned)uxTaskGetStackHighWaterMark(mqttTaskHandle),
      (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}
