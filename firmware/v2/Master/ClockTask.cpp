// ClockTask.cpp — the 1 Hz mode ticker (#192), split out of Tasks.cpp
// (#352). Re-shows the active mode's content — clock time or the retained
// message — via the pure decideClockTick(); reroutes LOGICAL grid content to
// the cluster leader while leading (#273) and re-shows the held segment
// while clustered (#272).

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ctime>

#include "ClockPolicy.h"
#include "ClusterFollower.h"
#include "ClusterLeader.h"
#include "HelpersSerialHandling.h"
#include "MqttService.h"
#include "TaskWatchdog.h"
#include "TasksInternal.h"
#include "WebEndpoints.h"

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
                                leaderContent.flapSpeed,
                                leaderContent.inputTextSource);
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
      // This ticker is not always "the clock" (#403): in text mode it
      // re-shows the retained message — after an overlay reverts, say — and
      // must credit whoever wrote it. Only an actual clock render is Clock;
      // a held segment belongs to the leader.
      DisplaySource source = segmentReshow      ? DisplaySource::Leader
                             : in.deviceMode == "clock"
                                 ? DisplaySource::Clock
                                 : content.inputTextSource;
      DisplayCommand cmd =
          segmentReshow
              ? makeShowTextCommand(d.text, "left", cluster.heldSpeed, source)
              : makeShowTextCommand(d.text, content.alignment,
                                    content.flapSpeed, source);
      if (displayEnqueue(cmd)) {
        lastQueued = d.text;
      }
      // Queue full: dedup state unchanged, the next tick retries.
    }
  }
}
