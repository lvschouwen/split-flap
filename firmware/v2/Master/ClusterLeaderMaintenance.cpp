// ClusterLeaderMaintenance.cpp — graceful-reboot hold (#321) + follower log pull (#318 E)
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

// #321 reboot-hold. CLUSTER_REBOOT_HOLD_MS < the follower clamp (60 s).
static const uint32_t CLUSTER_REBOOT_HOLD_MS = 45000;

// #321 reboot-hold delivery. Built once when a reboot is announced, then sent
// ONE member per clusterLeaderTick (see the tick) — the naive "ping everyone in
// one call" is the exact core-0-starving burst #320 was written to eliminate
// (up to 7 dead hosts × the 1.5 s HTTP timeout ≫ the 5 s task watchdog). Non-
// degraded members are queued FIRST so the reachable rows — the ones that could
// actually spuriously take over — get the hold within the drain's bounded wait
// even if some members are dead and burn their timeouts at the tail.
void rebootHoldBuildTargets() {
  String leaderMode = webDisplayContentSnapshot().deviceMode;  // #337: before the lock
  LeaderLock lock;
  rebootHoldCount = 0;
  rebootHoldCursor = 0;
  if (!clusterLeaderEnabled()) return;
  String digest = buildFreshDigestLocked(leaderMode);
  String encoded = urlEncode(digest);
  bool synced = false;
  uint64_t ts = epochNowMs(synced);
  // Pass 0 = reachable (non-degraded) members, pass 1 = degraded ones.
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0;
         i < table.count && rebootHoldCount < CLUSTER_MAX_MEMBERS; i++) {
      if (clusterMemberIsSelf(table.members[i])) continue;
      if (runtimes[i].degraded != (pass == 1)) continue;
      RebootHoldTarget& t = rebootHoldTargets[rebootHoldCount++];
      t.host = table.members[i].host;
      t.body = "digest=" + encoded + "&you=" + String(i);
      if (runtimes[i].hmacKeyValid) {
        String msg = clusterHmacPingMsg(ts, digest, i);
        t.body += "&ts=" + clusterU64ToStr(ts) + "&mac=" +
                  clusterHmacSign(runtimes[i].hmacKey, msg);
      }
    }
  }
}

// Called from the reboot drain (any task) before an intentional restart.
void clusterLeaderAnnounceRebootHold() {
  if (leaderMutex == nullptr || !clusterLeaderEnabled()) return;  // no followers
  {
    LeaderLock lock;
    rebootHoldMs = CLUSTER_REBOOT_HOLD_MS;
  }
  rebootHoldSent.store(false);
  rebootHoldPending.store(true);
}

// True once the hold has been fanned out — or immediately when not leading, so
// a standalone board never waits.
bool clusterLeaderRebootHoldSent() {
  if (leaderMutex == nullptr || !clusterLeaderEnabled()) return true;
  return rebootHoldSent.load();
}

// --- follower log pull (#318 E) --------------------------------------------------

// Interval between log pulls; one member per tick, round-robin. Gentle on
// purpose — follower logs are event-driven, not high-rate, and the pull is a
// blocking round-trip on clusterTask like every other outbound call.
static const uint32_t LOG_PULL_INTERVAL_MS = 20000;
static uint32_t nextLogPullMs = 0;
static int logPullRobin = 0;

// Pulls the delta of one ESP-01 row's log and tees it into the master's own
// log stream so /log/flash shows the whole wall's activity. Scoped to esp01
// members: they have no serial console (GPIO1/3 are the unit bus) so this is
// their only observability path (#316). S3 members are directly reachable and
// keep their own /log + /log/flash, so pulling them would only duplicate.
void followerLogPullTick() {
  uint32_t nowMs = millis();
  if ((int32_t)(nowMs - nextLogPullMs) < 0) return;
  nextLogPullMs = nowMs + LOG_PULL_INTERVAL_MS;

  String host;
  uint32_t cursor = 0;
  int idx = -1;
  {
    LeaderLock lock;
    int n = table.count;
    if (n == 0) return;
    for (int step = 0; step < n; step++) {
      int i = (logPullRobin + step) % n;
      const ClusterMemberDef& def = table.members[i];
      if (clusterMemberIsSelf(def)) continue;
      if (runtimes[i].plat != "esp01") continue;
      if (!runtimes[i].joined || runtimes[i].degraded) continue;
      // Don't poke a row whose async_tcp task is busy taking a firmware
      // push (#304) — the same reason collectMemberWork skips it.
      if (followerPush.phase == FollowerPushPhase::Uploading &&
          followerPush.memberIndex == i) {
        continue;
      }
      host = def.host;
      cursor = runtimes[i].logCursor;
      idx = i;
      logPullRobin = (i + 1) % n;
      break;
    }
  }
  if (idx < 0) return;  // no eligible ESP-01 row this round

  String body;
  int status = clusterHttpGetBody(
      "http://" + host + "/log?after=" + String((unsigned long)cursor), body);
  if (status != 200) return;

  // Body = "<nextCursor>\n<delta>". Advance the cursor from the first line;
  // a body without a newline is malformed — leave the cursor and retry.
  int nl = body.indexOf('\n');
  if (nl < 0) return;
  uint32_t next =
      (uint32_t)strtoul(body.substring(0, nl).c_str(), nullptr, 10);

  {
    LeaderLock lock;
    // The table may have moved while the lock was released; only advance the
    // cursor if this row is still the same host at the same index.
    if (idx < table.count && String(table.members[idx].host) == host) {
      runtimes[idx].logCursor = next;
    }
  }

  // Tee each delta line, tagged with the row's host. SerialPrintln funnels
  // through the master's webLogAppend, which stamps the ingest time (#318 E),
  // so the fleet log carries one coherent clock.
  int start = nl + 1;
  int len = (int)body.length();
  while (start < len) {
    int end = body.indexOf('\n', start);
    if (end < 0) end = len;
    if (end > start) {
      SerialPrintln("[" + host + "] " + body.substring(start, end));
    }
    start = end + 1;
  }
}
