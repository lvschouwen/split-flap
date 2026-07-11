#include "Tasks.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>

#include "ClockPolicy.h"
#include "FlapFrame.h"
#include "HelpersSerialHandling.h"
#include "StatusLed.h"
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
static constexpr uint32_t NET_TASK_STACK = 4096;
static constexpr uint32_t MQTT_TASK_STACK = 4096;

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

static StaticTask_t displayTaskBuf, clockTaskBuf, netTaskBuf, mqttTaskBuf;
static StackType_t displayTaskStack[DISPLAY_TASK_STACK];
static StackType_t clockTaskStack[CLOCK_TASK_STACK];
static StackType_t netTaskStack[NET_TASK_STACK];
static StackType_t mqttTaskStack[MQTT_TASK_STACK];

static StaticQueue_t displayQueueBuf;
static uint8_t displayQueueStorage[DISPLAY_QUEUE_DEPTH * sizeof(DisplayCommand)];
static QueueHandle_t displayQueue = nullptr;

static StaticQueue_t mqttInboxBuf;
static uint8_t mqttInboxStorage[MQTT_INBOX_DEPTH * sizeof(MqttInboxMessage)];
static QueueHandle_t mqttInbox = nullptr;

static TaskHandle_t displayTaskHandle, clockTaskHandle, netTaskHandle,
    mqttTaskHandle;

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
  displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT);
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

    // v1 #138 brownout throttle: once a batch is full (or the plan is
    // exhausted), wait for those units to come back online + finish homing
    // before flashing more. Bus safety pacing — never abort-shortened.
    if (inBatch >= REFLASH_BATCH_SIZE || (k == total - 1 && inBatch > 0)) {
      reflashProgressSettling(local.reflash);
      snapshotPublish(local);
      unitBusWaitBatchIdle(batch, inBatch, REFLASH_BATCH_SETTLE_MS);
      inBatch = 0;
    }
  }

  // Final reprobe + health poll: published topology and fw grades are
  // execution-time truth (a failed/cancelled unit shows as bootloader and
  // stays pinned there — deliberate, see block comment).
  unitBusProbe(busFacts, UNITS_AMOUNT);
  unitBusPollHealth(busFacts, UNITS_AMOUNT);
  displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT);
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
  unitBusPollHealth(busFacts, UNITS_AMOUNT);
  displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT);
  snapshotPublish(local);
  if (local.detectedUnitCount == 0) {
    SerialPrintf("display: no units responding — assuming full width %d\n",
                 local.displayWidth);
  } else {
    SerialPrintf("display: probe done, width %d\n", local.displayWidth);
  }

  // Boot auto-install + auto-update (#205, full v1 parity): flash any unit
  // the probe found sitting in twiboot (the recovery path for a failed or
  // cancelled flash), and push provably-outdated sketch units through the
  // same job. Runs before the command loop, but the WiFi join is already
  // racing on core 0 — the job's reflashActive gate turns away whatever
  // comes up mid-install (web 409s, clock skips).
  if (reflashHasWork(local, ReflashSweep::OutdatedOnly)) {
    SerialPrintln(F("reflash: boot auto-install/auto-update starting"));
    runReflashJob(local, busFacts, ReflashSweep::OutdatedOnly);
  }

  DisplayCommand cmd;
  for (;;) {
    if (xQueueReceive(displayQueue, &cmd, portMAX_DELAY) != pdTRUE) continue;
    local.busy = true;
    snapshotPublish(local);
    if (displayApplyCommand(local, cmd)) {
      SerialPrintln("display: " + describeDisplayCommand(cmd));
      switch (cmd.opcode) {
        case DisplayOpcode::ShowText: {
          uint8_t letters[UNITS_AMOUNT];
          flapFrameBuild(cmd.text, local.displayWidth, cmd.alignment,
                         letters);
          // Return value = failed unit writes (v1's lastShowUnitWriteErrors,
          // an MQTT telemetry input) — thread it into the snapshot when the
          // MQTT slice lands.
          unitBusShowFrame(local.units, local.displayWidth, letters,
                           convertSpeedToUnit(cmd.speed));
          break;
        }
        case DisplayOpcode::Probe:
          // Re-scan + health refresh: an address change moves a unit to a
          // slot only a probe can see (v1 #56 semantics). A refresh queued
          // right behind a /unit/reboot must not scan into the twiboot
          // window (review 2026-07-11) — wait the risk deadline out first.
          settleBeforeProbe();
          unitBusProbe(busFacts, UNITS_AMOUNT);
          unitBusPollHealth(busFacts, UNITS_AMOUNT);
          displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT);
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
          unitBusPollHealth(busFacts, UNITS_AMOUNT);
          displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT);
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
          unitBusPollHealth(busFacts, UNITS_AMOUNT);
          displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT);
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
          displayApplyMaintResult(local, cmd, MaintOutcome::Ok,
                                  MaintReason::None);
          break;
        }
        case DisplayOpcode::Stop: {
          // The abort flag (set by the /stop handler at enqueue) already
          // short-circuited every wait ahead of us; now park the display.
          int status = unitBusBroadcastHome();
          unitBusClearAbort();
          displayApplyMaintResult(
              local, cmd,
              status == 0 ? MaintOutcome::Ok : MaintOutcome::WireFail,
              MaintReason::None);
          break;
        }
        case DisplayOpcode::ReflashUnits: {
          // Drain commands that slipped into the queue between this job's
          // enqueue and its execution (the gate only closes when the job
          // starts): they would burst-drain onto a display the job just
          // rebuilt. Stop survives — it is the cancel, and its abort flag
          // is already set (order rule: flag BEFORE enqueue).
          DisplayCommand stale;
          while (xQueueReceive(displayQueue, &stale, 0) == pdTRUE) {
            if (stale.opcode == DisplayOpcode::Stop) {
              xQueueSend(displayQueue, &stale, 0);
              break;  // a Stop means nothing else can follow it usefully
            }
            SerialPrintln("display: dropped queued command at reflash start: " +
                          describeDisplayCommand(stale));
          }

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
  for (;;) {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1000));

    DisplaySnapshot snap = displaySnapshotGet();
    // Producer gate (#205): while a reflash runs, skip the whole tick —
    // nothing queues, nothing burst-drains after, and lastQueued stays
    // untouched so the first post-job tick re-sends fresh content.
    if (reflashInProgress(snap.reflash)) continue;
    clockTickObserve(lastQueued, String(snap.currentText));

    WebContentSnapshot content = webDisplayContentSnapshot();
    time_t now = time(nullptr);

    ClockTickInput in;
    in.deviceMode = content.deviceMode;
    in.inputText = content.inputText;
    in.timeSynced = clockIsTimeSynced(now);
    in.formattedTime = in.timeSynced ? formatDateTime(now, CLOCK_FORMAT) : "";
    in.displayBusy = snap.busy;
    in.displayCurrentText = String(snap.currentText);
    in.lastQueued = lastQueued;

    ClockTickDecision d = decideClockTick(in);
    if (d.enqueue) {
      DisplayCommand cmd =
          makeShowTextCommand(d.text, content.alignment, content.flapSpeed);
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
  for (;;) {
    wifiServiceTick();
    webEndpointsLoop(*ctx->settings, *ctx->store);
    statusLedTick();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Blocks on its inbox; nothing posts until the MQTT slice lands its client.
static void mqttTaskMain(void*) {
  SerialPrintf("mqttTask up on core %d\n", xPortGetCoreID());
  MqttInboxMessage msg;
  for (;;) {
    if (xQueueReceive(mqttInbox, &msg, portMAX_DELAY) != pdTRUE) continue;
    SerialPrintln("mqtt: inbox message dropped (client is a later #58 slice): " +
                  String(msg.topic));
  }
}

// --- lifecycle -----------------------------------------------------------------

void tasksInit(MasterSettings& settings, SettingsStore& store) {
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
