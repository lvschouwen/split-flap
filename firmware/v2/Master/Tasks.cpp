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

void tasksSetUnitCountOverride(int count) {
  unitWidthOverride.store(count, std::memory_order_relaxed);
}

#include "BootHomePlan.h"
#include "ClockPolicy.h"
#include "ClusterFollower.h"
#include "ClusterLeader.h"
#include "FlapFrame.h"
#include "HeartbeatPolicy.h"
#include "HelpersSerialHandling.h"
#include "MqttService.h"
#include "StatusLed.h"
#include "SystemStats.h"
#include "TaskWatchdog.h"
#include "UnitBus.h"
#include "WebEndpoints.h"
#include "WifiService.h"

// --- static RTOS allocation (memory policy rule 1) ---------------------------
// Stacks and queue storage are static arrays in internal SRAM: deterministic
// footprint, visible in the map file, no heap fragmentation from task churn.
// Sizes are the spec's table; the heartbeat prints each task's high-water
// mark so the numbers get validated (and trimmed) on real hardware.

static constexpr uint32_t DISPLAY_TASK_STACK = 4096;
// 2048 leaves only ~124 B HWM on real hardware — newlib's first
// tzset/localtime parse of the POSIX TZ string runs deep in the ticker.
static constexpr uint32_t CLOCK_TASK_STACK = 4096;
// netTask is the heaviest domain task: wifi + web + flashLog + cluster
// follower + SSE + system-stats all share one loop. 4096 overflowed the
// canary on real hardware (split-flap-c8a746) inside flashLogTick's
// LittleFS.open → fopen → esp_flash_read, whose cross-core cache-disable
// IPC is the deepest chain netTask ever runs; the #294-era SSE/stats work
// raised the per-iteration floor until the fopen spike no longer fit. The
// heartbeat HWM column stays the trim-down evidence.
static constexpr uint32_t NET_TASK_STACK = 8192;
// espMqttClient internals + the 512 B discovery build buffers (#224); the
// heartbeat's HWM column is the trim-down evidence.
static constexpr uint32_t MQTT_TASK_STACK = 6144;
// esp_http_client + String assembly for the cluster fan-out (#273). The digest
// build puts a full ClusterLeaderStatus (8 members x 4 Strings) + String
// mirror[8] on the stack; at the config-apply/leading peak 6144 left only ~620 B
// HWM (bench, #321) — bumped to 8192 for margin (also covers the reboot-hold
// fan-out, which builds the same objects). RAM is plentiful (150 KB+ free heap).
static constexpr uint32_t CLUSTER_TASK_STACK = 8192;

static constexpr UBaseType_t DISPLAY_TASK_PRIORITY = 3;  // flap timing wins
static constexpr UBaseType_t DOMAIN_TASK_PRIORITY = 1;   // everything else

static constexpr BaseType_t DISPLAY_CORE = 1;  // with loopTask (heartbeat)
static constexpr BaseType_t NETWORK_CORE = 0;  // with WiFi/LWIP/AsyncTCP

static constexpr UBaseType_t DISPLAY_QUEUE_DEPTH = 16;
static constexpr UBaseType_t MQTT_INBOX_DEPTH = 8;

// Settle after an address-mutating burn before the follow-up probe (#204):
// the unit watchdog-resets THROUGH its twiboot window (~1 s) and probing
// inside it pins the bootloader (v1 #88) — 3 s clears the window plus the
// homing start, same margin class as the 1500 ms boot delay.
static constexpr uint32_t ADDRESS_OP_SETTLE_MS = 3000;

// Self-test wait (#265): the unit's diagnostic is ~2 revolutions at homing
// speed (~12-15 s) but can queue behind a slow in-flight move unit-side, so
// the window is generous — and it re-arms once RUNNING is first observed,
// so the true worst-case displayTask block is ~2x this constant. Polled at
// 500 ms; three consecutive invalid replies with none ever valid =
// firmware predating the opcode.
static constexpr uint32_t SELF_TEST_TIMEOUT_MS = 45000;
static constexpr uint32_t SELF_TEST_POLL_MS = 500;
static constexpr int SELF_TEST_UNSUPPORTED_POLLS = 3;

static StaticTask_t displayTaskBuf, clockTaskBuf, netTaskBuf, mqttTaskBuf,
    clusterTaskBuf;
static StackType_t displayTaskStack[DISPLAY_TASK_STACK];
static StackType_t clockTaskStack[CLOCK_TASK_STACK];
static StackType_t netTaskStack[NET_TASK_STACK];
static StackType_t mqttTaskStack[MQTT_TASK_STACK];
static StackType_t clusterTaskStack[CLUSTER_TASK_STACK];

static StaticQueue_t displayQueueBuf;
static uint8_t displayQueueStorage[DISPLAY_QUEUE_DEPTH * sizeof(DisplayCommand)];
static QueueHandle_t displayQueue = nullptr;

static StaticQueue_t mqttInboxBuf;
static uint8_t mqttInboxStorage[MQTT_INBOX_DEPTH * sizeof(MqttInboxMessage)];
static QueueHandle_t mqttInbox = nullptr;

static TaskHandle_t displayTaskHandle, clockTaskHandle, netTaskHandle,
    mqttTaskHandle, clusterTaskHandle;

// --- display snapshot (single writer: displayTask) ---------------------------

static DisplaySnapshot snapshot;
static SemaphoreHandle_t snapshotMutex = nullptr;

static void snapshotPublish(const DisplaySnapshot& next) {
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

// --- core 1: display domain ---------------------------------------------------

// Exclusive owner of I2C/Wire (via UnitBus, #203). Boot: bus up → twiboot
// settle → probe → health poll → publish. Loop: pure transition
// (displayApplyCommand) + the hardware work per opcode. Blocking bus
// operations run right here by design — commands queue behind them, and
// the published busy flag covers the whole execution.
// Probe inhibit after any op that reboots a unit THROUGH its twiboot window
// (v1 #88 hard rule: the probe's CHIPINFO query pins twiboot alive). Owned
// by displayTask exclusively; every runtime probe waits this deadline out —
// including a Probe that was already queued behind a /unit/reboot.
static uint32_t twibootRiskUntilMs = 0;

static void armTwibootRiskWindow() {
  twibootRiskUntilMs = millis() + ADDRESS_OP_SETTLE_MS;
}

static void settleBeforeProbe() {
  int32_t remaining = (int32_t)(twibootRiskUntilMs - millis());
  if (remaining > 0) delay((uint32_t)remaining);
}

// --- heartbeat freshness + batched boot-home (#309/#310) ---------------------

// A full health poll + a freshness stamp for every slot (#310, HeartbeatPolicy).
// Used by boot, the explicit Probe and every post-op reprobe so a refresh
// resets the miss counters and makes /units/health "age" truthful immediately
// after. The read outcome per slot is its statusValid (set by unitBusPollHealth).
static void pollHealthWithFreshness(UnitFacts* busFacts) {
  unitBusPollHealth(busFacts, UNITS_AMOUNT);
  uint32_t now = millis();
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    heartbeatApply(busFacts[i], busFacts[i].statusValid, now,
                   HEARTBEAT_MISS_THRESHOLD);
  }
}

// Batched boot-home (#309). The units boot UNHOMED; home the ones that still
// report unhomed in bounded batches with a rail-settle between them, so a
// whole row's steppers don't spike the shared rail at once (the #305
// verify-boot brownout). Targets only unhomed sketch units, so it serves both
// a cold boot (home all) and a post-reflash top-up (home just the flashed
// units) without re-homing good ones. Status-driven waits (homed-or-faulted)
// come from unitBusWaitBatchIdle; abort (/stop) bails between batches.
static void runBootHomeSequence(DisplaySnapshot& local, UnitFacts* busFacts) {
  uint8_t targets[UNITS_AMOUNT];
  int n = bootHomeCollectTargets(local.units, local.displayWidth,
                                 SFP_I2C_ADDRESS_BASE, targets);
  if (n == 0) return;
  SerialPrintf("boot-home: staggering %d unit(s) in batches of %d\n", n,
               BOOT_HOME_BATCH_SIZE);
  for (int i = 0; i < n; i += BOOT_HOME_BATCH_SIZE) {
    wdtFeed();  // #314: each batch waits on unit settle — keep the dog fed
    if (unitBusAbortRequested()) break;
    uint8_t batch[BOOT_HOME_BATCH_SIZE];
    int batchN = 0;
    for (int j = i; j < n && batchN < BOOT_HOME_BATCH_SIZE; j++) {
      unitBusHome(targets[j]);
      batch[batchN++] = targets[j];
    }
    // Status-driven: wait for each commanded unit to report homed-or-faulted
    // (not moving) before settling the rail for the next batch.
    unitBusWaitBatchIdle(batch, batchN, BOOT_HOME_BATCH_TIMEOUT_MS);
    delay(BOOT_HOME_SETTLE_MS);
  }
  // Re-poll so the published snapshot reflects the now-homed state.
  pollHealthWithFreshness(busFacts);
  displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                        unitWidthOverride.load(std::memory_order_relaxed));
  snapshotPublish(local);
}

// One opportunistic heartbeat read (#310), synthesized by displayTask only on
// an idle tick — display writes / reflash / an explicit Probe always preempt
// (they arrive as commands). Round-robins one unit per tick; skipped entirely
// while a unit may be in its twiboot window (v1 #88: a status read pins the
// bootloader alive).
static void heartbeatTick(DisplaySnapshot& local, UnitFacts* busFacts,
                          int& slot) {
  int width = local.displayWidth;
  if (width <= 0) return;
  if ((int32_t)(twibootRiskUntilMs - millis()) > 0) return;
  int i = slot;
  slot = heartbeatNextSlot(slot, width);
  bool ok = unitBusPollHealthOne(busFacts, i);
  heartbeatApply(busFacts[i], ok, millis(), HEARTBEAT_MISS_THRESHOLD);
  displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                        unitWidthOverride.load(std::memory_order_relaxed));
  snapshotPublish(local);
}

// --- unit reflash job (#205) ---------------------------------------------------
// The one long-running job in the firmware, run INLINE by displayTask (the
// design's approach A: bus exclusivity stays structural, commands queue
// behind it — except nothing queues, because the reflashActive gate turns
// producers away at their boundaries while this runs).
//
// The job's internal probes deliberately BYPASS settleBeforeProbe(): the
// v1 #88 hazard is probing a twiboot unit *without* flashing it (the
// CHIPINFO query pins the bootloader alive forever); here every pinned
// unit is immediately flashed and exited, and a FAILED unit is left
// pinned on purpose — it must not jump to a torn sketch, and the pin
// keeps it reachable for the retry.

// Which sketch units the pre-flash reboot sweep targets.
enum class ReflashSweep : uint8_t {
  OffBundle,     // web job: outdated + unknown revs (v1 #114 semantics)
  OutdatedOnly,  // boot auto-update: only provably stale revs
};

// True when a job would have work: something sits in twiboot already, or
// the sweep predicate matches a sketch unit. Boot uses this to skip the
// whole job (and its progress churn) on a healthy display.
static bool reflashHasWork(const DisplaySnapshot& snap, ReflashSweep sweep) {
  uint8_t addrs[UNITS_AMOUNT];
  if (reflashCollectFlashTargets(snap.units, UNITS_AMOUNT,
                                 SFP_I2C_ADDRESS_BASE, addrs) > 0) {
    return true;
  }
  int n = sweep == ReflashSweep::OffBundle
              ? reflashCollectRebootTargets(snap.units, UNITS_AMOUNT,
                                            SFP_I2C_ADDRESS_BASE, addrs)
              : reflashCollectOutdatedTargets(snap.units, UNITS_AMOUNT,
                                              SFP_I2C_ADDRESS_BASE, addrs);
  return n > 0;
}

static void runReflashJob(DisplaySnapshot& local, UnitFacts* busFacts,
                          ReflashSweep sweep) {
  reflashProgressBegin(local.reflash, 0);  // total known after the rescan
  snapshotPublish(local);                  // gate closes here

  // With the gate closed, drain whatever slipped into the queue earlier —
  // it would burst-drain onto a display this job is about to rebuild.
  // Stop survives (it is the cancel; its abort flag is already set — the
  // #204 order rule) but is re-sent only AFTER the drain finishes: a
  // mid-drain re-send would leave commands that sat behind the Stop
  // running ahead of it after a cancelled job (Codex review finding).
  // Multiple Stops collapse into one — supersession is the #204 contract.
  DisplayCommand stale;
  bool sawStop = false;
  DisplayCommand stopCmd;
  while (xQueueReceive(displayQueue, &stale, 0) == pdTRUE) {
    if (stale.opcode == DisplayOpcode::Stop) {
      sawStop = true;
      stopCmd = stale;
      continue;
    }
    SerialPrintln("display: dropped queued command at reflash start: " +
                  describeDisplayCommand(stale));
  }
  if (sawStop) xQueueSend(displayQueue, &stopCmd, 0);

  // Push sweep-matching sketch units into twiboot (v1's
  // enterBootloaderAllDetected), then wait out the watchdog reset +
  // twiboot init before talking to anyone.
  uint8_t addrs[UNITS_AMOUNT];
  int rebooted = 0;
  int sweepCount =
      sweep == ReflashSweep::OffBundle
          ? reflashCollectRebootTargets(local.units, UNITS_AMOUNT,
                                        SFP_I2C_ADDRESS_BASE, addrs)
          : reflashCollectOutdatedTargets(local.units, UNITS_AMOUNT,
                                          SFP_I2C_ADDRESS_BASE, addrs);
  for (int i = 0; i < sweepCount; i++) {
    if (unitBusRebootToBootloader(addrs[i]) == 0) {
      displayInvalidateUnitReads(local, addrs[i]);
      rebooted++;
    }
  }
  if (rebooted > 0) {
    SerialPrintf("reflash: sent %d unit(s) into the bootloader\n", rebooted);
    delay(TWIBOOT_STARTUP_MS);
  }

  // Rescan (inhibit bypassed by design, see block comment) to see who
  // actually sits in twiboot, then plan the flash list from live truth.
  unitBusProbe(busFacts, UNITS_AMOUNT);
  displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                        unitWidthOverride.load(std::memory_order_relaxed));
  uint8_t targets[UNITS_AMOUNT];
  int total = reflashCollectFlashTargets(local.units, UNITS_AMOUNT,
                                         SFP_I2C_ADDRESS_BASE, targets);
  local.reflash.total = (uint8_t)total;
  snapshotPublish(local);
  SerialPrintf("reflash: %d unit(s) to flash\n", total);

  const uint8_t* image = webUnitFirmwareBin();
  size_t imageLen = webUnitFirmwareBinLen();
  bool cancelled = false;
  uint8_t batch[REFLASH_BATCH_SIZE];
  int inBatch = 0;
  for (int k = 0; k < total; k++) {
    wdtFeed();  // #314: I2C page-streaming is the longest displayTask op
    if (unitBusAbortRequested()) {
      cancelled = true;
      break;
    }
    uint8_t addr = targets[k];
    reflashProgressUnitStart(local.reflash, addr);
    snapshotPublish(local);

    UnitFlashResult r = unitBusFlashUnit(addr, image, imageLen);
    if (r == UnitFlashResult::Aborted) {
      reflashProgressUnitResult(local.reflash, false);
      snapshotPublish(local);
      cancelled = true;
      break;
    }
    bool ok = (r == UnitFlashResult::Ok);
    if (!ok) {
      SerialPrintf("reflash: unit 0x%02x failed (%s)\n", addr,
                   unitFlashResultName(r));
    }
    reflashProgressUnitResult(local.reflash, ok);
    if (ok) {
      // Just-flashed unit runs the sketch again; bump the fact so the
      // batch-idle wait below polls it (the final reprobe rewrites all
      // facts wholesale anyway).
      local.units[addr - SFP_I2C_ADDRESS_BASE].state = 1;
      batch[inBatch++] = addr;
    }
    snapshotPublish(local);

    // v1 #138 brownout throttle: once a batch is full, wait for those
    // units to come back online + finish homing before flashing more.
    if (inBatch >= REFLASH_BATCH_SIZE) {
      reflashProgressSettling(local.reflash);
      snapshotPublish(local);
      unitBusWaitBatchIdle(batch, inBatch, REFLASH_BATCH_SETTLE_MS);
      inBatch = 0;
    }
  }
  // Trailing partial batch — reached on plan exhaustion AND on both abort
  // exits (cpp-review HIGH): the settle is brownout pacing and is never
  // abort-shortened, so even a cancelled job waits out the homing of the
  // units it already flashed before the queued Stop broadcast-homes.
  if (inBatch > 0) {
    reflashProgressSettling(local.reflash);
    snapshotPublish(local);
    unitBusWaitBatchIdle(batch, inBatch, REFLASH_BATCH_SETTLE_MS);
  }

  // Final reprobe + health poll: published topology and fw grades are
  // execution-time truth (a failed/cancelled unit shows as bootloader and
  // stays pinned there — deliberate, see block comment).
  unitBusProbe(busFacts, UNITS_AMOUNT);
  pollHealthWithFreshness(busFacts);
  displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                        unitWidthOverride.load(std::memory_order_relaxed));
  // Staggered boot-home of the just-flashed units (#309): a reflashed unit
  // reboots UNHOMED, so without this the caller's re-show (or the next cluster
  // render) would home every flashed unit at once — the #305 inrush #309
  // exists to prevent. Targets only the still-unhomed units; a cancel leaves
  // the abort flag set so this bails and the queued Stop broadcast-homes.
  wdtFeed();  // #314: boot-home of just-flashed units
  runBootHomeSequence(local, busFacts);
  reflashProgressFinish(local.reflash, cancelled);
  snapshotPublish(local);  // gate reopens here
  SerialPrintf("reflash: %s — %u ok, %u failed of %u\n",
               reflashStateName(local.reflash.state),
               (unsigned)local.reflash.done, (unsigned)local.reflash.failed,
               (unsigned)local.reflash.total);
}

static void displayTaskMain(void*) {
  SerialPrintf("displayTask up on core %d\n", xPortGetCoreID());
  DisplaySnapshot local;  // task-private working state; published as copies
  // Static: ~400 B that would otherwise sit on the task stack forever.
  static UnitFacts busFacts[UNITS_AMOUNT];

  unitBusInit();
  // Load-bearing pre-probe delay (v1 #88): probing earlier catches units
  // still in twiboot's boot window and the CHIPINFO read pins them there.
  delay(1500);
  unitBusProbe(busFacts, UNITS_AMOUNT);
  pollHealthWithFreshness(busFacts);
  displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                        unitWidthOverride.load(std::memory_order_relaxed));
  snapshotPublish(local);
  if (local.detectedUnitCount == 0) {
    SerialPrintf("display: no units responding — assuming full width %d\n",
                 local.displayWidth);
  } else {
    SerialPrintf("display: probe done, width %d\n", local.displayWidth);
  }
  if (unitWidthOverride.load(std::memory_order_relaxed) > 0) {
    SerialPrintf("display: width pinned to %d (unit-count override)\n",
                 local.displayWidth);
  }

  // Boot auto-install + auto-update (#205, full v1 parity): flash any unit
  // the probe found sitting in twiboot (the recovery path for a failed or
  // cancelled flash), and push provably-outdated sketch units through the
  // same job. Runs before the command loop, but the WiFi join is already
  // racing on core 0 — the job's reflashActive gate turns away whatever
  // comes up mid-install (web 409s, clock skips).
  // Staggered boot-home (#309): the units boot UNHOMED, so the master
  // orchestrates the homing inrush in bounded batches instead of letting the
  // whole row's steppers spike the shared rail at once (the #305 verify-boot
  // brownout). runReflashJob ends with its own boot-home of the units it
  // flashed, so only home here when no boot reflash ran.
  if (reflashHasWork(local, ReflashSweep::OutdatedOnly)) {
    SerialPrintln(F("reflash: boot auto-install/auto-update starting"));
    runReflashJob(local, busFacts, ReflashSweep::OutdatedOnly);
  } else {
    runBootHomeSequence(local, busFacts);
  }

  DisplayCommand cmd;
  int heartbeatSlot = 0;  // round-robin cursor for the scheduled poll (#310)
  if (esp_err_t e = wdtSubscribeSelf(); e != ESP_OK)
    SerialPrintf("wdt: display subscribe -> %s\n", esp_err_to_name(e));
  for (;;) {
    wdtFeed();
    // Timed wait: a real command preempts (display writes / reflash / Probe);
    // an idle timeout synthesizes one opportunistic heartbeat read.
    if (xQueueReceive(displayQueue, &cmd,
                      pdMS_TO_TICKS(HEARTBEAT_TICK_MS)) != pdTRUE) {
      heartbeatTick(local, busFacts, heartbeatSlot);
      continue;
    }
    local.busy = true;
    snapshotPublish(local);
    if (displayApplyCommand(local, cmd)) {
      SerialPrintln("display: " + describeDisplayCommand(cmd));
      switch (cmd.opcode) {
        case DisplayOpcode::ShowText: {
          uint8_t letters[UNITS_AMOUNT];
          flapFrameBuild(cmd.text, local.displayWidth, cmd.alignment,
                         letters);
          int errs = unitBusShowFrame(local.units, local.displayWidth,
                                      letters, convertSpeedToUnit(cmd.speed));
          // v1's lastShowUnitWriteErrors — the MQTT unitErrors telemetry
          // input (#224).
          local.lastShowWriteErrors = errs > 0 ? (uint8_t)errs : 0;
          // The "intended" side of the displayed==intended check (#264).
          memcpy(local.lastFrameLetters, letters, sizeof(letters));
          local.lastFrameValid = true;
          break;
        }
        case DisplayOpcode::Probe:
          // Re-scan + health refresh: an address change moves a unit to a
          // slot only a probe can see (v1 #56 semantics). A refresh queued
          // right behind a /unit/reboot must not scan into the twiboot
          // window (review 2026-07-11) — wait the risk deadline out first.
          settleBeforeProbe();
          unitBusProbe(busFacts, UNITS_AMOUNT);
          pollHealthWithFreshness(busFacts);
          displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                        unitWidthOverride.load(std::memory_order_relaxed));
          break;
        // --- calibration + provisioning (#204). Every op grades a
        // MaintResult; the web layer serves it via /unit/op-result.
        case DisplayOpcode::WriteOffset: {
          int status = unitBusWriteOffset(cmd.unitAddress, cmd.value);
          if (status == 0) {
            // The only in-place offset mutation — probes own everything else.
            displayApplyOffsetWrite(local, cmd.unitAddress, cmd.value);
          }
          displayApplyMaintResult(
              local, cmd,
              status == 0 ? MaintOutcome::Ok : MaintOutcome::WireFail,
              MaintReason::None);
          break;
        }
        case DisplayOpcode::Jog: {
          int status = unitBusJog(cmd.unitAddress, cmd.value);
          displayApplyMaintResult(
              local, cmd,
              status == 0 ? MaintOutcome::Ok : MaintOutcome::WireFail,
              MaintReason::None);
          break;
        }
        case DisplayOpcode::Home: {
          int status = unitBusHome(cmd.unitAddress);
          if (status == 0 && local.lastFrameValid) {
            // The unit parks at blank — keep the intended frame truthful so
            // the #264 mismatch check doesn't flag the deliberate home.
            int idx = cmd.unitAddress - SFP_I2C_ADDRESS_BASE;
            if (idx >= 0 && idx < UNITS_AMOUNT) {
              local.lastFrameLetters[idx] = 0;
            }
          }
          displayApplyMaintResult(
              local, cmd,
              status == 0 ? MaintOutcome::Ok : MaintOutcome::WireFail,
              MaintReason::None);
          break;
        }
        case DisplayOpcode::Identify: {
          int status = unitBusIdentify(cmd.unitAddress);
          displayApplyMaintResult(
              local, cmd,
              status == 0 ? MaintOutcome::Ok : MaintOutcome::WireFail,
              MaintReason::None);
          break;
        }
        case DisplayOpcode::SelfTest: {
          // On-demand diagnostic revolution (#265): start, then poll the
          // unit's result until a terminal state. A stale terminal from a
          // PREVIOUS test can still sit in the unit's reply buffer while
          // this one queues behind an in-flight move — only accept a
          // terminal state after RUNNING has been observed.
          SelfTestSlot slot;
          slot.seq = cmd.seq;
          slot.addr = cmd.unitAddress;
          int status = unitBusStartSelfTest(cmd.unitAddress);
          if (status != 0) {
            slot.outcome = SelfTestOutcome::WireFail;
          } else {
            slot.outcome = SelfTestOutcome::Timeout;
            bool sawRunning = false;
            bool everValid = false;
            bool haveBaseline = false;
            UnitSelfTestReading baseline{};
            int badPolls = 0;
            uint32_t start = millis();
            while (millis() - start < SELF_TEST_TIMEOUT_MS) {
              wdtFeed();  // #314: self-test polls the unit until it reports an outcome
              if (unitBusAbortRequested()) {
                slot.outcome = SelfTestOutcome::Aborted;
                break;
              }
              delay(SELF_TEST_POLL_MS);
              UnitSelfTestReading r;
              if (!unitBusReadSelfTest(cmd.unitAddress, r)) {
                if (!everValid && ++badPolls >= SELF_TEST_UNSUPPORTED_POLLS) {
                  slot.outcome = SelfTestOutcome::Unsupported;
                  break;
                }
                continue;
              }
              everValid = true;
              if (!haveBaseline) {
                // First valid reading = the pre-test buffer content. A later
                // terminal that DIFFERS from it is provably fresh even when
                // every poll of the RUNNING window was lost to bus glitches
                // (codex review).
                haveBaseline = true;
                baseline = r;
              }
              if (r.state == 1) {  // running
                if (!sawRunning) {
                  // The test provably started — re-arm the window so time the
                  // unit spent finishing a prior move doesn't eat the test's
                  // own budget (codex review).
                  sawRunning = true;
                  start = millis();
                }
                continue;
              }
              bool freshTerminal =
                  sawRunning ||
                  (haveBaseline &&
                   (r.state != baseline.state ||
                    r.stepsPerRev != baseline.stepsPerRev ||
                    r.hallWindowSteps != baseline.hallWindowSteps ||
                    r.revTimeMs != baseline.revTimeMs));
              if (r.state == 0 || !freshTerminal) continue;  // not started / stale
              if (r.state == 2) {
                slot.outcome = SelfTestOutcome::Ok;
                slot.stepsPerRev = r.stepsPerRev;
                slot.hallWindowSteps = r.hallWindowSteps;
                slot.revTimeMs = r.revTimeMs;
              } else {
                slot.outcome = SelfTestOutcome::UnitFailed;
              }
              break;
            }
          }
          SerialPrintf("display: self-test unit 0x%02x → %s\n",
                       cmd.unitAddress, selfTestOutcomeName(slot.outcome));
          displayApplySelfTestResult(local, slot);
          displayApplyMaintResult(local, cmd,
                                  slot.outcome == SelfTestOutcome::Ok
                                      ? MaintOutcome::Ok
                                      : MaintOutcome::PostconditionFail,
                                  MaintReason::None);
          break;
        }
        case DisplayOpcode::ResetOdometer: {
          int status = unitBusResetOdometer(cmd.unitAddress);
          if (status == 0) {
            // Patch the fact in place like a successful offset write —
            // the wear view must not show the stale count until the next
            // probe (#231).
            displayApplyOdometerReset(local, cmd.unitAddress);
          }
          displayApplyMaintResult(
              local, cmd,
              status == 0 ? MaintOutcome::Ok : MaintOutcome::WireFail,
              MaintReason::None);
          break;
        }
        case DisplayOpcode::RebootToBootloader: {
          // NO follow-up probe (v1 #88 hard rule): the unit sits in twiboot
          // ~1 s and the probe's CHIPINFO query would pin it there forever.
          // Reads for this unit are invalidated until the next probe.
          int status = unitBusRebootToBootloader(cmd.unitAddress);
          if (status == 0) {
            displayInvalidateUnitReads(local, cmd.unitAddress);
            armTwibootRiskWindow();
          }
          displayApplyMaintResult(
              local, cmd,
              status == 0 ? MaintOutcome::Ok : MaintOutcome::WireFail,
              MaintReason::None);
          break;
        }
        case DisplayOpcode::SetAddress: {
          // Execution-time recheck against LIVE facts: the web handler
          // validated a snapshot copy that the queue delay made stale.
          MaintVerdict verdict = maintValidateSetAddressTarget(
              cmd.value, cmd.unitAddress, local.units, UNITS_AMOUNT);
          if (verdict.httpStatus != 200) {
            displayApplyMaintResult(
                local, cmd, MaintOutcome::ExecValidationFail,
                verdict.httpStatus == 409 ? MaintReason::TargetAddressOccupied
                                          : MaintReason::None);
            break;
          }
          int status = unitBusSetAddress(cmd.unitAddress, (uint8_t)cmd.value);
          if (status != 0) {
            displayApplyMaintResult(local, cmd, MaintOutcome::WireFail,
                                    MaintReason::None);
            break;
          }
          // Compound op: settle THROUGH the unit's reboot + twiboot window
          // (probing inside it pins twiboot — v1 #88), then reprobe so the
          // published topology is execution-time truth, not a UI timer race.
          // NOT abort-shortened: the settle is bus safety, not pacing.
          armTwibootRiskWindow();
          settleBeforeProbe();
          unitBusProbe(busFacts, UNITS_AMOUNT);
          pollHealthWithFreshness(busFacts);
          displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                        unitWidthOverride.load(std::memory_order_relaxed));
          MaintReason reason = MaintReason::None;
          MaintOutcome outcome = classifySetAddressOutcome(
              local.units, UNITS_AMOUNT, cmd.value, reason);
          displayApplyMaintResult(local, cmd, outcome, reason);
          break;
        }
        case DisplayOpcode::ClearAddress: {
          int countBefore = local.detectedUnitCount;
          int status = unitBusClearAddress(cmd.unitAddress);
          if (status != 0) {
            displayApplyMaintResult(local, cmd, MaintOutcome::WireFail,
                                    MaintReason::None);
            break;
          }
          armTwibootRiskWindow();
          settleBeforeProbe();
          unitBusProbe(busFacts, UNITS_AMOUNT);
          pollHealthWithFreshness(busFacts);
          displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                        unitWidthOverride.load(std::memory_order_relaxed));
          MaintReason reason = MaintReason::None;
          MaintOutcome outcome = classifyClearAddressOutcome(
              countBefore, local.detectedUnitCount, reason);
          displayApplyMaintResult(local, cmd, outcome, reason);
          break;
        }
        case DisplayOpcode::ResetUnits: {
          // v1 blank-out sequence: a full row of '-', 2 s, a full row of
          // '.' — the wrap-around forces each unit's recalibration — then
          // re-show the text baked at enqueue time.
          uint8_t letters[UNITS_AMOUNT];
          char row[UNITS_AMOUNT + 1];
          int unitSpeed = convertSpeedToUnit(cmd.speed);
          memset(row, '-', local.displayWidth);
          row[local.displayWidth] = '\0';
          flapFrameBuild(row, local.displayWidth, DisplayAlignment::Left,
                         letters);
          unitBusShowFrame(local.units, local.displayWidth, letters,
                           unitSpeed);
          delay(2000);
          memset(row, '.', local.displayWidth);
          row[local.displayWidth] = '\0';
          flapFrameBuild(row, local.displayWidth, DisplayAlignment::Left,
                         letters);
          unitBusShowFrame(local.units, local.displayWidth, letters,
                           unitSpeed);
          flapFrameBuild(cmd.text, local.displayWidth, cmd.alignment,
                         letters);
          unitBusShowFrame(local.units, local.displayWidth, letters,
                           unitSpeed);
          memcpy(local.lastFrameLetters, letters, sizeof(letters));  // #264
          local.lastFrameValid = true;
          displayApplyMaintResult(local, cmd, MaintOutcome::Ok,
                                  MaintReason::None);
          break;
        }
        case DisplayOpcode::Stop: {
          // The abort flag (set by the /stop handler at enqueue) already
          // short-circuited every wait ahead of us; now park the display.
          int status = unitBusBroadcastHome();
          if (status == 0) {
            // Every unit parks at blank — the intended frame follows (#264).
            memset(local.lastFrameLetters, 0, sizeof(local.lastFrameLetters));
            local.lastFrameValid = true;
          }
          unitBusClearAbort();
          displayApplyMaintResult(
              local, cmd,
              status == 0 ? MaintOutcome::Ok : MaintOutcome::WireFail,
              MaintReason::None);
          break;
        }
        case DisplayOpcode::ReflashUnits: {
          // The job closes the gate, drains queue stragglers (Stop
          // survives), flashes in batches, and reprobes — see runReflashJob.
          runReflashJob(local, busFacts, ReflashSweep::OffBundle);

          // Baked re-show: reflashed units homed to blank — put the
          // enqueue-time content back. Skipped on cancel: the queued Stop
          // right behind us broadcast-homes and clears the text anyway.
          if (local.reflash.state != ReflashState::Cancelled) {
            uint8_t letters[UNITS_AMOUNT];
            flapFrameBuild(cmd.text, local.displayWidth, cmd.alignment,
                           letters);
            unitBusShowFrame(local.units, local.displayWidth, letters,
                             convertSpeedToUnit(cmd.speed));
            memcpy(local.currentText, cmd.text, sizeof(local.currentText));
            memcpy(local.lastFrameLetters, letters, sizeof(letters));  // #264
            local.lastFrameValid = true;
          }
          MaintReason reason = MaintReason::None;
          MaintOutcome outcome = classifyReflashOutcome(local.reflash, reason);
          displayApplyMaintResult(local, cmd, outcome, reason);
          break;
        }
        default:
          break;
      }
    } else {
      SerialPrintln("display: dropped un-executable command");
    }
    local.busy = false;
    snapshotPublish(local);
  }
}

// 1 Hz mode ticker (#192): re-shows the active mode's content — clock time
// or the retained message — whenever the display drifts away from it (mode
// switches, drain messages, minute rollover). The whole decision is the
// pure decideClockTick(); this loop only gathers snapshots and enqueues.
static void clockTaskMain(void*) {
  SerialPrintf("clockTask up on core %d\n", xPortGetCoreID());
  TickType_t lastWake = xTaskGetTickCount();
  String lastQueued;  // in-flight dedup, see ClockPolicy.h contract
  if (esp_err_t e = wdtSubscribeSelf(); e != ESP_OK)
    SerialPrintf("wdt: clock subscribe -> %s\n", esp_err_to_name(e));
  for (;;) {
    wdtFeed();
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1000));

    DisplaySnapshot snap = displaySnapshotGet();
    // Producer gate (#205): while a reflash runs, skip the whole tick —
    // nothing queues, nothing burst-drains after, and lastQueued stays
    // untouched so the first post-job tick re-sends fresh content.
    if (reflashInProgress(snap.reflash)) continue;
    // Notification gate (#224): while an MQTT notification owns the
    // display, don't tick over it — the overlay's expiry releases this
    // gate and the next tick re-shows the active mode's content (v1
    // gated loop()'s mode block via mqttNotificationTick()). On the
    // falling edge, drop the dedup marker: it was frozen at the pre-
    // notification text, which is exactly the revert target — a stale
    // match would block the revert forever (cpp-review HIGH).
    static bool notifWasActive = false;
    if (mqttNotificationActive()) {
      notifWasActive = true;
      continue;
    }
    if (notifWasActive) {
      notifWasActive = false;
      lastQueued = "";
    }
    // Leader reroute (#273): with the cluster enabled the ticker's product
    // becomes LOGICAL grid content handed to the cluster layer — which
    // dedups, slices, and stages this master's own row on the shared
    // commitAt clock — so nothing enqueues from here. Overlays still win:
    // the gates above run first, and the self-row re-show after an overlay
    // belongs to clusterTask.
    if (clusterLeaderEnabled()) {
      WebContentSnapshot leaderContent = webDisplayContentSnapshot();
      time_t leaderNow = time(nullptr);
      if (leaderContent.deviceMode == "clock") {
        // Un-synced clock holds (v1 deviation, same as decideClockTick).
        if (clockIsTimeSynced(leaderNow)) {
          clusterLeaderSubmitClock(
              formatDateTime(leaderNow, CLOCK_FORMAT),
              formatDateTime(leaderNow, CLUSTER_DATE_FORMAT),
              leaderContent.alignment, leaderContent.flapSpeed);
        }
      } else if (leaderContent.deviceMode == "text" &&
                 leaderContent.inputText.length() > 0) {
        clusterLeaderSubmitText(leaderContent.inputText,
                                leaderContent.alignment,
                                leaderContent.flapSpeed);
      }
      lastQueued = "";  // the ticker owns nothing while leading
      continue;
    }

    clockTickObserve(lastQueued, String(snap.currentText));

    WebContentSnapshot content = webDisplayContentSnapshot();
    time_t now = time(nullptr);

    // Cluster gate (#272): while this board is a cluster member the leader
    // owns the content — the ticker's job becomes re-showing the held
    // segment (restores the wall after transients and reset-units). While a
    // commitAt render is in flight it stands down entirely so a re-show
    // can't preempt the synchronized flip. LocalFallback (leader silent ~2
    // min) shows the follower's OWN clock through the normal clock path.
    ClusterFollowerView cluster = clusterFollowerViewGet();
    if (cluster.gated && cluster.renderPending) continue;

    ClockTickInput in;
    if (cluster.gated && !cluster.forcesLocalClock) {
      in.deviceMode = "text";
      in.inputText = cluster.heldSegment;  // "" until a render arrives → no-op
    } else if (cluster.gated) {
      in.deviceMode = "clock";
    } else {
      in.deviceMode = content.deviceMode;
      in.inputText = content.inputText;
    }
    in.timeSynced = clockIsTimeSynced(now);
    in.formattedTime = in.timeSynced ? formatDateTime(now, CLOCK_FORMAT) : "";
    in.displayBusy = snap.busy;
    in.displayCurrentText = String(snap.currentText);
    in.lastQueued = lastQueued;

    ClockTickDecision d = decideClockTick(in);
    if (d.enqueue) {
      // Segment re-shows are pre-positioned by the leader: rendered Left at
      // the speed the render arrived with, like the original enqueue.
      bool segmentReshow = cluster.gated && !cluster.forcesLocalClock;
      DisplayCommand cmd =
          segmentReshow
              ? makeShowTextCommand(d.text, "left", cluster.heldSpeed)
              : makeShowTextCommand(d.text, content.alignment,
                                    content.flapSpeed);
      if (displayEnqueue(cmd)) {
        lastQueued = d.text;
      }
      // Queue full: dedup state unchanged, the next tick retries.
    }
  }
}

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
    webEndpointsLoop(*ctx->settings, *ctx->store);
    clusterFollowerServiceTick(*ctx->store);  // #272: decay + NVS + renders
    webDisplayEventsTick();  // #251: SSE push on display text change
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

// Leader-side cluster fan-out (#273): ALL outbound cluster HTTP lives in
// this task (esp_http_client, 1.5 s timeouts) — a dead follower stalls
// only the fan-out, never netTask. The body is clusterLeaderTick()
// (ClusterLeader.cpp); disabled clusters make it a no-op read.
static void clusterTaskMain(void*) {
  SerialPrintf("clusterTask up on core %d\n", xPortGetCoreID());
  if (esp_err_t e = wdtSubscribeSelf(); e != ESP_OK)
    SerialPrintf("wdt: cluster subscribe -> %s\n", esp_err_to_name(e));
  for (;;) {
    wdtFeed();
    clusterLeaderTick();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// --- lifecycle -----------------------------------------------------------------

void tasksInit(MasterSettings& settings, SettingsStore& store) {
  unitWidthOverride.store(settings.unitCountOverride,
                          std::memory_order_relaxed);
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
  clusterTaskHandle = xTaskCreateStaticPinnedToCore(
      clusterTaskMain, "cluster", CLUSTER_TASK_STACK, nullptr,
      DOMAIN_TASK_PRIORITY, clusterTaskStack, &clusterTaskBuf, NETWORK_CORE);
}

void tasksHeartbeatReport() {
  Serial.printf(
      "[%8lu ms] heap %u KB free (min %u KB), psram %u KB free | stack HWM: "
      "display %u, clock %u, net %u, mqtt %u, cluster %u, loop %u\n",
      (unsigned long)millis(), ESP.getFreeHeap() / 1024,
      ESP.getMinFreeHeap() / 1024, ESP.getFreePsram() / 1024,
      (unsigned)uxTaskGetStackHighWaterMark(displayTaskHandle),
      (unsigned)uxTaskGetStackHighWaterMark(clockTaskHandle),
      (unsigned)uxTaskGetStackHighWaterMark(netTaskHandle),
      (unsigned)uxTaskGetStackHighWaterMark(mqttTaskHandle),
      (unsigned)uxTaskGetStackHighWaterMark(clusterTaskHandle),
      (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}
