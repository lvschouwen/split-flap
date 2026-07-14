// ClusterLeader.cpp — leader-side cluster service (#273). Contract, lock
// ordering and producer rules in ClusterLeader.h; pure decisions in
// ClusterLeaderPolicy.h / ClusterLayout.h.

#include "ClusterLeader.h"

#include <MD5Builder.h>
#include <WiFi.h>
#include <esp_http_client.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sys/time.h>

#include <atomic>

#include "BuildVersion.h"  // GIT_REV — the cluster's firmware version (#276)
#include "ClockPolicy.h"  // clockIsTimeSynced
#include "ClusterFollowerPolicy.h"  // clusterRenderDelayMs (shared math)
#include "ClusterRolloutPolicy.h"
#include "DisplayCommand.h"
#include "HelpersSerialHandling.h"
#include "MqttService.h"  // mqttNotificationActive — self-row re-show gate
#include "ReflashPlan.h"
#include "Tasks.h"

#define CLUSTER_KEY_MEMBERS "clMembers"

// Short LAN timeout: a follower answers in tens of ms; anything past this
// is a failure the backoff machinery owns.
static const int CLUSTER_HTTP_TIMEOUT_MS = 1500;

static SemaphoreHandle_t leaderMutex = nullptr;

struct LeaderLock {
  LeaderLock() { xSemaphoreTake(leaderMutex, portMAX_DELAY); }
  ~LeaderLock() { xSemaphoreGive(leaderMutex); }
  LeaderLock(const LeaderLock&) = delete;
  LeaderLock& operator=(const LeaderLock&) = delete;
};

// Producer gate only — carries NO ordering for the mutex-guarded state
// below (relaxed loads). Never read table/segments/runtimes off a bare
// enabledAtomic check; always take LeaderLock.
static std::atomic<bool> enabledAtomic{false};
static String leaderDeviceName;          // set once in init, read-only after
static SettingsStore* leaderStore = nullptr;  // NVS writes: clusterTask only

// All guarded by leaderMutex.
static ClusterMemberTable table;
static ClusterMemberRuntime runtimes[CLUSTER_MAX_MEMBERS];
static String segments[CLUSTER_MAX_MEMBERS];
static uint32_t epoch = 0;
static uint32_t seqCounter = 0;      // minted per render POST, not per submit
static int gridSpeed = 80;           // web-scale speed of the last submit
static uint64_t gridCommitAtMs = 0;  // shared flip deadline of the last submit
static String lastContentKey;        // submit dedup
// Leader's own row staging (same commitAt clock the followers get).
static bool selfPending = false;
static String selfText;
static int selfSpeed = 0;
static uint32_t selfDueMs = 0;
// Config swap staging (validated at the web boundary, applied here).
static bool configPending = false;
static String configSpec;
// Fleet rollout (#276): sequencing state guarded by leaderMutex (status
// reads it); the live upload session below is clusterTask-private.
static ClusterRolloutState rollout;

// Rollout upload session teardown (#276) — defined in the rollout section
// below; applyStagedConfig needs it before that.
static void rolloutCloseUpload();

static uint64_t epochNowMs(bool& synced) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  synced = clockIsTimeSynced(tv.tv_sec);
  return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static String urlEncode(const String& value) {
  String out;
  out.reserve(value.length() + 8);
  for (unsigned int i = 0; i < value.length(); i++) {
    char c = value[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      out += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

// One blocking form-POST/GET round-trip. Returns -1 on transport failure,
// else the HTTP status; `outBody` gets the (bounded) reply body.
static int clusterHttpRequest(const String& url, const String& postBody,
                              String& outBody) {
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.timeout_ms = CLUSTER_HTTP_TIMEOUT_MS;
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) return -1;

  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type",
                             "application/x-www-form-urlencoded");

  int status = -1;
  if (esp_http_client_open(client, postBody.length()) == ESP_OK) {
    int written = esp_http_client_write(client, postBody.c_str(),
                                        postBody.length());
    if (written == (int)postBody.length() &&
        esp_http_client_fetch_headers(client) >= 0) {
      status = esp_http_client_get_status_code(client);
      char buf[257];
      int got = esp_http_client_read_response(client, buf, sizeof(buf) - 1);
      if (got > 0) {
        buf[got] = '\0';
        outBody = buf;
      }
    }
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return status;
}

void clusterLeaderInit(SettingsStore& store, const String& deviceName) {
  leaderMutex = xSemaphoreCreateMutex();
  if (leaderMutex == nullptr) {
    Serial.println(F("FATAL: leaderMutex allocation failed"));
    abort();
  }
  leaderDeviceName = deviceName;
  leaderStore = &store;
  epoch = esp_random();

  String stored = store.getString(CLUSTER_KEY_MEMBERS, "");
  ClusterMemberTable parsed;
  ClusterGrid grid;
  if (clusterTableFromString(stored, parsed) && parsed.count > 0 &&
      validateMemberTable(parsed, grid).ok) {
    table = parsed;
    enabledAtomic.store(true);
    SerialPrintf("cluster: leading %d member(s), epoch %08x\n",
                 (int)table.count, (unsigned)epoch);
  } else if (stored.length() > 0) {
    SerialPrintln(F("cluster: stored member table invalid — leader disabled"));
  }
}

bool clusterLeaderEnabled() {
  return enabledAtomic.load(std::memory_order_relaxed);
}

// --- grid submit ----------------------------------------------------------------

// Slices `contentKey`-identified content into segments and stages the
// deltas. The key carries everything that changes the frame (mode, text,
// alignment, speed) so identical ticks dedup to nothing.
static void submitGrid(const String& contentKey, bool isClock,
                       const String& textOrTime, const String& date,
                       const String& alignment, int speed) {
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
  gridCommitAtMs = synced ? nowE + CLUSTER_COMMIT_LEAD_MS : 0;

  for (int i = 0; i < table.count; i++) {
    if (segs[i] == segments[i]) continue;  // that row didn't change
    segments[i] = segs[i];
    if (clusterMemberIsSelf(table.members[i])) {
      selfPending = true;
      selfText = segs[i];
      selfSpeed = speed;
      selfDueMs =
          millis() + clusterRenderDelayMs(gridCommitAtMs, nowE, synced);
    } else {
      runtimes[i].renderDirty = true;
    }
  }
}

void clusterLeaderSubmitText(const String& text, const String& alignment,
                             int speed) {
  submitGrid("t:" + alignment + ":" + String(speed) + ":" + text, false, text,
             "", alignment, speed);
}

void clusterLeaderSubmitClock(const String& timeText, const String& dateText,
                              const String& alignment, int speed) {
  submitGrid("c:" + alignment + ":" + String(speed) + ":" + timeText + "|" +
                 dateText,
             true, timeText, dateText, alignment, speed);
}

// --- clusterTask body -----------------------------------------------------------

// Applies a staged member-table swap: leave fan-out to dropped remote
// hosts (single best-effort attempt), runtime/segment reset, NVS persist.
static void applyStagedConfig() {
  String spec;
  {
    LeaderLock lock;
    if (!configPending) return;
    configPending = false;
    spec = configSpec;
  }

  ClusterMemberTable next;
  ClusterGrid grid;
  bool nextEnabled = clusterTableFromString(spec, next) && next.count > 0 &&
                     validateMemberTable(next, grid).ok;
  if (!nextEnabled && spec.length() > 0) {
    // The web boundary validated this same spec — a failure here means it
    // was clobbered in between; refuse rather than half-apply.
    SerialPrintln(F("cluster: staged config no longer valid — ignored"));
    return;
  }

  // A config swap invalidates any in-flight rollout (member indexes move);
  // same task as the upload pump, so the teardown is race-free (#276).
  rolloutCloseUpload();

  // Old remote hosts that are not in the new table get a leave.
  String leaveHosts[CLUSTER_MAX_MEMBERS];
  int leaveCount = 0;
  {
    LeaderLock lock;
    for (int i = 0; i < table.count; i++) {
      if (clusterMemberIsSelf(table.members[i])) continue;
      bool kept = false;
      for (int j = 0; j < next.count; j++) {
        if (strcmp(table.members[i].host, next.members[j].host) == 0) {
          kept = true;
          break;
        }
      }
      if (!kept) leaveHosts[leaveCount++] = table.members[i].host;
    }
    table = next;
    for (int i = 0; i < CLUSTER_MAX_MEMBERS; i++) {
      runtimes[i] = ClusterMemberRuntime{};
      segments[i] = "";
    }
    clusterRolloutReset(rollout);
    lastContentKey = "";  // force a fresh submit → renders for everyone
    selfPending = false;
    selfText = "";
    enabledAtomic.store(nextEnabled);
  }

  // NVS write from clusterTask: safe — the NVS layer is internally
  // mutex-protected, and the netTask-sole-flash-writer Hard rule governs
  // the storage LittleFS partition, not the nvs partition.
  leaderStore->putString(CLUSTER_KEY_MEMBERS, nextEnabled ? spec : String(""));
  SerialPrintf("cluster: config applied — %s, %d member(s)\n",
               nextEnabled ? "leading" : "disabled", (int)next.count);

  for (int i = 0; i < leaveCount; i++) {
    String body;
    clusterHttpRequest("http://" + leaveHosts[i] + "/cluster/leave", "", body);
    SerialPrintln("cluster: sent leave to " + leaveHosts[i]);
  }
}

// The leader's own row: due commitAt enqueue, plus the segment re-show
// that restores it after transients/reset-units (the follower gets the
// same from its gated clockTask).
static void serviceSelfRow() {
  LeaderLock lock;
  if (selfPending) {
    if ((int32_t)(millis() - selfDueMs) < 0) return;
    if (reflashInProgress(displaySnapshotGet().reflash)) return;  // retry
    if (displayEnqueue(makeShowTextCommand(selfText, "left", selfSpeed))) {
      selfPending = false;
    }
    return;
  }
  if (selfText.length() == 0) return;
  if (mqttNotificationActive()) return;  // overlay owns the row for now
  DisplaySnapshot snap = displaySnapshotGet();
  if (snap.busy || reflashInProgress(snap.reflash)) return;
  if (selfText == String(snap.currentText)) return;
  displayEnqueue(makeShowTextCommand(selfText, "left", selfSpeed));
}

struct MemberWorkItem {
  int index = -1;
  ClusterLeaderAction action = ClusterLeaderAction::None;
  String url;
  String body;
};

// Builds the next round of outbound calls under the lock; the blocking
// HTTP happens with the mutex RELEASED so submits/status reads never wait
// on a slow follower.
static int collectMemberWork(MemberWorkItem* items) {
  LeaderLock lock;
  int count = 0;
  uint32_t nowMs = millis();
  for (int i = 0; i < table.count; i++) {
    const ClusterMemberDef& def = table.members[i];
    if (clusterMemberIsSelf(def)) continue;
    // While a rollout upload streams to a member, its flash writes hog the
    // follower's async_tcp task — regular contact would time out and read
    // as failures. Skip it; the upload round-trip IS the contact (#276).
    if (rollout.phase == ClusterRolloutPhase::Uploading &&
        rollout.memberIndex == i) {
      continue;
    }
    ClusterLeaderAction action = clusterMemberNextAction(runtimes[i], nowMs);
    if (action == ClusterLeaderAction::None) continue;

    MemberWorkItem& item = items[count++];
    item.index = i;
    item.action = action;
    String host(def.host);
    switch (action) {
      case ClusterLeaderAction::Join:
        // Resolved only when a join is actually due — a per-tick
        // WiFi.localIP().toString() would churn the heap ~10x/s for a
        // value that only changes on DHCP renewal.
        item.url = "http://" + host + "/cluster/join";
        item.body = "leaderName=" + urlEncode(leaderDeviceName) +
                    "&leaderHost=" + urlEncode(WiFi.localIP().toString()) +
                    "&row=" + String((int)def.row) +
                    "&epoch=" + String((unsigned long)epoch);
        break;
      case ClusterLeaderAction::Render:
        // Seq is minted per POST attempt: an applied-but-timed-out render
        // retries with a higher seq and re-applies IDENTICAL content (a
        // visual no-op), while cross-ordered stale renders still lose the
        // seq race. A stable per-content seq would save that no-op but
        // couple retry state to content state for no wall-visible gain.
        item.url = "http://" + host + "/cluster/render";
        item.body = "epoch=" + String((unsigned long)epoch) +
                    "&seq=" + String((unsigned long)++seqCounter) +
                    "&text=" + urlEncode(segments[i]) +
                    "&speed=" + String(gridSpeed) +
                    "&commitAtMs=" + String((unsigned long long)gridCommitAtMs);
        break;
      default:  // Ping
        item.url = "http://" + host + "/cluster/ping";
        item.body = "";
        break;
    }
  }
  return count;
}

static void applyMemberResult(const MemberWorkItem& item, int status,
                              const String& body) {
  LeaderLock lock;
  ClusterMemberRuntime& m = runtimes[item.index];
  uint32_t nowMs = millis();

  if (status == 200) {
    bool wasDegraded = m.degraded;
    clusterMemberOnSuccess(m, nowMs);
    switch (item.action) {
      case ClusterLeaderAction::Join:
        m.joined = true;
        m.rev = clusterExtractJsonString(body, "rev");
        m.reportedWidth = clusterExtractJsonInt(body, "width", 0);
        // The handshake ends with a re-send of the current segment.
        m.renderDirty = segments[item.index].length() > 0;
        SerialPrintln("cluster: member " +
                      String(table.members[item.index].host) + " joined (rev " +
                      m.rev + ")");
        break;
      case ClusterLeaderAction::Render:
        m.renderDirty = false;
        break;
      default:
        break;
    }
    if (wasDegraded) {
      SerialPrintln("cluster: member " +
                    String(table.members[item.index].host) + " recovered");
    }
    return;
  }

  if (status == 409) {
    // The follower answered but rejected: for ping/render that means it
    // lost its membership — fresh join next round, no backoff (the link
    // is fine). A join 409 doesn't exist in the protocol; treat any other
    // 4xx/5xx as failure below.
    if (item.action != ClusterLeaderAction::Join) {
      clusterMemberOnSuccess(m, nowMs);
      clusterMemberOnNotClustered(m);
      return;
    }
  }

  bool wasDegraded = m.degraded;
  clusterMemberOnFailure(m, nowMs);
  if (m.degraded && !wasDegraded) {
    SerialPrintln("cluster: member " + String(table.members[item.index].host) +
                  " DEGRADED (no re-layout; its board falls back alone)");
  }
}

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
static std::atomic<bool> rolloutFactsFailed{false};
// Shared flash-read buffer (facts pass + upload pump never overlap).
static uint8_t rolloutBuf[4096];

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
  rolloutPart = part;
  rolloutImageLen = meta.image_len;
  SerialPrintf("cluster: rollout image ready — %u bytes, md5 %s (%u ms)\n",
               (unsigned)rolloutImageLen, rolloutMd5.c_str(),
               (unsigned)(millis() - t0));
  return true;
}

static void rolloutCloseUpload() {
  if (rolloutClient == nullptr) return;
  esp_http_client_close(rolloutClient);
  esp_http_client_cleanup(rolloutClient);
  rolloutClient = nullptr;
}

static bool rolloutOpenUpload(const String& host) {
  String url = clusterRolloutUrl(host, rolloutMd5, GIT_REV);
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  // Chunk writes ride the render fan-out's 1.5 s LAN bound so a stalled
  // follower can never freeze clusterTask beyond the already-accepted
  // worst case; only the finalize round-trip gets the long budget below.
  cfg.timeout_ms = CLUSTER_HTTP_TIMEOUT_MS;
  rolloutClient = esp_http_client_init(&cfg);
  if (rolloutClient == nullptr) return false;

  esp_http_client_set_method(rolloutClient, HTTP_METHOD_POST);
  esp_http_client_set_header(
      rolloutClient, "Content-Type",
      "multipart/form-data; boundary=" CLUSTER_ROLLOUT_BOUNDARY);
  String preamble = clusterRolloutMultipartPreamble();
  int total = (int)preamble.length() + (int)rolloutImageLen +
              (int)clusterRolloutMultipartTrailer().length();
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
  while (budget > 0 && rolloutOffset < rolloutImageLen) {
    uint32_t take = rolloutImageLen - rolloutOffset;
    if (take > sizeof(rolloutBuf)) take = sizeof(rolloutBuf);
    if (take > budget) take = budget;
    if (esp_partition_read(rolloutPart, rolloutOffset, rolloutBuf, take) !=
            ESP_OK ||
        esp_http_client_write(rolloutClient, (const char*)rolloutBuf, take) !=
            (int)take) {
      rolloutCloseUpload();
      LeaderLock lock;
      int target = rollout.memberIndex;
      SerialPrintln("cluster: rollout upload to " +
                    ((target >= 0 && target < table.count)
                         ? String(table.members[target].host)
                         : String("?")) +
                    " failed mid-stream");
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
  String trailer = clusterRolloutMultipartTrailer();
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

static void rolloutServiceTick() {
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
      candidate = clusterRolloutNextCandidate(table, runtimes, GIT_REV,
                                              rollout, millis());
    }
    if (candidate < 0) return;
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
    rolloutPumpUpload();
    return;
  }

  // WaitingRejoin: the normal supervision keeps re-joining the rebooted
  // member; the health gate watches the rev it comes back with.
  LeaderLock lock;
  if (target < 0 || target >= table.count) {
    clusterRolloutReset(rollout);  // config swap raced the wait — start over
    return;
  }
  ClusterRolloutWait wait =
      clusterRolloutCheckWait(rollout, runtimes[target].joined,
                              runtimes[target].rev, GIT_REV, millis());
  switch (wait) {
    case ClusterRolloutWait::Converged:
      SerialPrintln("cluster: " + String(table.members[target].host) +
                    " converged on " GIT_REV " — rollout advances");
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

void clusterLeaderTick() {
  if (leaderMutex == nullptr) return;
  applyStagedConfig();
  if (!clusterLeaderEnabled()) return;

  serviceSelfRow();

  // Sequential fan-out (spec: a bad follower strands only itself; renders
  // carry an absolute commitAt so ordering doesn't skew the flip). Known
  // one-tick limit: a member dying exactly during a render fan-out burns
  // its 1.5 s timeout before later members are reached, so THAT render's
  // commitAt may already be past for them (they flip immediately, ~1 s
  // early relative to the wall). Backoff keeps it from repeating; #278
  // drills it on the bench.
  static MemberWorkItem items[CLUSTER_MAX_MEMBERS];
  int count = collectMemberWork(items);
  for (int i = 0; i < count; i++) {
    String body;
    int status = clusterHttpRequest(items[i].url, items[i].body, body);
    applyMemberResult(items[i], status, body);
  }

  // Fleet firmware convergence (#276) — after the fan-out so renders and
  // pings always get their tick before an upload chunk does.
  rolloutServiceTick();
}

ClusterLeaderStatus clusterLeaderStatusGet() {
  ClusterLeaderStatus st;
  if (leaderMutex == nullptr) return st;
  LeaderLock lock;
  st.enabled = enabledAtomic.load();
  st.epoch = epoch;
  st.seq = seqCounter;
  st.memberCount = table.count;
  for (int i = 0; i < table.count; i++) {
    ClusterLeaderMemberStatus& out = st.members[i];
    out.host = table.members[i].host;
    out.row = table.members[i].row;
    out.col = table.members[i].col;
    out.width = table.members[i].width;
    out.joined = runtimes[i].joined || clusterMemberIsSelf(table.members[i]);
    out.degraded = runtimes[i].degraded;
    out.failures = runtimes[i].failures;
    out.rev = runtimes[i].rev;
    out.reportedWidth = runtimes[i].reportedWidth;
    out.updating = rollout.phase != ClusterRolloutPhase::Idle &&
                   rollout.memberIndex == i;
    out.updateBlocked = rollout.blocked[i];
  }
  st.rolloutPhase = clusterRolloutPhaseName(rollout.phase);
  if (rollout.memberIndex >= 0 && rollout.memberIndex < table.count) {
    st.rolloutHost = table.members[rollout.memberIndex].host;
  }
  st.rolloutSent = rollout.bytesSent;
  st.rolloutTotal = rollout.bytesTotal;
  st.rolloutImageFailed = rolloutFactsFailed.load(std::memory_order_relaxed);
  ClusterGrid grid;
  if (st.enabled && validateMemberTable(table, grid).ok) {
    st.gridRows = grid.rows;
    for (int r = 0; r < grid.rows; r++) st.gridCapacity += grid.rowWidth[r];
  }
  return st;
}

int clusterLeaderMirrorRows(String* rows, int& selfRowOut,
                            const String& selfRowText,
                            const String& alignment) {
  selfRowOut = 0;
  if (leaderMutex == nullptr || !enabledAtomic.load()) return 0;
  LeaderLock lock;
  for (int i = 0; i < table.count; i++) {
    if (clusterMemberIsSelf(table.members[i])) {
      selfRowOut = table.members[i].row;
      break;
    }
  }
  return clusterMirrorRows(table, segments, selfRowText,
                           displayAlignmentFromString(alignment), rows);
}

ClusterConfigVerdict clusterLeaderStageConfig(const String& membersSpec) {
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
  {
    LeaderLock lock;
    configSpec = membersSpec;
    configPending = true;
  }
  return {200, t.count > 0 ? "Cluster config staged" : "Cluster disabled"};
}
