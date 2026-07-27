// ClockTask.cpp — the 1 Hz mode ticker (#192), split out of Tasks.cpp
// (#352). Re-shows the active mode's content — clock time or the retained
// message — via the pure decideClockTick(); reroutes LOGICAL grid content to
// the display's own content — the cluster reroute and the follower gate
// went with the cluster wire.

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ctime>

#include "ClockPolicy.h"
#include "HelpersSerialHandling.h"
#include "MqttService.h"
#include "TaskWatchdog.h"
#include "TasksInternal.h"
#include "ContentState.h"

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
    if (reflashInProgress(snap.reflash)) continue;
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
    clockTickObserve(lastQueued, String(snap.currentText));

    DisplayContent content = contentSnapshot();
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
      // This ticker is not always "the clock" (#403): in text mode it
      // re-shows the retained message — after an overlay reverts, say — and
      // must credit whoever wrote it, not claim the clock authored it.
      DisplaySource source = in.deviceMode == "clock"
                                 ? DisplaySource::Clock
                                 : content.inputTextSource;
      DisplayCommand cmd = makeShowTextCommand(
          d.text, content.alignment, content.flapSpeed, source);
      if (displayEnqueue(cmd)) {
        lastQueued = d.text;
      }
      // Queue full: dedup state unchanged, the next tick retries.
    }
  }
}
