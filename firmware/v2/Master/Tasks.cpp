#include "NetLiveness.h"  // #501 probes (workerTask)
#include "Tasks.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>

#include "CrashContext.h"  // task activity breadcrumb (#504)

// #289 dummy mode: the settings-stored unit-count override, seeded by
// tasksInit() and updated live by the settings drain (netTask). displayTask
// reads it at every fold; 0 = auto (probe-derived width).
static std::atomic<int> unitWidthOverride{0};

// #412: false suppresses the boot auto-install so the fleet can be converged
// unit by unit. Read once by displayTask at boot; pushed live by the settings
// drain so a mid-session change lands without a reboot.
static std::atomic<bool> reflashOnBootEnabled{true};
// #227: the wall's quiet setting, read by every content producer.
static std::atomic<bool> quietEnabled{false};

void tasksSetQuiet(bool quiet) {
  quietEnabled.store(quiet, std::memory_order_relaxed);
}

bool tasksQuiet() { return quietEnabled.load(std::memory_order_relaxed); }

void tasksSetUnitCountOverride(int count) {
  unitWidthOverride.store(count, std::memory_order_relaxed);
}

void tasksSetReflashOnBoot(bool enabled) {
  reflashOnBootEnabled.store(enabled, std::memory_order_relaxed);
}

bool tasksReflashOnBoot() {
  return reflashOnBootEnabled.load(std::memory_order_relaxed);
}

// The width-override value handed to displayApplyUnitFacts: the #289
// unit-count override (0 = auto).
int effectiveWidthOverride() {
  return unitWidthOverride.load(std::memory_order_relaxed);
}

bool tasksUnitCountOverridePinned() {
  return unitWidthOverride.load(std::memory_order_relaxed) > 0;
}

#include "HelpersSerialHandling.h"
#include "MqttService.h"
#include "OdometerLog.h"
#include "StatusLed.h"
#include "SystemStats.h"
#include "TaskWatchdog.h"
#include "WallJobs.h"
#include "WallLink.h"
#include "WallPair.h"
#include "WallWatch.h"
#include "WallShow.h"
#include "WebEndpoints.h"
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
// 4096 fared little better: 364 B HWM on BOTH wall masters (#434, first
// /system/stats hwm readout — deterministic, not noise), one deeper library
// path away from the #414 canary-panic class. 8192 matches the other domain tasks;
// static BSS, RAM is plentiful.
static constexpr uint32_t CLOCK_TASK_STACK = 8192;
// netTask is the heaviest domain task: wifi + web + flashLog + SSE +
// system-stats all share one loop. 4096 overflowed the
// canary on real hardware (split-flap-c8a746) inside flashLogTick's
// LittleFS.open → fopen → esp_flash_read, whose cross-core cache-disable
// IPC is the deepest chain netTask ever runs; the #294-era SSE/stats work
// raised the per-iteration floor until the fopen spike no longer fit. The
// heartbeat HWM column stays the trim-down evidence.
static constexpr uint32_t NET_TASK_STACK = 12288;
// espMqttClient internals + the 512 B discovery build buffers (#224); the
// heartbeat's HWM column is the trim-down evidence.
// 6144 overflowed on the first-ever broker connect + discovery burst (#479:
// coredump task=mqtt, excVaddr a5a5..; idle hwm was already down to 1768).
// Sizing policy since then (#480): >=50% margin at the measured peak — a
// never-exercised path can need far more than the busiest observed one.
static constexpr uint32_t MQTT_TASK_STACK = 16384;
// Everything that blocks on the network and so may not run on the link task
// or netTask: the pairing POST to a row board (HTTP client, 1.5 s timeouts)
// and the two liveness probes. Measured 8.8 KB at its peak.
static constexpr uint32_t WORKER_TASK_STACK = 16384;
// lwIP socket calls, one log line's vsnprintf, nanopb's decode, and the own
// row's service with its copy of the display snapshot; the message structs
// and the read buffer are static, not on this stack. Measured peak 4.2 KB
// with a row connected and the wall showing the clock (#566): sized for the
// >=50% margin of #480. /system/stats hwm.link is the evidence.
static constexpr uint32_t LINK_TASK_STACK = 12288;

static constexpr UBaseType_t DISPLAY_TASK_PRIORITY = 3;  // flap timing wins
static constexpr UBaseType_t DOMAIN_TASK_PRIORITY = 1;   // everything else

static constexpr BaseType_t DISPLAY_CORE = 1;  // with loopTask (heartbeat)
static constexpr BaseType_t NETWORK_CORE = 0;  // with WiFi/LWIP/AsyncTCP

static constexpr UBaseType_t DISPLAY_QUEUE_DEPTH = 16;
static constexpr UBaseType_t MQTT_INBOX_DEPTH = 8;

static StaticTask_t displayTaskBuf, clockTaskBuf, netTaskBuf, mqttTaskBuf,
    workerTaskBuf, linkTaskBuf;
static StackType_t displayTaskStack[DISPLAY_TASK_STACK];
static StackType_t clockTaskStack[CLOCK_TASK_STACK];
static StackType_t netTaskStack[NET_TASK_STACK];
static StackType_t mqttTaskStack[MQTT_TASK_STACK];
static StackType_t workerTaskStack[WORKER_TASK_STACK];
static StackType_t linkTaskStack[LINK_TASK_STACK];

static StaticQueue_t displayQueueBuf;
static uint8_t displayQueueStorage[DISPLAY_QUEUE_DEPTH * sizeof(DisplayCommand)];
QueueHandle_t displayQueue = nullptr;  // shared with DisplayTask.cpp (TasksInternal.h)

static StaticQueue_t mqttInboxBuf;
static uint8_t mqttInboxStorage[MQTT_INBOX_DEPTH * sizeof(MqttInboxMessage)];
static QueueHandle_t mqttInbox = nullptr;

static TaskHandle_t displayTaskHandle, clockTaskHandle, netTaskHandle,
    mqttTaskHandle, workerTaskHandle, linkTaskHandle;

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
    crashCtxHeartbeat();  // #504: the clock crash "ages" are measured against
    crashCtxMark(CRASH_SLOT_NET, CRASH_ACT_WIFI);
    wifiServiceTick();
    crashCtxMark(CRASH_SLOT_NET, CRASH_ACT_WEB_LOOP);
    webEndpointsLoop(*ctx->settings, *ctx->store);
    webDisplayEventsTick();  // #251: SSE push on display text change
    statusLedTick();
    systemStatsTick();  // #245/#251: self-throttled, 1 s fast + 5 s ring
    odometerLogTick();  // #465: self-throttled odometer historian append
    crashCtxMark(CRASH_SLOT_NET, CRASH_ACT_IDLE);
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
    crashCtxMark(CRASH_SLOT_MQTT, CRASH_ACT_MQTT);
    while (xQueueReceive(mqttInbox, &msg, 0) == pdTRUE) {
      mqttServiceHandleInbox(msg);
    }
    mqttServiceTick();
    crashCtxMark(CRASH_SLOT_MQTT, CRASH_ACT_IDLE);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Blocking network jobs (see WORKER_TASK_STACK): a row that does not answer
// stalls only this task, never the link or netTask.
static void workerTaskMain(void*) {
  SerialPrintf("workerTask up on core %d\n", xPortGetCoreID());
  if (esp_err_t e = wdtSubscribeSelf(); e != ESP_OK)
    SerialPrintf("wdt: worker subscribe -> %s\n", esp_err_to_name(e));
  for (;;) {
    wdtFeed();
    crashCtxMark(CRASH_SLOT_WORKER, CRASH_ACT_WORKER);
    netLivenessProbeTick();  // #501: the gateway and own-server probes
    wallPairTick();          // #566: pairing and the other rows-table requests
    wallOwnJobTick();        // #566: the end of a unit job on the own row
    wallWatchTick();         // #570: judge the wall, record what changed
    crashCtxMark(CRASH_SLOT_WORKER, CRASH_ACT_IDLE);
    vTaskDelay(pdMS_TO_TICKS(100));
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
  quietEnabled.store(settings.quiet, std::memory_order_relaxed);  // #227
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
  workerTaskHandle = xTaskCreateStaticPinnedToCore(
      workerTaskMain, "worker", WORKER_TASK_STACK, nullptr,
      DOMAIN_TASK_PRIORITY, workerTaskStack, &workerTaskBuf, NETWORK_CORE);
  linkTaskHandle = xTaskCreateStaticPinnedToCore(
      wallLinkTaskMain, "link", LINK_TASK_STACK, nullptr, DOMAIN_TASK_PRIORITY,
      linkTaskStack, &linkTaskBuf, NETWORK_CORE);
}

TasksStackHwm tasksStackHwm() {
  TasksStackHwm h;
  if (displayTaskHandle)
    h.display = (uint32_t)uxTaskGetStackHighWaterMark(displayTaskHandle);
  if (clockTaskHandle)
    h.clock = (uint32_t)uxTaskGetStackHighWaterMark(clockTaskHandle);
  if (netTaskHandle)
    h.net = (uint32_t)uxTaskGetStackHighWaterMark(netTaskHandle);
  if (mqttTaskHandle)
    h.mqtt = (uint32_t)uxTaskGetStackHighWaterMark(mqttTaskHandle);
  if (workerTaskHandle)
    h.worker = (uint32_t)uxTaskGetStackHighWaterMark(workerTaskHandle);
  if (linkTaskHandle)
    h.link = (uint32_t)uxTaskGetStackHighWaterMark(linkTaskHandle);
  return h;
}

void tasksHeartbeatReport() {
  Serial.printf(
      "[%8lu ms] heap %u KB free (min %u KB), psram %u KB free | stack HWM: "
      "display %u, clock %u, net %u, mqtt %u, worker %u, link %u, loop %u\n",
      (unsigned long)millis(), ESP.getFreeHeap() / 1024,
      ESP.getMinFreeHeap() / 1024, ESP.getFreePsram() / 1024,
      (unsigned)uxTaskGetStackHighWaterMark(displayTaskHandle),
      (unsigned)uxTaskGetStackHighWaterMark(clockTaskHandle),
      (unsigned)uxTaskGetStackHighWaterMark(netTaskHandle),
      (unsigned)uxTaskGetStackHighWaterMark(mqttTaskHandle),
      (unsigned)uxTaskGetStackHighWaterMark(workerTaskHandle),
      (unsigned)uxTaskGetStackHighWaterMark(linkTaskHandle),
      (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}
