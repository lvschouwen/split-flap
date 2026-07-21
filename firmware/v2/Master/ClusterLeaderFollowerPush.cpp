// ClusterLeaderFollowerPush.cpp — on-demand ESP-01 firmware relay + file-source convergence stream (#304/#344)
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

// --- on-demand ESP-01 firmware relay (#304 Part B) --------------------------------
// Streams the stored /follower-fw.bin to a chosen esp01 row's EXISTING POST
// /firmware/master, reusing #276's multipart wire + chunk cadence but sourcing
// the LittleFS file. clusterTask-private session; mutually exclusive with the
// auto-rollout (rolloutServiceTick stands down while a push is active, and a
// push won't start while the rollout is uploading). Facts are recomputed per
// push — unlike the running slot, the stored file can change between pushes.

static esp_http_client_handle_t fpushClient = nullptr;
static File fpushFile;
static uint32_t fpushOffset = 0;
static uint32_t fpushLen = 0;
static String fpushMd5;
static String fpushBoundary;  // md5-derived (#292) — cannot occur in the image
static String fpushRev;       // stored image rev → the ?v= intendedVersion

void followerPushCloseUpload() {
  if (fpushFile) fpushFile.close();
  if (fpushClient != nullptr) {
    esp_http_client_close(fpushClient);
    esp_http_client_cleanup(fpushClient);
    fpushClient = nullptr;
  }
  followerImageReleaseRelay();  // let netTask flush a queued upload
}

// Size + MD5 over the stored file (reuses rolloutBuf — the auto-rollout and
// this push never overlap by the mutual-exclusion guards).
static bool followerPushEnsureFacts() {
  File f = LittleFS.open(FOLLOWER_IMAGE_PATH, FILE_READ);
  if (!f) return false;
  uint32_t len = f.size();
  if (len == 0) { f.close(); return false; }
  MD5Builder md5;
  md5.begin();
  uint32_t off = 0;
  while (off < len) {
    uint32_t take = len - off;
    if (take > sizeof(rolloutBuf)) take = sizeof(rolloutBuf);
    if (f.read(rolloutBuf, take) != (int)take) { f.close(); return false; }
    md5.add(rolloutBuf, take);
    off += take;
  }
  f.close();
  md5.calculate();
  fpushMd5 = md5.toString();
  fpushBoundary = clusterRolloutBoundary(fpushMd5);
  fpushLen = len;
  fpushRev = followerImageStoredRev();
  return true;
}

static bool followerPushOpenUpload(const String& host) {
  wdtFeed();  // #314: open + preamble write can each block the stream timeout
  fpushFile = LittleFS.open(FOLLOWER_IMAGE_PATH, FILE_READ);
  if (!fpushFile) { followerPushCloseUpload(); return false; }
  String url = clusterRolloutUrl(host, fpushMd5, fpushRev.c_str());
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  // #340: firmware stream — same dedicated write timeout as the rollout.
  cfg.timeout_ms = CLUSTER_ROLLOUT_STREAM_TIMEOUT_MS;
  cfg.disable_auto_redirect = true;  // #313: firmware never chases a redirect
  fpushClient = esp_http_client_init(&cfg);
  if (fpushClient == nullptr) { followerPushCloseUpload(); return false; }
  esp_http_client_set_method(fpushClient, HTTP_METHOD_POST);
  String contentType = clusterRolloutContentType(fpushBoundary);
  esp_http_client_set_header(fpushClient, "Content-Type", contentType.c_str());
  String preamble = clusterRolloutMultipartPreamble(fpushBoundary);
  int total = (int)preamble.length() + (int)fpushLen +
              (int)clusterRolloutMultipartTrailer(fpushBoundary).length();
  if (esp_http_client_open(fpushClient, total) != ESP_OK ||
      esp_http_client_write(fpushClient, preamble.c_str(), preamble.length()) !=
          (int)preamble.length()) {
    followerPushCloseUpload();
    return false;
  }
  fpushOffset = 0;
  return true;
}

// #344: auto-start an esp01 convergence — called from rolloutServiceTick's
// Idle branch when the S3 scan found nothing (so the two sources share the
// strictly-sequential machine, attempts and blocked latch).
void rolloutTryFollowerImageStart() {
  if (!followerImageStored()) return;
  String storedRev = followerImageStoredRev();
  int candidate;
  {
    LeaderLock lock;
    // #343: a NEW stored image voids the evidence behind rescue give-ups —
    // esp01 members get fresh attempts before the scan.
    static String lastStoredRev;
    if (storedRev != lastStoredRev) {
      if (lastStoredRev.length() > 0) {
        clusterRolloutForgiveFollowerTargets(rollout, table, runtimes,
                                             FOLLOWER_IMAGE_PLAT);
      }
      lastStoredRev = storedRev;
    }
    candidate = clusterFollowerImageNextCandidate(
        table, runtimes, storedRev.c_str(), FOLLOWER_IMAGE_PLAT, rollout,
        millis());
  }
  if (candidate < 0) return;
  // Claim BEFORE reading facts (netTask-rewrite guard, mirrors the manual
  // push); a pending upload flush simply retries next tick.
  if (!followerImageTryClaimRelay()) return;
  if (!followerPushEnsureFacts()) {
    followerImageReleaseRelay();
    SerialPrintln(
        F("cluster: stored follower image unreadable — esp01 convergence "
          "holds off"));
    LeaderLock lock;
    // Idle + memberIndex -1: burns no attempt, just arms the retry holdoff.
    clusterRolloutUploadFailed(rollout, millis());
    return;
  }
  String host;
  String fromRev;
  bool rescuePush;
  {
    LeaderLock lock;
    // #343: rescue-triggered pushes burn their attempt at start (the
    // rejoin verdict can't be trusted — see ClusterRolloutPolicy.h).
    rescuePush = runtimes[candidate].rescue;
    if (rescuePush) clusterRolloutStartRescue(rollout, candidate, fpushLen);
    else clusterRolloutStart(rollout, candidate, fpushLen);
    rolloutFollowerSource = true;
    rolloutRescueTriggered = rescuePush;
    rolloutFollowerTargetRev = fpushRev;
    host = table.members[candidate].host;
    fromRev = runtimes[candidate].rev;
  }
  SerialPrintln("cluster: converging esp01 " + host + " rev " + fromRev +
                " -> " + rolloutFollowerTargetRev +
                (rescuePush ? " (rescue re-push)" : " (stored follower image)"));
  if (!followerPushOpenUpload(host)) {
    SerialPrintln("cluster: esp01 convergence could not reach " + host);
    LeaderLock lock;
    clusterRolloutUploadFailed(rollout, millis());
    rolloutFollowerSource = false;
    rolloutRescueTriggered = false;
  }
}

// caller holds LeaderLock.
static void followerPushFinishLocked(FollowerPushResult result,
                                     const String& host, const String& what) {
  followerPushFinish(followerPush, result);
  SerialPrintln("cluster: follower-image push to " + host + " " + what);
}

// #344: one failure settle point for the file-source stream — the same
// low-level pump serves the manual push (followerPush state) and the auto
// esp01 convergence (rollout state). Takes the lock itself.
static void fpushSettleFailed(const String& what) {
  LeaderLock lock;
  if (rolloutFollowerSource) {
    int t = rollout.memberIndex;
    String host = (t >= 0 && t < table.count) ? String(table.members[t].host)
                                              : String("?");
    SerialPrintln("cluster: esp01 convergence to " + host + " " + what);
    clusterRolloutUploadFailed(rollout, millis());
    rolloutFollowerSource = false;
    rolloutRescueTriggered = false;
    return;
  }
  int t = followerPush.memberIndex;
  String host = (t >= 0 && t < table.count) ? String(table.members[t].host)
                                            : String("?");
  followerPushFinishLocked(FollowerPushResult::Failed, host, what);
}

void followerPushPump() {
  uint32_t budget = CLUSTER_ROLLOUT_CHUNK_PER_TICK;
  uint32_t tickStartMs = millis();
  while (budget > 0 && fpushOffset < fpushLen) {
    // #340: mirrors rolloutPumpUpload — TWDT feed between long-timeout
    // writes, wall-clock tick bound, stream resumes next tick.
    wdtFeed();
    if (millis() - tickStartMs > (uint32_t)CLUSTER_ROLLOUT_STREAM_TIMEOUT_MS)
      break;
    uint32_t take = fpushLen - fpushOffset;
    if (take > sizeof(rolloutBuf)) take = sizeof(rolloutBuf);
    if (take > budget) take = budget;
    if (fpushFile.read(rolloutBuf, take) != (int)take) {
      followerPushCloseUpload();
      fpushSettleFailed("file read failed at " + String((unsigned)fpushOffset));
      return;
    }
    int wrote =
        esp_http_client_write(fpushClient, (const char*)rolloutBuf, take);
    if (wrote != (int)take) {
      int wrErrno = errno;  // capture before close() can clobber it (#340)
      followerPushCloseUpload();
      fpushSettleFailed("failed mid-stream at " + String((unsigned)fpushOffset) +
                        "/" + String((unsigned)fpushLen) + " B (write " +
                        String(wrote) + ", errno " + String(wrErrno) + ")");
      return;
    }
    fpushOffset += take;
    budget -= take;
  }
  {
    LeaderLock lock;
    if (rolloutFollowerSource) rollout.bytesSent = fpushOffset;  // #344
    else followerPush.bytesSent = fpushOffset;
  }
  if (fpushOffset < fpushLen) return;  // more ticks to go

  esp_http_client_set_timeout_ms(fpushClient,
                                 CLUSTER_ROLLOUT_FINALIZE_TIMEOUT_MS);
  String trailer = clusterRolloutMultipartTrailer(fpushBoundary);
  int status = -1;
  if (esp_http_client_write(fpushClient, trailer.c_str(), trailer.length()) ==
          (int)trailer.length() &&
      esp_http_client_fetch_headers(fpushClient) >= 0) {
    status = esp_http_client_get_status_code(fpushClient);
  }
  followerPushCloseUpload();

  LeaderLock lock;
  if (rolloutFollowerSource) {
    // #344: settle the auto convergence into the rollout machine — the
    // rejoin health gate takes over on 200, exactly like the S3 stream.
    uint32_t nowMs = millis();
    int t = rollout.memberIndex;
    String h = (t >= 0 && t < table.count) ? String(table.members[t].host)
                                           : String("?");
    if (status == 200) {
      SerialPrintln("cluster: esp01 " + h + " flashed — waiting for rejoin on " +
                    rolloutFollowerTargetRev);
      clusterRolloutUploadDone(rollout, nowMs);
      // rolloutFollowerSource stays set: the wait gate compares the rejoin
      // rev against the stored image's rev.
      if (t >= 0 && t < table.count) {
        runtimes[t].joined = false;
        runtimes[t].rev = "";
        runtimes[t].failures = 0;
        runtimes[t].nextAttemptMs = nowMs + 5000;
      }
    } else if (status == 409) {
      SerialPrintln("cluster: esp01 " + h +
                    " busy (409) — convergence retries later");
      clusterRolloutUploadRejected(rollout, nowMs);
      rolloutFollowerSource = false;
      rolloutRescueTriggered = false;
    } else {
      SerialPrintln("cluster: esp01 convergence to " + h + " failed (status " +
                    String(status) + ")");
      clusterRolloutUploadFailed(rollout, nowMs);
      rolloutFollowerSource = false;
      rolloutRescueTriggered = false;
    }
    return;
  }
  int target = followerPush.memberIndex;
  String host = (target >= 0 && target < table.count)
                    ? String(table.members[target].host) : String("?");
  if (status == 200) {
    followerPushFinishLocked(FollowerPushResult::Done, host,
                             "flashed — rebooting, will rejoin");
    // Reboot window (~750 ms): mark un-joined so the doomed contacts don't
    // read as failures → degraded noise (mirrors the rollout).
    if (target >= 0 && target < table.count) {
      runtimes[target].joined = false;
      runtimes[target].rev = "";
      runtimes[target].failures = 0;
      runtimes[target].nextAttemptMs = millis() + 5000;
    }
  } else if (status == 409) {
    followerPushFinishLocked(FollowerPushResult::Rejected, host,
                             "busy (409) — try again shortly");
  } else {
    followerPushFinishLocked(FollowerPushResult::Failed, host,
                             "failed (status " + String(status) + ")");
  }
}

void followerPushServiceTick() {
  FollowerPushPhase phase;
  {
    LeaderLock lock;
    phase = followerPush.phase;
  }
  if (phase == FollowerPushPhase::Uploading) {
    followerPushPump();
    return;
  }

  // Idle: pick up a staged trigger, if any.
  bool pending;
  String host;
  {
    LeaderLock lock;
    pending = followerPushPending;
    host = followerPushHost;
    if (pending) followerPushPending = false;
  }
  if (!pending) return;

  // Never start while the auto-rollout owns the flash, or while an image
  // upload is still flushing to the file this push would read.
  {
    LeaderLock lock;
    if (rollout.phase != ClusterRolloutPhase::Idle) {
      followerPushFinish(followerPush, FollowerPushResult::Rejected);
      SerialPrintln(F("cluster: follower push deferred — auto-rollout busy"));
      return;
    }
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
  FollowerPushEligibility elig =
      followerPushEligibility(plat, followerImageStored());
  if (idx < 0 || elig != FollowerPushEligibility::Eligible) {
    LeaderLock lock;
    followerPushFinish(followerPush, FollowerPushResult::Failed);
    SerialPrintln("cluster: follower push to " + host + " refused: " +
                  (idx < 0 ? String("unknown member")
                           : String(followerPushEligibilityReason(elig))));
    return;
  }

  // Atomically claim the file against a netTask rewrite BEFORE reading it for
  // facts — fails if an upload is still accumulating or its flush is pending
  // (a stale/about-to-change file).
  if (!followerImageTryClaimRelay()) {
    LeaderLock lock;
    followerPushFinish(followerPush, FollowerPushResult::Rejected);
    SerialPrintln(F("cluster: follower push deferred — image upload flushing"));
    return;
  }
  if (!followerPushEnsureFacts()) {
    followerImageReleaseRelay();
    LeaderLock lock;
    followerPushFinish(followerPush, FollowerPushResult::Failed);
    SerialPrintln(F("cluster: follower image unreadable — push aborted"));
    return;
  }
  {
    LeaderLock lock;
    followerPushStart(followerPush, idx, fpushLen);
  }
  SerialPrintln("cluster: pushing follower image rev " + fpushRev + " to " +
                host + " (" + String(fpushLen) + " bytes)");
  if (!followerPushOpenUpload(host)) {
    LeaderLock lock;
    followerPushFinishLocked(FollowerPushResult::Failed, host,
                             "could not open upload");
  }
}
