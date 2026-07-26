// ClusterLeaderGrid.cpp — grid submit + self row + wall mirror (#273/#277)
// Split out of ClusterLeader.cpp (#352); contract in ClusterLeader.h,
// shared seams in ClusterLeaderInternal.h.

#include "ClusterLeader.h"

#include <LittleFS.h>
#include <MD5Builder.h>
#include <WiFi.h>
#include <esp_http_client.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <errno.h>  // #340: lwip socket errno for stream-failure diagnostics
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sys/time.h>

#include <atomic>

#include "BuildVersion.h"  // GIT_REV — the cluster's firmware version (#276)
#include "ClockPolicy.h"  // clockIsTimeSynced
#include "ClusterDigest.h"  // ping piggyback: digest build + health parse (#294)
#include "ClusterFollowerPolicy.h"  // clusterRenderDelayMs (shared math)
#include "ClusterHmac.h"  // cluster-wire auth: key mint + request signing
#include "WearPolicy.h"  // self-row wear fold for the status/digest health
#include "ClusterRolloutPolicy.h"
#include "DisplayCommand.h"
#include "FollowerImagePolicy.h"  // #304 on-demand esp01 firmware relay
#include "FollowerImageStore.h"
#include "HelpersSerialHandling.h"
#include "MqttService.h"  // mqttNotificationActive — self-row re-show gate
#include "ReflashPlan.h"
#include "Tasks.h"
#include "TaskWatchdog.h"
#include "WebEndpoints.h"  // #337: webDisplayContentSnapshot() — leader's mode

#include "ClusterLeaderInternal.h"

// --- grid submit ----------------------------------------------------------------

// Slices `contentKey`-identified content into segments and stages the
// deltas. The key carries everything that changes the frame (mode, text,
// alignment, speed) so identical ticks dedup to nothing.
static void submitGrid(const String& contentKey, bool isClock,
                       const String& textOrTime, const String& date,
                       const String& alignment, int speed,
                       DisplaySource source) {
  if (!clusterLeaderEnabled()) return;

  bool synced = false;
  uint64_t nowE = epochNowMs(synced);

  LeaderLock lock;
  if (contentKey == lastContentKey) return;

  String segs[CLUSTER_MAX_MEMBERS];
  DisplayAlignment align = displayAlignmentFromString(alignment);
  bool ok = isClock
                ? clusterClockSegments(textOrTime, date, align, table, segs)
                : layoutGridText(textOrTime, align, table, segs);
  if (!ok) {
    // Re-validation failed — never fan out from a bad table (Hard rule
    // philosophy: config-time AND use-time checks).
    SerialPrintln(F("cluster: submit refused — member table invalid"));
    return;
  }
  lastContentKey = contentKey;
  gridSpeed = speed;
  gridAlignment = alignment;
  gridCommitAtMs = synced ? nowE + CLUSTER_COMMIT_LEAD_MS : 0;

  bool anyChanged = false;
  for (int i = 0; i < table.count; i++) {
    if (segs[i] == segments[i]) continue;  // that row didn't change
    segments[i] = segs[i];
    anyChanged = true;
    if (clusterMemberIsSelf(table.members[i])) {
      selfPending = true;
      selfText = segs[i];
      selfSpeed = speed;
      // Carry the producer across the grid boundary (#403). Without this the
      // leader's own row would report Leader — "I put this here because I put
      // it here" — and the browser/MQTT/clock origin would be lost at exactly
      // the box where it is still knowable.
      selfSource = source;
      selfDueMs =
          millis() + clusterRenderDelayMs(gridCommitAtMs, nowE, synced);
    } else {
      clusterMemberMarkRenderDirty(runtimes[i], millis());
    }
  }
  if (anyChanged) {
    gridGenerationAtomic.fetch_add(1, std::memory_order_relaxed);
  }
}

void clusterLeaderSubmitText(const String& text, const String& alignment,
                             int speed, DisplaySource source) {
  submitGrid("t:" + alignment + ":" + String(speed) + ":" + text, false, text,
             "", alignment, speed, source);
}

void clusterLeaderSetSelfRole(const String& role) {
  LeaderLock lock;
  leaderSelfRole = role;
}

void clusterLeaderSetTz(const String& tzPosix) {
  // #342: rides every join body so esp01 rows can clock-fallback when this
  // leader dies. A live tz change reaches members at their next (re)join —
  // leader reboot or degraded-recovery — which is fine for an event this
  // rare; the members keep their last known zone meanwhile.
  LeaderLock lock;
  leaderTzPosix = tzPosix;
}

void clusterLeaderSubmitClock(const String& timeText, const String& dateText,
                              const String& alignment, int speed) {
  submitGrid("c:" + alignment + ":" + String(speed) + ":" + timeText + "|" +
                 dateText,
             true, timeText, dateText, alignment, speed, DisplaySource::Clock);
}

// /stop propagation (#317): blank every FOLLOWER row in sync (the leader's own
// row is blanked by the local Stop opcode). `lastContentKey` is deliberately
// NOT touched — so the next clock tick recomputes the SAME content key and
// submitGrid early-returns, leaving the wall blank until the clock content
// actually moves (next minute) or a producer sends new text. That mirrors a
// standalone board's "blank until the clock ticks on" behavior exactly.
void clusterLeaderBlankWall() {
  if (!clusterLeaderEnabled()) return;
  bool synced = false;
  uint64_t nowE = epochNowMs(synced);
  LeaderLock lock;
  gridCommitAtMs = synced ? nowE + CLUSTER_COMMIT_LEAD_MS : 0;
  bool anyChanged = false;
  for (int i = 0; i < table.count; i++) {
    if (clusterMemberIsSelf(table.members[i])) continue;
    if (segments[i].length() == 0) continue;  // already blank
    segments[i] = "";
    clusterMemberMarkRenderDirty(runtimes[i], millis());
    anyChanged = true;
  }
  if (anyChanged) {
    gridGenerationAtomic.fetch_add(1, std::memory_order_relaxed);
  }
}

// The leader's own row: due commitAt enqueue, plus the segment re-show
// that restores it after transients/reset-units (the follower gets the
// same from its gated clockTask).
void serviceSelfRow() {
  LeaderLock lock;
  if (selfPending) {
    if ((int32_t)(millis() - selfDueMs) < 0) return;
    if (reflashInProgress(displaySnapshotGet().reflash)) return;  // retry
    if (displayEnqueue(makeShowTextCommand(selfText, "left", selfSpeed, selfSource))) {
      selfPending = false;
    }
    return;
  }
  if (selfText.length() == 0) return;
  if (mqttNotificationActive()) return;  // overlay owns the row for now
  DisplaySnapshot snap = displaySnapshotGet();
  if (snap.busy || reflashInProgress(snap.reflash)) return;
  if (selfText == String(snap.currentText)) return;
  displayEnqueue(makeShowTextCommand(selfText, "left", selfSpeed, selfSource));
}

uint32_t clusterLeaderGridGeneration() {
  return gridGenerationAtomic.load(std::memory_order_relaxed);
}

int clusterLeaderMirrorRows(String* rows, int& selfRowOut,
                            const String& selfRowText,
                            const String& alignment) {
  // -1 = no own row: a pure-orchestrator table is legal (config requires
  // at most one self member, not at least one) and the browser must not
  // anchor the health strip under someone else's row.
  selfRowOut = -1;
  if (leaderMutex == nullptr || !enabledAtomic.load()) return 0;
  LeaderLock lock;
  for (int i = 0; i < table.count; i++) {
    if (clusterMemberIsSelf(table.members[i])) {
      // #333: a width-0 self member is OFF-GRID — a headless leader (monitor/
      // backup, no units of its own) renders no row, so leave selfRowOut at
      // -1. Its `row` is meaningless off-grid and would wrongly anchor the
      // self health strip under a follower's row.
      if (table.members[i].width != 0) selfRowOut = table.members[i].row;
      break;
    }
  }
  return clusterMirrorRows(table, segments, selfRowText,
                           displayAlignmentFromString(alignment), rows);
}
