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

// Short LAN timeout: a follower answers in tens of ms; anything past this
// is a failure the backoff machinery owns.
static const int CLUSTER_HTTP_TIMEOUT_MS = 1500;

SemaphoreHandle_t leaderMutex = nullptr;

// Producer gate only — carries NO ordering for the mutex-guarded state
// below (relaxed loads). Never read table/segments/runtimes off a bare
// enabledAtomic check; always take LeaderLock.
std::atomic<bool> enabledAtomic{false};
String leaderDeviceName;          // set once in init, read-only after
// #332: this board's own deviceRole for the self row of /cluster/status —
// seeded at webEndpointsInit, pushed by the settings drain (netTask), read
// under LeaderLock by statusFillLocked. The runtime table never carries it
// (the self row is never joined/pinged over HTTP).
String leaderSelfRole;
// #342: the leader's POSIX tz, sent in every join body (clock fallback on
// esp01 rows). Seeded at init + pushed by the settings drain, like the role.
String leaderTzPosix;
SettingsStore* leaderStore = nullptr;  // NVS writes: clusterTask only

// All guarded by leaderMutex.
ClusterMemberTable table;
ClusterMemberRuntime runtimes[CLUSTER_MAX_MEMBERS];
String segments[CLUSTER_MAX_MEMBERS];
uint32_t epoch = 0;
uint32_t seqCounter = 0;      // minted per render POST, not per submit
int gridSpeed = 80;           // web-scale speed of the last submit
String gridAlignment = "left";  // alignment of the last submit
uint64_t gridCommitAtMs = 0;  // shared flip deadline of the last submit
String lastContentKey;        // submit dedup
// Leader's own row staging (same commitAt clock the followers get).
bool selfPending = false;
String selfText;
int selfSpeed = 0;
uint32_t selfDueMs = 0;
// Config swap staging (validated at the web boundary, applied here).
bool configPending = false;
String configSpec;
// #295 demote: a swap staged because another live leader owns our members
// must NOT leave-fan-out — that would kick them out of the NEW leader's
// cluster; they are not ours to release.
bool configSuppressLeave = false;
// #294 digest: bumped only when the piggybacked content actually changed
// (the follower UI gates re-renders on it).
uint32_t digestGen = 0;
String digestLastBody;
// Fleet rollout (#276): sequencing state guarded by leaderMutex (status
// reads it); the live upload session below is clusterTask-private.
ClusterRolloutState rollout;
// On-demand ESP-01 firmware relay (#304 Part B): push the stored
// /follower-fw.bin to a chosen esp01 row. Guarded by leaderMutex (status
// reads it); the file-stream session below is clusterTask-private. Mutually
// exclusive with the #276 auto-rollout — both run on clusterTask.
FollowerPushState followerPush;
bool followerPushPending = false;  // staged trigger
String followerPushHost;           // staged target host

// Wall-mirror change signal (#277): bumped whenever segments[] change
// (submit deltas, config swaps) so netTask's SSE tick can skip the mirror
// reconstruction — mutex + up to 8 String rebuilds — on the ~100 ms
// cadence when nothing moved. Relaxed: a missed bump is caught next tick.
std::atomic<uint32_t> gridGenerationAtomic{0};

uint64_t epochNowMs(bool& synced) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  synced = clockIsTimeSynced(tv.tv_sec);
  return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

// Fill a 32-byte cluster-wire auth key from the S3 hardware RNG (#313
// follow-on). esp_random() is the TRNG once the RF subsystem is up (always,
// by the time a join fires) — 8 draws for 256 bits.
void clusterMintKey(uint8_t key[32]) {
  for (int i = 0; i < 8; i++) {
    uint32_t r = esp_random();
    key[i * 4] = (uint8_t)(r >> 24);
    key[i * 4 + 1] = (uint8_t)(r >> 16);
    key[i * 4 + 2] = (uint8_t)(r >> 8);
    key[i * 4 + 3] = (uint8_t)(r);
  }
}

String urlEncode(const String& value) {
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
int clusterHttpRequest(const String& url, const String& postBody,
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
int clusterHttpGetBody(const String& url, String& outBody) {
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
// Reboot-hold delivery state (#321) — built by ClusterLeaderMaintenance.cpp,
// drained one member per tick below. Cross-task: the reboot drain waits on
// rebootHoldSent.
RebootHoldTarget rebootHoldTargets[CLUSTER_MAX_MEMBERS];
int rebootHoldCount = 0;
int rebootHoldCursor = 0;
uint32_t rebootHoldMs = 0;
std::atomic<bool> rebootHoldPending{false};
std::atomic<bool> rebootHoldSent{false};

// Rollout seams shared across the Rollout/FollowerPush/Status TUs. The
// facts-failed latch is atomic because the status snapshot surfaces it from
// the web task; the 4 KB buffer is shared because the #276 flash stream and
// the #304 file stream are mutually exclusive by design.
std::atomic<bool> rolloutFactsFailed{false};
uint8_t rolloutBuf[4096];
bool rolloutFollowerSource = false;
String rolloutFollowerTargetRev;
bool rolloutRescueTriggered = false;

void clusterLeaderTick() {
  if (leaderMutex == nullptr) return;
  applyStagedConfig();
  if (!clusterLeaderEnabled()) return;

  // #385 netif-recovery epoch: failures while our own STA was down never
  // counted (leader-offline gate in applyMemberResult) — on the up-edge every
  // member gets a fresh benefit-of-the-doubt window so a >30 s leader outage
  // can't degrade the whole wall on its first post-reconnect timeout.
  static bool lastNetifUp = true;
  bool netifUp = WiFi.status() == WL_CONNECTED;
  if (netifUp && !lastNetifUp) {
    LeaderLock lock;
    uint32_t nowMs = millis();
    for (int i = 0; i < table.count; i++) {
      clusterMemberStampContactEpoch(runtimes[i], nowMs);
    }
  }
  lastNetifUp = netifUp;

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
