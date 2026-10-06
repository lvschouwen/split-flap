// ClockTask.cpp — the 1 Hz mode ticker (#192), split out of Tasks.cpp
// (#352). Re-shows the active mode's content — clock time or the retained
// message — via the pure decideClockTick(); with row boards the logical
// content goes to the wall instead (WallShow.h).

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ctime>
#include <sys/time.h>

#include "ClockPolicy.h"
#include "HelpersSerialHandling.h"
#include "MqttService.h"
#include "TaskWatchdog.h"
#include "TasksInternal.h"
#include "WallShow.h"
#include "WebEndpoints.h"
#include "WallState.h"

// 1 Hz mode ticker (#192): re-shows the active mode's content — clock time
// or the retained message — whenever the display drifts away from it (mode
// switches, drain messages, minute rollover). The whole decision is the
// pure decideClockTick(); this loop only gathers snapshots and enqueues.
void clockTaskMain(void*) {
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
    if (reflashInProgress(snap.reflash) || wallUnitUpdateRunning()) continue;
    // Notification gate (#224): while an MQTT notification owns the
    // display, don't tick over it — the overlay's expiry releases this
    // gate and the next tick re-shows the active mode's content (v1
    // gated loop()'s mode block via mqttNotificationTick()). On the
    // falling edge, drop the dedup marker: it was frozen at the pre-
    // notification text, which is exactly the revert target — a stale
    // match would block the revert forever.
    static bool notifWasActive = false;
    if (mqttNotificationActive()) {
      notifWasActive = true;
      continue;
    }
    if (notifWasActive) {
      notifWasActive = false;
      lastQueued = "";
    }
    // Quiet (#227): no content from the modes.
    if (tasksQuiet()) continue;
    // A wall with row boards (#566): the logical content goes to the wall,
    // which lays it out and flips every row at one instant. The clock's next
    // minute is handed over ahead of the boundary (WallShowPolicy.h).
    if (wallShowActive()) {
      WebContentSnapshot wallContent = webDisplayContentSnapshot();
      struct timeval tv;
      gettimeofday(&tv, nullptr);
      if (wallContent.deviceMode == "clock") {
        if (clockIsTimeSynced(tv.tv_sec)) {
          const WallClockTarget target = wallClockTarget(
              (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL);
          wallShowClock(formatDateTime(target.minuteEpochS, CLOCK_FORMAT),
                        formatDateTime(target.minuteEpochS, CLUSTER_DATE_FORMAT),
                        wallContent.alignment, wallContent.flapSpeed, target.commitAtMs);
        }
      } else if (wallContent.deviceMode == "text" && wallContent.inputText.length() > 0) {
        wallShowText(wallContent.inputText, wallContent.alignment, wallContent.flapSpeed);
      }
      lastQueued = "";  // the ticker owns nothing while the wall shows
      continue;
    }

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
