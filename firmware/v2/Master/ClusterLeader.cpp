// ClusterLeader.cpp — leader-side cluster service (#273). Contract, lock
// ordering and producer rules in ClusterLeader.h; pure decisions in
// ClusterLeaderPolicy.h / ClusterLayout.h.

#include "ClusterLeader.h"

#include <LittleFS.h>
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
// #332: this board's own deviceRole for the self row of /cluster/status —
// seeded at webEndpointsInit, pushed by the settings drain (netTask), read
// under LeaderLock by statusFillLocked. The runtime table never carries it
// (the self row is never joined/pinged over HTTP).
static String leaderSelfRole;
static SettingsStore* leaderStore = nullptr;  // NVS writes: clusterTask only

// All guarded by leaderMutex.
static ClusterMemberTable table;
static ClusterMemberRuntime runtimes[CLUSTER_MAX_MEMBERS];
static String segments[CLUSTER_MAX_MEMBERS];
static uint32_t epoch = 0;
static uint32_t seqCounter = 0;      // minted per render POST, not per submit
static int gridSpeed = 80;           // web-scale speed of the last submit
static String gridAlignment = "left";  // alignment of the last submit
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
// #295 demote: a swap staged because another live leader owns our members
// must NOT leave-fan-out — that would kick them out of the NEW leader's
// cluster; they are not ours to release.
static bool configSuppressLeave = false;
// #294 digest: bumped only when the piggybacked content actually changed
// (the follower UI gates re-renders on it).
static uint32_t digestGen = 0;
static String digestLastBody;
// Fleet rollout (#276): sequencing state guarded by leaderMutex (status
// reads it); the live upload session below is clusterTask-private.
static ClusterRolloutState rollout;
// On-demand ESP-01 firmware relay (#304 Part B): push the stored
// /follower-fw.bin to a chosen esp01 row. Guarded by leaderMutex (status
// reads it); the file-stream session below is clusterTask-private. Mutually
// exclusive with the #276 auto-rollout — both run on clusterTask.
static FollowerPushState followerPush;
static bool followerPushPending = false;  // staged trigger
static String followerPushHost;           // staged target host

// Rollout upload session teardown (#276) — defined in the rollout section
// below; applyStagedConfig needs it before that.
static void rolloutCloseUpload();
static void followerPushCloseUpload();
// Status fill (leaderMutex held) — defined after the rollout statics it
// reads; collectMemberWork needs it for the ping digest (#294).
static void statusFillLocked(ClusterLeaderStatus& st);

// Wall-mirror change signal (#277): bumped whenever segments[] change
// (submit deltas, config swaps) so netTask's SSE tick can skip the mirror
// reconstruction — mutex + up to 8 String rebuilds — on the ~100 ms
// cadence when nothing moved. Relaxed: a missed bump is caught next tick.
static std::atomic<uint32_t> gridGenerationAtomic{0};

static uint64_t epochNowMs(bool& synced) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  synced = clockIsTimeSynced(tv.tv_sec);
  return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

// Fill a 32-byte cluster-wire auth key from the S3 hardware RNG (#313
// follow-on). esp_random() is the TRNG once the RF subsystem is up (always,
// by the time a join fires) — 8 draws for 256 bits.
static void clusterMintKey(uint8_t key[32]) {
  for (int i = 0; i < 8; i++) {
    uint32_t r = esp_random();
    key[i * 4] = (uint8_t)(r >> 24);
    key[i * 4 + 1] = (uint8_t)(r >> 16);
    key[i * 4 + 2] = (uint8_t)(r >> 8);
    key[i * 4 + 3] = (uint8_t)(r);
  }
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
  cfg.disable_auto_redirect = true;  // #313: never follow a member's redirect
                                     // off the LAN (SSRF/exfil hop)
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
      // 512: the #294 health keys grew the join/ping replies (~180 B worst
      // case today) — headroom so a truncated reply can't silently drop
      // health/rev facts.
      char buf[513];
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

// One blocking GET for a follower's log ring (#318 E). Separate from the POST
// helper above because the reply is far larger (the ESP-01 ring is 2 KB, vs
// the ~180 B join/ping replies) — a bigger read buffer, not the 513 B one.
// Returns -1 on transport failure, else the HTTP status; `outBody` gets the
// (bounded) reply. clusterTask-only caller, so the read buffer is static.
static int clusterHttpGetBody(const String& url, String& outBody) {
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.timeout_ms = CLUSTER_HTTP_TIMEOUT_MS;
  cfg.disable_auto_redirect = true;  // #313: never follow a member off-LAN
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) return -1;

  esp_http_client_set_method(client, HTTP_METHOD_GET);

  int status = -1;
  if (esp_http_client_open(client, 0) == ESP_OK &&
      esp_http_client_fetch_headers(client) >= 0) {
    status = esp_http_client_get_status_code(client);
    static char buf[2200];  // 2 KB follower ring + cursor line + headroom
    int got = esp_http_client_read_response(client, buf, sizeof(buf) - 1);
    if (got > 0) {
      buf[got] = '\0';
      outBody = buf;
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
      selfDueMs =
          millis() + clusterRenderDelayMs(gridCommitAtMs, nowE, synced);
    } else {
      runtimes[i].renderDirty = true;
    }
  }
  if (anyChanged) {
    gridGenerationAtomic.fetch_add(1, std::memory_order_relaxed);
  }
}

void clusterLeaderSubmitText(const String& text, const String& alignment,
                             int speed) {
  submitGrid("t:" + alignment + ":" + String(speed) + ":" + text, false, text,
             "", alignment, speed);
}

void clusterLeaderSetSelfRole(const String& role) {
  LeaderLock lock;
  leaderSelfRole = role;
}

void clusterLeaderSubmitClock(const String& timeText, const String& dateText,
                              const String& alignment, int speed) {
  submitGrid("c:" + alignment + ":" + String(speed) + ":" + timeText + "|" +
                 dateText,
             true, timeText, dateText, alignment, speed);
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
    runtimes[i].renderDirty = true;
    anyChanged = true;
  }
  if (anyChanged) {
    gridGenerationAtomic.fetch_add(1, std::memory_order_relaxed);
  }
}

// --- clusterTask body -----------------------------------------------------------

// Applies a staged member-table swap: leave fan-out to dropped remote
// hosts (single best-effort attempt), runtime/segment reset, NVS persist.
static void applyStagedConfig() {
  String spec;
  bool suppressLeave;
  {
    LeaderLock lock;
    if (!configPending) return;
    configPending = false;
    spec = configSpec;
    suppressLeave = configSuppressLeave;
    configSuppressLeave = false;
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
  // same task as the upload pump, so the teardown is race-free (#276). The
  // on-demand follower push (#304) is torn down the same way.
  rolloutCloseUpload();
  followerPushCloseUpload();

  // Old remote hosts that are not in the new table get a leave.
  String leaveHosts[CLUSTER_MAX_MEMBERS];
  uint8_t leaveKeys[CLUSTER_MAX_MEMBERS][32];
  bool leaveKeyValid[CLUSTER_MAX_MEMBERS];
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
      if (!kept && !suppressLeave) {
        // Capture the member's auth key BEFORE the runtime reset below so the
        // leave can be signed (#313 follow-on) — a keyed follower requires it.
        leaveKeyValid[leaveCount] = runtimes[i].hmacKeyValid;
        if (runtimes[i].hmacKeyValid) {
          memcpy(leaveKeys[leaveCount], runtimes[i].hmacKey, 32);
        }
        leaveHosts[leaveCount++] = table.members[i].host;
      }
    }
    table = next;
    for (int i = 0; i < CLUSTER_MAX_MEMBERS; i++) {
      runtimes[i] = ClusterMemberRuntime{};
      segments[i] = "";
    }
    clusterRolloutReset(rollout);
    followerPush = FollowerPushState{};
    followerPushPending = false;
    lastContentKey = "";  // force a fresh submit → renders for everyone
    selfPending = false;
    selfText = "";
    enabledAtomic.store(nextEnabled);
    gridGenerationAtomic.fetch_add(1, std::memory_order_relaxed);
  }

  // NVS write from clusterTask: safe — the NVS layer is internally
  // mutex-protected, and the netTask-sole-flash-writer Hard rule governs
  // the storage LittleFS partition, not the nvs partition.
  leaderStore->putString(CLUSTER_KEY_MEMBERS, nextEnabled ? spec : String(""));
  SerialPrintf("cluster: config applied — %s, %d member(s)\n",
               nextEnabled ? "leading" : "disabled", (int)next.count);

  for (int i = 0; i < leaveCount; i++) {
    String body;
    // Sign the leave (#313 follow-on) so a keyed follower honors it; an
    // un-keyed (pre-HMAC) follower ignores the extra params and falls back to
    // #313 source-IP acceptance.
    String post;
    if (leaveKeyValid[i]) {
      bool synced = false;
      uint64_t ts = epochNowMs(synced);
      String msg = clusterHmacLeaveMsg(ts);
      post = "ts=" + clusterU64ToStr(ts) + "&mac=" +
             clusterHmacSign(leaveKeys[i], msg);
    }
    clusterHttpRequest("http://" + leaveHosts[i] + "/cluster/leave", post,
                       body);
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

// Round-robin cursor for the one-op-per-tick fan-out (#320). clusterTask-
// private: only collectMemberWork touches it, always under LeaderLock.
static int fanoutCursor = -1;

// #321 graceful-reboot hold. Before an intentional restart the leader stamps a
// hold (ms) into its digest and force-pings every member once so a designated
// backup doesn't take over during the reboot window. rebootHoldMs also rides
// the normal ping digest while set. Cross-task: the reboot drain waits on
// rebootHoldSent.
static const uint32_t CLUSTER_REBOOT_HOLD_MS = 45000;  // < follower clamp (60s)
static uint32_t rebootHoldMs = 0;
static std::atomic<bool> rebootHoldPending{false};
static std::atomic<bool> rebootHoldSent{false};

// Builds the next round of outbound calls under the lock; the blocking
// HTTP happens with the mutex RELEASED so submits/status reads never wait
// on a slow follower. At most ONE member is contacted per tick (#320): a
// promote-time catch-up makes every member due at once, and doing them all in
// one tick — one being a dead host burning the full HTTP timeout — starved
// core-0 idle past the 5 s task watchdog. clusterFanoutNext round-robins so a
// stuck dead host can't monopolize the fan-out.
static int collectMemberWork(MemberWorkItem* items) {
  // #337: capture the leader's mode for the digest BEFORE taking LeaderLock.
  // webDisplayContentSnapshot() takes WebStateLock, and the documented order is
  // WebStateLock -> clusterMutex — nesting it the other way deadlocks against
  // netTask's settings drain (WebStateLock -> clusterLeaderSubmit*).
  String leaderMode = webDisplayContentSnapshot().deviceMode;
  LeaderLock lock;
  int count = 0;
  uint32_t nowMs = millis();
  // One epoch-ms stamp for every signed request this round (#313 follow-on).
  bool signSynced = false;
  uint64_t signTs = epochNowMs(signSynced);

  bool needsAction[CLUSTER_MAX_MEMBERS] = {false};
  for (int i = 0; i < table.count; i++) {
    if (clusterMemberIsSelf(table.members[i])) continue;
    // While a rollout upload streams to a member, its flash writes hog the
    // follower's async_tcp task — regular contact would time out and read
    // as failures. Skip it; the upload round-trip IS the contact (#276).
    if (rollout.phase == ClusterRolloutPhase::Uploading &&
        rollout.memberIndex == i) {
      continue;
    }
    // Same hazard for the on-demand follower-image push (#304): don't ping a
    // row whose async_tcp task is busy taking our /firmware/master stream.
    if (followerPush.phase == FollowerPushPhase::Uploading &&
        followerPush.memberIndex == i) {
      continue;
    }
    needsAction[i] =
        clusterMemberNextAction(runtimes[i], nowMs) != ClusterLeaderAction::None;
  }

  int pick = clusterFanoutNext(fanoutCursor, needsAction, table.count);
  if (pick >= 0) {
    const ClusterMemberDef& def = table.members[pick];
    ClusterLeaderAction action = clusterMemberNextAction(runtimes[pick], nowMs);
    MemberWorkItem& item = items[count++];
    item.index = pick;
    item.action = action;
    String host(def.host);
    switch (action) {
      case ClusterLeaderAction::Join: {
        // Resolved only when a join is actually due — a per-tick
        // WiFi.localIP().toString() would churn the heap ~10x/s for a
        // value that only changes on DHCP renewal.
        item.url = "http://" + host + "/cluster/join";
        // Mint the per-member wire-auth key once (#313 follow-on); reused
        // across rejoins so the key stays stable while the membership does.
        if (!runtimes[pick].hmacKeyValid) {
          clusterMintKey(runtimes[pick].hmacKey);
          runtimes[pick].hmacKeyValid = true;
        }
        item.body = "leaderName=" + urlEncode(leaderDeviceName) +
                    "&leaderHost=" + urlEncode(WiFi.localIP().toString()) +
                    "&row=" + String((int)def.row) +
                    "&epoch=" + String((unsigned long)epoch) +
                    "&key=" + clusterKeyToHex(runtimes[pick].hmacKey);
        break;
      }
      case ClusterLeaderAction::Render: {
        // Seq is minted per POST attempt: an applied-but-timed-out render
        // retries with a higher seq and re-applies IDENTICAL content (a
        // visual no-op), while cross-ordered stale renders still lose the
        // seq race. A stable per-content seq would save that no-op but
        // couple retry state to content state for no wall-visible gain.
        item.url = "http://" + host + "/cluster/render";
        uint32_t seq = ++seqCounter;
        item.body = "epoch=" + String((unsigned long)epoch) +
                    "&seq=" + String((unsigned long)seq) +
                    "&text=" + urlEncode(segments[pick]) +
                    "&speed=" + String(gridSpeed) +
                    "&commitAtMs=" + String((unsigned long long)gridCommitAtMs);
        // Sign the content (#313 follow-on) once a key is negotiated — the
        // canonical fields mirror what the follower reconstructs from the
        // wire params, so a captured mac can't be reused for other text.
        if (runtimes[pick].hmacKeyValid) {
          String msg = clusterHmacRenderMsg(signTs, epoch, seq, segments[pick],
                                            gridSpeed, gridCommitAtMs);
          item.body += "&ts=" + clusterU64ToStr(signTs) + "&mac=" +
                       clusterHmacSign(runtimes[pick].hmacKey, msg);
        }
        break;
      }
      default:  // Ping
        item.url = "http://" + host + "/cluster/ping";
        item.body = "";  // digest piggyback attached below, once per round
        break;
    }
  }

  // #294 rung 2: the digest rides every outbound ping — built at most once
  // per round (~10 s cadence per member), gen-bumped only on real change.
  bool anyPing = false;
  for (int i = 0; i < count; i++) {
    if (items[i].action == ClusterLeaderAction::Ping) anyPing = true;
  }
  if (anyPing) {
    ClusterLeaderStatus st;
    statusFillLocked(st);
    String mirror[CLUSTER_MAX_MEMBERS];
    DisplaySnapshot snap = displaySnapshotGet();
    int rowCount =
        clusterMirrorRows(table, segments, String(snap.currentText),
                          displayAlignmentFromString(gridAlignment), mirror);
    String body = clusterBuildDigest(0, leaderDeviceName,
                                     WiFi.localIP().toString(),
                                     clusterTableToString(table), mirror,
                                     rowCount, st, rebootHoldMs, leaderMode);
    if (body != digestLastBody) {
      digestLastBody = body;
      digestGen++;
    }
    // Splice the real gen over the 0 sentinel the comparison body carries.
    String digest = "{\"gen\":" + String((unsigned long)digestGen) +
                    body.substring(strlen("{\"gen\":0"));
    String encoded = urlEncode(digest);
    for (int i = 0; i < count; i++) {
      if (items[i].action != ClusterLeaderAction::Ping) continue;
      items[i].body =
          "digest=" + encoded + "&you=" + String(items[i].index);
      // Sign the ping per member (#313 follow-on): its own key, so the
      // digest/you piggyback rides an authenticated request.
      int mi = items[i].index;
      if (runtimes[mi].hmacKeyValid) {
        // Sign the raw (un-encoded) digest + this member's index too (#313
        // follow-on HIGH#2): the follower persists them as promote-critical
        // state, so the mac must bind them against an on-path swap.
        String msg = clusterHmacPingMsg(signTs, digest, mi);
        items[i].body += "&ts=" + clusterU64ToStr(signTs) + "&mac=" +
                         clusterHmacSign(runtimes[mi].hmacKey, msg);
      }
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
        // Absent = same platform as this leader (#297); an ESP-01 row
        // reports "esp01" and is excluded from firmware convergence.
        m.plat = clusterExtractJsonString(body, "plat");
        // Absent = pre-#332 peer. NOT the same as an explicit "display":
        // "" keeps the old width-0-preferred slot (tier 0), "display" at
        // width 0 ranks with spare — so a mixed-rev cluster is unchanged.
        m.role = clusterExtractJsonString(body, "role");
        m.reportedWidth = clusterExtractJsonInt(body, "width", 0);
        // The handshake ends with a re-send of the current segment.
        m.renderDirty = segments[item.index].length() > 0;
        SerialPrintln("cluster: member " +
                      String(table.members[item.index].host) + " joined (rev " +
                      m.rev + (m.hmacKeyValid ? ", authenticated)" : ")"));
        break;
      case ClusterLeaderAction::Render:
        m.renderDirty = false;
        break;
      default:
        break;
    }
    if (item.action != ClusterLeaderAction::Render) {
      // Join and ping replies carry the #294 health keys; a pre-#294
      // follower parses to invalid (strip hidden, never zero faults). The
      // rev refresh keeps the fleet-convergence fact alive across OUR
      // reboots without waiting for a re-join.
      clusterParsePingHealth(body, m.health);
      String rev = clusterExtractJsonString(body, "rev");
      if (rev.length() > 0) m.rev = rev;
      String plat = clusterExtractJsonString(body, "plat");
      if (plat.length() > 0) m.plat = plat;
      // #332: ping refresh keeps a live role change (role picker POST on the
      // member) flowing into the succession tiers without a re-join.
      String role = clusterExtractJsonString(body, "role");
      if (role.length() > 0) m.role = role;
      m.reportedWidth = clusterExtractJsonInt(body, "width", m.reportedWidth);
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
    // is fine).
    if (item.action != ClusterLeaderAction::Join) {
      clusterMemberOnSuccess(m, nowMs);
      clusterMemberOnNotClustered(m);
      return;
    }
    // A join 409 with the other-leader marker (#295 sticky leadership):
    // the member belongs to a promoted successor — WE are the stale
    // leader. Demote: stage a table wipe with the leave fan-out
    // suppressed (the members are the new leader's to keep).
    if (clusterJoinRejectedOtherLeader(body)) {
      SerialPrintln("cluster: member " +
                    String(table.members[item.index].host) +
                    " is clustered to another live leader (" +
                    clusterExtractJsonString(body, "leaderHost") +
                    ") — DEMOTING, this board joins the wall as a member");
      configSpec = "";
      configPending = true;
      configSuppressLeave = true;
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
static String rolloutBoundary;  // md5-derived — never occurs in the image (#292)
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
  rolloutBoundary = clusterRolloutBoundary(rolloutMd5);
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
      // Stand down while an on-demand follower push owns the flash (#304).
      if (followerPush.phase != FollowerPushPhase::Idle) return;
      candidate = clusterRolloutNextCandidate(table, runtimes, GIT_REV,
                                              CLUSTER_LEADER_PLAT, rollout,
                                              millis());
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

static void followerPushCloseUpload() {
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
  fpushFile = LittleFS.open(FOLLOWER_IMAGE_PATH, FILE_READ);
  if (!fpushFile) { followerPushCloseUpload(); return false; }
  String url = clusterRolloutUrl(host, fpushMd5, fpushRev.c_str());
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.timeout_ms = CLUSTER_HTTP_TIMEOUT_MS;
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

// caller holds LeaderLock.
static void followerPushFinishLocked(FollowerPushResult result,
                                     const String& host, const String& what) {
  followerPushFinish(followerPush, result);
  SerialPrintln("cluster: follower-image push to " + host + " " + what);
}

static void followerPushPump() {
  uint32_t budget = CLUSTER_ROLLOUT_CHUNK_PER_TICK;
  while (budget > 0 && fpushOffset < fpushLen) {
    uint32_t take = fpushLen - fpushOffset;
    if (take > sizeof(rolloutBuf)) take = sizeof(rolloutBuf);
    if (take > budget) take = budget;
    if (fpushFile.read(rolloutBuf, take) != (int)take ||
        esp_http_client_write(fpushClient, (const char*)rolloutBuf, take) !=
            (int)take) {
      followerPushCloseUpload();
      LeaderLock lock;
      int t = followerPush.memberIndex;
      String host = (t >= 0 && t < table.count) ? String(table.members[t].host)
                                                : String("?");
      followerPushFinishLocked(FollowerPushResult::Failed, host,
                               "failed mid-stream");
      return;
    }
    fpushOffset += take;
    budget -= take;
  }
  {
    LeaderLock lock;
    followerPush.bytesSent = fpushOffset;
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

static void followerPushServiceTick() {
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

// #321 reboot-hold delivery. Built once when a reboot is announced, then sent
// ONE member per clusterLeaderTick (see the tick) — the naive "ping everyone in
// one call" is the exact core-0-starving burst #320 was written to eliminate
// (up to 7 dead hosts × the 1.5 s HTTP timeout ≫ the 5 s task watchdog). Non-
// degraded members are queued FIRST so the reachable rows — the ones that could
// actually spuriously take over — get the hold within the drain's bounded wait
// even if some members are dead and burn their timeouts at the tail.
struct RebootHoldTarget {
  String host;
  String body;
};
static RebootHoldTarget rebootHoldTargets[CLUSTER_MAX_MEMBERS];
static int rebootHoldCount = 0;
static int rebootHoldCursor = 0;

static void rebootHoldBuildTargets() {
  String leaderMode = webDisplayContentSnapshot().deviceMode;  // #337: before the lock
  LeaderLock lock;
  rebootHoldCount = 0;
  rebootHoldCursor = 0;
  if (!clusterLeaderEnabled()) return;
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
  String digest = "{\"gen\":" + String((unsigned long)digestGen) +
                  body.substring(strlen("{\"gen\":0"));
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
static void followerLogPullTick() {
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

void clusterLeaderTick() {
  if (leaderMutex == nullptr) return;
  applyStagedConfig();
  if (!clusterLeaderEnabled()) return;

  // #321: deliver a pending reboot-hold ONE member per tick — watchdog-safe,
  // same cadence as the normal fan-out (the 100 ms inter-tick vTaskDelay feeds
  // core-0 idle between every blocking send). While it drains, this tick does
  // ONLY the hold send so a tail of dead members can never stack two blocking
  // ops in one tick. The reboot drain waits (bounded) on rebootHoldSent.
  if (rebootHoldPending.exchange(false)) {
    rebootHoldBuildTargets();
    if (rebootHoldCount == 0) rebootHoldSent.store(true);  // nobody to tell
  }
  if (rebootHoldCursor < rebootHoldCount) {
    String resp;
    clusterHttpRequest(
        "http://" + rebootHoldTargets[rebootHoldCursor].host + "/cluster/ping",
        rebootHoldTargets[rebootHoldCursor].body, resp);
    rebootHoldCursor++;
    if (rebootHoldCursor >= rebootHoldCount) {
      rebootHoldSent.store(true);
      SerialPrintln("cluster: announced reboot-hold to " +
                    String(rebootHoldCount) + " member(s)");
    }
    return;  // one blocking op this tick
  }

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
    wdtFeed();  // #314: bounds the blocking send (one member per tick, #320)
  }

  // Fleet firmware convergence (#276) — after the fan-out so renders and
  // pings always get their tick before an upload chunk does.
  rolloutServiceTick();
  wdtFeed();  // #314: a #276 firmware chunk write can block
  // On-demand ESP-01 firmware relay (#304) — mutually exclusive with the
  // auto-rollout above (each guards on the other's phase).
  followerPushServiceTick();
  wdtFeed();  // #314: #304 relay chunk write
  // Pull one ESP-01 row's log into the fleet log (#318 E) — after the pushes
  // so a firmware stream always wins the tick over a log poll.
  followerLogPullTick();
  wdtFeed();  // #314: #318E log poll
}

// Fills the status snapshot — leaderMutex HELD by the caller. The self
// row's health folds straight from the display snapshot (#294); remote
// rows carry their last ping-reply health (leaderMutex -> snapshotMutex is
// the allowed lock order).
static void statusFillLocked(ClusterLeaderStatus& st) {
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
    out.failures = runtimes[i].failures;
    out.rev = runtimes[i].rev;
    out.plat = runtimes[i].plat;
    out.role = runtimes[i].role;
    out.reportedWidth = runtimes[i].reportedWidth;
    out.updating = rollout.phase != ClusterRolloutPhase::Idle &&
                   rollout.memberIndex == i;
    out.updateBlocked = rollout.blocked[i];
    out.hmac = runtimes[i].hmacKeyValid;  // signing to this member (#313)
    if (clusterMemberIsSelf(table.members[i])) {
      out.rev = GIT_REV;
      // #332: the self row is never joined/pinged, so its runtime role stays
      // empty — inject our live deviceRole like GIT_REV above (review MED).
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
    // demote's suppress-leave flag (review HIGH), so it resets here.
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
