// ClusterLeaderStatus.cpp — status snapshot + config/push staging API (#273/#304)
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

// Fills the status snapshot — leaderMutex HELD by the caller. The self
// row's health folds straight from the display snapshot (#294); remote
// rows carry their last ping-reply health (leaderMutex -> snapshotMutex is
// the allowed lock order).
void statusFillLocked(ClusterLeaderStatus& st) {
  st.enabled = enabledAtomic.load();
  st.epoch = epoch;
  st.seq = seqCounter;
  st.memberCount = table.count;

  DisplaySnapshot snap = displaySnapshotGet();
  char selfMask[16];
  clusterFaultMaskHex(snap.units, snap.displayWidth, selfMask,
                      sizeof(selfMask));
  WearAssessment selfWear;
  assessWear(snap.units, snap.displayWidth, selfWear);

  for (int i = 0; i < table.count; i++) {
    ClusterLeaderMemberStatus& out = st.members[i];
    out.host = table.members[i].host;
    out.row = table.members[i].row;
    out.col = table.members[i].col;
    out.width = table.members[i].width;
    out.joined = runtimes[i].joined || clusterMemberIsSelf(table.members[i]);
    out.degraded = runtimes[i].degraded;
    out.suspect = clusterMemberSuspect(runtimes[i]);
    out.renderStuck = clusterMemberRenderStuck(runtimes[i], millis());
    out.failures = runtimes[i].failures;
    out.rev = runtimes[i].rev;
    out.plat = runtimes[i].plat;
    out.role = runtimes[i].role;
    out.reportedWidth = runtimes[i].reportedWidth;
    out.updating = rollout.phase != ClusterRolloutPhase::Idle &&
                   rollout.memberIndex == i;
    out.updateBlocked = rollout.blocked[i];
    out.rescue = runtimes[i].rescue;  // #343
    out.hmac = runtimes[i].hmacKeyValid;  // signing to this member (#313)
    if (clusterMemberIsSelf(table.members[i])) {
      out.rev = GIT_REV;
      // #332: the self row is never joined/pinged, so its runtime role stays
      // empty — inject our live deviceRole like GIT_REV above.
      out.role = leaderSelfRole;
      out.healthValid = true;
      out.faulty = snap.faultyUnitCount;
      out.detected = snap.detectedUnitCount;
      out.faultMask = selfMask;
      out.wear = selfWear.flaggedCount > 0;
    } else if (runtimes[i].health.valid) {
      out.healthValid = true;
      out.faulty = runtimes[i].health.faulty;
      out.detected = runtimes[i].health.detected;
      out.faultMask = runtimes[i].health.faultMask;
      out.wear = runtimes[i].health.wear;
    }
  }
  st.rolloutPhase = clusterRolloutPhaseName(rollout.phase);
  if (rollout.memberIndex >= 0 && rollout.memberIndex < table.count) {
    st.rolloutHost = table.members[rollout.memberIndex].host;
  }
  st.rolloutSent = rollout.bytesSent;
  st.rolloutTotal = rollout.bytesTotal;
  st.rolloutImageFailed = rolloutFactsFailed.load(std::memory_order_relaxed);
  st.rolloutFollowerImage = rolloutFollowerSource;  // #344
  // On-demand ESP-01 firmware relay (#304). followerImage* query the store's
  // own mutex — leaderMutex → imgMutex is a consistent leaf order.
  st.followerImagePresent = followerImageStored();
  st.followerImageRev = followerImageStoredRev();
  st.followerPushPhase = followerPushPhaseName(followerPush.phase);
  if (followerPush.memberIndex >= 0 && followerPush.memberIndex < table.count) {
    st.followerPushHost = table.members[followerPush.memberIndex].host;
  }
  st.followerPushSent = followerPush.bytesSent;
  st.followerPushTotal = followerPush.bytesTotal;
  st.followerPushResult = followerPushResultName(followerPush.lastResult);
  ClusterGrid grid;
  if (st.enabled && validateMemberTable(table, grid).ok) {
    st.gridRows = grid.rows;
    for (int r = 0; r < grid.rows; r++) st.gridCapacity += grid.rowWidth[r];
  }
}

ClusterLeaderStatus clusterLeaderStatusGet() {
  ClusterLeaderStatus st;
  if (leaderMutex == nullptr) return st;
  LeaderLock lock;
  statusFillLocked(st);
  return st;
}

ClusterConfigVerdict clusterLeaderValidateSpec(const String& membersSpec) {
  ClusterMemberTable t;
  if (!clusterTableFromString(membersSpec, t)) {
    return {400, "Malformed members spec"};
  }
  if (t.count > 0) {
    ClusterGrid grid;
    ClusterVerdict v = validateMemberTable(t, grid);
    if (!v.ok) return {400, String(v.message)};
    int selfCount = 0;
    for (int i = 0; i < t.count; i++) {
      if (clusterMemberIsSelf(t.members[i])) selfCount++;
      for (int j = i + 1; j < t.count; j++) {
        // A mirror is two HOSTS sharing a span — the same host twice can
        // only be a config mistake.
        if (t.members[i].host[0] != '\0' &&
            strcmp(t.members[i].host, t.members[j].host) == 0) {
          return {400, "Duplicate member host"};
        }
      }
    }
    if (selfCount > 1) return {400, "More than one local (empty-host) row"};
  }
  return {200, t.count > 0 ? "Cluster config staged" : "Cluster disabled"};
}

ClusterConfigVerdict clusterLeaderStageConfig(const String& membersSpec) {
  ClusterConfigVerdict verdict = clusterLeaderValidateSpec(membersSpec);
  if (verdict.httpStatus != 200) return verdict;
  {
    LeaderLock lock;
    // The single staged slot is last-writer-wins ON PURPOSE (a demote
    // racing a user edit: whichever intent lands second is the one that
    // holds) — but a user-originated stage must never inherit a pending
    // demote's suppress-leave flag, so it resets here.
    configSpec = membersSpec;
    configPending = true;
    configSuppressLeave = false;
  }
  return verdict;
}

ClusterConfigVerdict clusterLeaderStageFollowerPush(const String& host) {
  ClusterConfigVerdict v;
  if (leaderMutex == nullptr || !clusterLeaderEnabled()) {
    v.httpStatus = 409;
    v.message = "not leading a cluster";
    return v;
  }
  if (host.length() == 0) {
    v.httpStatus = 400;
    v.message = "host required";
    return v;
  }
  int idx = -1;
  String plat;
  {
    LeaderLock lock;
    for (int i = 0; i < table.count; i++) {
      if (clusterMemberIsSelf(table.members[i])) continue;
      if (host == table.members[i].host) {
        idx = i;
        plat = runtimes[i].plat;
        break;
      }
    }
  }
  if (idx < 0) {
    v.httpStatus = 404;
    v.message = "unknown member host";
    return v;
  }
  FollowerPushEligibility elig =
      followerPushEligibility(plat, followerImageStored());
  if (elig != FollowerPushEligibility::Eligible) {
    v.httpStatus = 409;
    v.message = followerPushEligibilityReason(elig);
    return v;
  }
  {
    LeaderLock lock;
    if (followerPush.phase != FollowerPushPhase::Idle || followerPushPending) {
      v.httpStatus = 409;
      v.message = "a follower push is already in progress";
      return v;
    }
    followerPushPending = true;
    followerPushHost = host;
    followerPush.lastResult = FollowerPushResult::None;
  }
  v.message = "queued";
  return v;
}

String buildFreshDigestLocked(const String& leaderMode) {
  ClusterLeaderStatus st;
  statusFillLocked(st);
  String mirror[CLUSTER_MAX_MEMBERS];
  DisplaySnapshot snap = displaySnapshotGet();
  int rowCount =
      clusterMirrorRows(table, segments, String(snap.currentText),
                        displayAlignmentFromString(gridAlignment), mirror);
  String body =
      clusterBuildDigest(0, leaderDeviceName, WiFi.localIP().toString(),
                         clusterTableToString(table), mirror, rowCount, st,
                         rebootHoldMs, leaderMode);
  if (body != digestLastBody) {
    digestLastBody = body;
    digestGen++;
  }
  // Splice the real gen over the 0 sentinel the comparison body carries.
  return "{\"gen\":" + String((unsigned long)digestGen) +
         body.substring(strlen("{\"gen\":0"));
}
