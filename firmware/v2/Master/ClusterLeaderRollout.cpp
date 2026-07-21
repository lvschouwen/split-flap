// ClusterLeaderRollout.cpp — fleet firmware rollout (#276) + esp01 auto-convergence dispatch (#344)
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

// --- fleet firmware rollout (#276) ------------------------------------------------
// Sequencing decisions are pure (ClusterRolloutPolicy.h); this section owns
// the flash reads and the multipart streaming upload to the follower's
// EXISTING POST /firmware/master?md5=. Chunked across ticks on purpose:
// renders/pings to OTHER members keep flowing between chunks. The leader
// rebooting mid-stream (its own OTA) just kills the upload — the follower's
// Update session errors out harmlessly and the next boot re-converges.

// clusterTask-private upload session — no lock, only clusterTask touches.
static esp_http_client_handle_t rolloutClient = nullptr;
static uint32_t rolloutOffset = 0;
// Running-image facts, computed once per boot on first use (the running
// slot cannot change under us). A hard failure latches for the boot so a
// broken read doesn't re-verify 2.6 MB every tick — atomic because the
// status snapshot surfaces it from the web task (imageVerifyFailed).
static const esp_partition_t* rolloutPart = nullptr;
static uint32_t rolloutImageLen = 0;
static String rolloutMd5;
static String rolloutBoundary;  // md5-derived — never occurs in the image (#292)
// rolloutFactsFailed / rolloutBuf are defined in ClusterLeader.cpp — the
// facts pass, the fpush stream and the status snapshot share them.


static bool rolloutEnsureImageFacts() {
  if (rolloutImageLen > 0) return true;
  if (rolloutFactsFailed.load(std::memory_order_relaxed)) return false;

  uint32_t t0 = millis();
  const esp_partition_t* part = esp_ota_get_running_partition();
  esp_partition_pos_t pos = {};
  esp_image_metadata_t meta = {};
  if (part != nullptr) {
    pos.offset = part->address;
    pos.size = part->size;
  }
  // Full silent verify: never ship an image we can't prove is intact —
  // exact image_len (header + segments + checksum + appended hash) is a
  // byproduct. ~1-2 s once per boot; renders stall that once.
  if (part == nullptr ||
      esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK) {
    SerialPrintln(F("cluster: running image failed verify — rollout disabled"));
    rolloutFactsFailed = true;
    return false;
  }

  MD5Builder md5;
  md5.begin();
  for (uint32_t off = 0; off < meta.image_len;) {
    uint32_t take = meta.image_len - off;
    if (take > sizeof(rolloutBuf)) take = sizeof(rolloutBuf);
    if (esp_partition_read(part, off, rolloutBuf, take) != ESP_OK) {
      SerialPrintln(F("cluster: running image read failed — rollout disabled"));
      rolloutFactsFailed = true;
      return false;
    }
    md5.add(rolloutBuf, take);
    off += take;
  }
  md5.calculate();
  rolloutMd5 = md5.toString();
  rolloutBoundary = clusterRolloutBoundary(rolloutMd5);
  rolloutPart = part;
  rolloutImageLen = meta.image_len;
  SerialPrintf("cluster: rollout image ready — %u bytes, md5 %s (%u ms)\n",
               (unsigned)rolloutImageLen, rolloutMd5.c_str(),
               (unsigned)(millis() - t0));
  return true;
}

void rolloutCloseUpload() {
  if (rolloutClient == nullptr) return;
  esp_http_client_close(rolloutClient);
  esp_http_client_cleanup(rolloutClient);
  rolloutClient = nullptr;
}

static bool rolloutOpenUpload(const String& host) {
  wdtFeed();  // #314: open + preamble write can each block the stream timeout
  String url = clusterRolloutUrl(host, rolloutMd5, GIT_REV);
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  // #340: chunk writes get the dedicated stream timeout — the receiver's
  // early flash-sector erases stall the socket past the 1.5 s ping/render
  // bound; the pump wall-clock-bounds each tick so renders keep flowing.
  cfg.timeout_ms = CLUSTER_ROLLOUT_STREAM_TIMEOUT_MS;
  cfg.disable_auto_redirect = true;  // #313: firmware never chases a redirect
  rolloutClient = esp_http_client_init(&cfg);
  if (rolloutClient == nullptr) return false;

  esp_http_client_set_method(rolloutClient, HTTP_METHOD_POST);
  String contentType = clusterRolloutContentType(rolloutBoundary);
  esp_http_client_set_header(rolloutClient, "Content-Type",
                             contentType.c_str());
  String preamble = clusterRolloutMultipartPreamble(rolloutBoundary);
  int total = (int)preamble.length() + (int)rolloutImageLen +
              (int)clusterRolloutMultipartTrailer(rolloutBoundary).length();
  if (esp_http_client_open(rolloutClient, total) != ESP_OK ||
      esp_http_client_write(rolloutClient, preamble.c_str(),
                            preamble.length()) != (int)preamble.length()) {
    rolloutCloseUpload();
    return false;
  }
  rolloutOffset = 0;
  return true;
}

// Streams up to one tick budget; on the final chunk, sends the trailer and
// settles the verdict into the policy state.
static void rolloutPumpUpload() {
  uint32_t budget = CLUSTER_ROLLOUT_CHUNK_PER_TICK;
  uint32_t tickStartMs = millis();
  while (budget > 0 && rolloutOffset < rolloutImageLen) {
    // #340: several writes can stack in one tick and each may now block up
    // to the stream timeout — feed the TWDT between them (#314) and bound
    // the tick by wall clock so a slow-but-alive socket can't starve
    // renders/pings (the stream just resumes next tick).
    wdtFeed();
    if (millis() - tickStartMs > (uint32_t)CLUSTER_ROLLOUT_STREAM_TIMEOUT_MS)
      break;
    uint32_t take = rolloutImageLen - rolloutOffset;
    if (take > sizeof(rolloutBuf)) take = sizeof(rolloutBuf);
    if (take > budget) take = budget;
    if (esp_partition_read(rolloutPart, rolloutOffset, rolloutBuf, take) !=
        ESP_OK) {
      rolloutCloseUpload();
      LeaderLock lock;
      SerialPrintln("cluster: rollout flash read failed at offset " +
                    String((unsigned)rolloutOffset));
      clusterRolloutUploadFailed(rollout, millis());
      return;
    }
    int wrote =
        esp_http_client_write(rolloutClient, (const char*)rolloutBuf, take);
    if (wrote != (int)take) {
      int wrErrno = errno;  // capture before close() can clobber it (#340)
      rolloutCloseUpload();
      LeaderLock lock;
      int target = rollout.memberIndex;
      SerialPrintln("cluster: rollout upload to " +
                    ((target >= 0 && target < table.count)
                         ? String(table.members[target].host)
                         : String("?")) +
                    " failed mid-stream at " + String((unsigned)rolloutOffset) +
                    "/" + String((unsigned)rolloutImageLen) + " B (write " +
                    String(wrote) + ", errno " + String(wrErrno) + ")");
      clusterRolloutUploadFailed(rollout, millis());
      return;
    }
    rolloutOffset += take;
    budget -= take;
  }
  {
    LeaderLock lock;
    rollout.bytesSent = rolloutOffset;
  }
  if (rolloutOffset < rolloutImageLen) return;  // more ticks to go

  // Finalize: the follower's Update.end() MD5-verifies the whole image
  // (~1-2 s) before answering — the ONE deliberately long block per
  // converged member (bounded by the finalize budget; renders resume the
  // next tick).
  esp_http_client_set_timeout_ms(rolloutClient,
                                 CLUSTER_ROLLOUT_FINALIZE_TIMEOUT_MS);
  String trailer = clusterRolloutMultipartTrailer(rolloutBoundary);
  int status = -1;
  if (esp_http_client_write(rolloutClient, trailer.c_str(),
                            trailer.length()) == (int)trailer.length() &&
      esp_http_client_fetch_headers(rolloutClient) >= 0) {
    status = esp_http_client_get_status_code(rolloutClient);
  }
  rolloutCloseUpload();

  LeaderLock lock;
  uint32_t nowMs = millis();
  int target = rollout.memberIndex;
  if (target < 0 || target >= table.count) {
    // A config swap raced this finalize inside the same tick — the swap
    // already reset the rollout; nothing to attribute the verdict to.
    return;
  }
  String host(table.members[target].host);
  if (status == 200) {
    SerialPrintln("cluster: " + host +
                  " flashed — rebooting, waiting for it to rejoin on " GIT_REV);
    clusterRolloutUploadDone(rollout, nowMs);
    // The follower reboots in ~750 ms: mark it un-joined now (rev unknown
    // until the rejoin handshake) and skip the doomed contact attempts so
    // the reboot window doesn't read as failures → degraded noise.
    runtimes[target].joined = false;
    runtimes[target].rev = "";
    runtimes[target].failures = 0;
    runtimes[target].nextAttemptMs = nowMs + 5000;
  } else if (status == 409) {
    // Its own reflash/OTA owns the flash right now — transient by contract.
    SerialPrintln("cluster: " + host + " busy (409) — rollout retries later");
    clusterRolloutUploadRejected(rollout, nowMs);
  } else {
    SerialPrintln("cluster: rollout upload to " + host + " failed (status " +
                  String(status) + ")");
    clusterRolloutUploadFailed(rollout, nowMs);
  }
}

// --- #344 esp01 auto-convergence -------------------------------------------------
// The stored follower image (#304) rides the SAME rollout machine: when the
// S3 scan finds nothing, an esp01 member whose reported rev differs from the
// stored image's rev converges through the fpush file-source stream below.
// While rolloutFollowerSource is set, the active rollout streams the fpush
// globals and the rejoin gate compares against the stored rev, not GIT_REV.
// Only clusterTask touches these.
// rolloutFollowerSource / TargetRev / RescueTriggered are defined in
// ClusterLeader.cpp (shared with the fpush stream + status snapshot).

void rolloutServiceTick() {
  ClusterRolloutPhase phase;
  int target;
  {
    LeaderLock lock;
    phase = rollout.phase;
    target = rollout.memberIndex;
  }

  if (phase == ClusterRolloutPhase::Idle) {
    int candidate;
    {
      LeaderLock lock;
      rolloutFollowerSource = false;  // self-heal: Idle never streams (#344)
      rolloutRescueTriggered = false;
      // #343: sustained health forgives esp01 give-ups — a healed row's
      // future beacons must be served again (S3 semantics untouched).
      uint32_t sweepNow = millis();
      for (int i = 0; i < table.count; i++) {
        if (rollout.attempts[i] == 0 && !rollout.blocked[i]) continue;
        if (runtimes[i].plat != FOLLOWER_IMAGE_PLAT) continue;
        if (!runtimes[i].joined || runtimes[i].rescue) continue;
        if (!clusterRolloutHealthyForgiveDue(runtimes[i].healthySinceMs,
                                             sweepNow)) {
          continue;
        }
        rollout.attempts[i] = 0;
        rollout.blocked[i] = false;
        SerialPrintln("cluster: " + String(table.members[i].host) +
                      " healthy long enough — rescue give-ups forgiven");
      }
      // Stand down while an on-demand follower push owns the flash (#304).
      if (followerPush.phase != FollowerPushPhase::Idle) return;
      candidate = clusterRolloutNextCandidate(table, runtimes, GIT_REV,
                                              CLUSTER_LEADER_PLAT, rollout,
                                              millis());
    }
    if (candidate < 0) {
      rolloutTryFollowerImageStart();  // #344: esp01 rows converge next
      return;
    }
    if (!rolloutEnsureImageFacts()) return;
    String host;
    String fromRev;
    {
      LeaderLock lock;
      clusterRolloutStart(rollout, candidate, rolloutImageLen);
      host = table.members[candidate].host;
      fromRev = runtimes[candidate].rev;
    }
    // Both directions on purpose (spec): the leader's build wins even over
    // a NEWER follower — uncluster first to bench-test a follower build.
    SerialPrintln("cluster: converging " + host + " rev " + fromRev +
                  " -> " GIT_REV " (leader build wins both directions)");
    if (!rolloutOpenUpload(host)) {
      SerialPrintln("cluster: rollout could not reach " + host);
      LeaderLock lock;
      clusterRolloutUploadFailed(rollout, millis());
    }
    return;
  }

  if (phase == ClusterRolloutPhase::Uploading) {
    if (rolloutFollowerSource) followerPushPump();  // #344 file-source stream
    else rolloutPumpUpload();
    return;
  }

  // WaitingRejoin: the normal supervision keeps re-joining the rebooted
  // member; the health gate watches the rev it comes back with.
  LeaderLock lock;
  if (target < 0 || target >= table.count) {
    clusterRolloutReset(rollout);  // config swap raced the wait — start over
    return;
  }
  // #344: an esp01 convergence rejoins on the STORED image's rev.
  const char* wantRev =
      rolloutFollowerSource ? rolloutFollowerTargetRev.c_str() : GIT_REV;
  ClusterRolloutWait wait =
      clusterRolloutCheckWait(rollout, runtimes[target].joined,
                              runtimes[target].rev, runtimes[target].rescue,
                              rolloutRescueTriggered, wantRev, millis());
  if (wait != ClusterRolloutWait::Waiting) {
    rolloutFollowerSource = false;
    rolloutRescueTriggered = false;
  }
  switch (wait) {
    case ClusterRolloutWait::Converged:
      SerialPrintln("cluster: " + String(table.members[target].host) +
                    " converged on " + String(wantRev) + " — rollout advances");
      break;
    case ClusterRolloutWait::RescueLooping:
      SerialPrintln("cluster: " + String(table.members[target].host) +
                    " rejoined still in rescue — attempt burned");
      break;
    case ClusterRolloutWait::RolledBack:
      SerialPrintln("cluster: " + String(table.members[target].host) +
                    " came back on its OLD rev (rollback) — attempt burned");
      break;
    case ClusterRolloutWait::TimedOut:
      SerialPrintln("cluster: " + String(table.members[target].host) +
                    " never rejoined after flashing — attempt burned");
      break;
    default:
      break;
  }
}
