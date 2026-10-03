#include "WifiService.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <WiFi.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "BootTrace.h"  // #504
#include "BuildVersion.h"
#include "ClockService.h"
#include "DeviceIdentity.h"
#include "HelpersSerialHandling.h"
#include "OtaService.h"
#include "RebootCause.h"  // #432
#include "ReflashPlan.h"  // reflashInProgress: no retry restart mid-reflash
#include "Tasks.h"
#include "WebEndpoints.h"
#include "WifiPolicy.h"
#include "WifiScanJson.h"
#include "WifiTxPolicy.h"

// Wiring from setup(); only netTask touches the radio afterwards.
static AsyncWebServer* webServer = nullptr;
static MasterSettings* liveSettings = nullptr;
static SettingsStore* settingsStore = nullptr;
static String deviceName;

static WifiPolicyState policy;
// Failed-join tally across software resets (WifiPolicy.h). netTask only.
RTC_NOINIT_ATTR static WifiJoinRetryRecord joinRetry;
// Reason code of the last STA disconnect while joining: the one clue a failed
// join leaves, carried into the reboot cause. Event task -> netTask.
static std::atomic<uint8_t> lastJoinDisconnectReason{0};
static DNSServer dnsServer;
static bool portalUp = false;  // netTask-private: gates the DNS pump only

// Grace-delayed restart so the HTTP response that triggered it flushes
// (same 750 ms rule as WebEndpoints' pendingReboot).
static bool restartPending = false;
static uint32_t restartRequestedAtMs = 0;
static const uint32_t RESTART_GRACE_MS = 750;

// Handler->tick staging, all under one mutex (async_tcp task vs netTask).
static SemaphoreHandle_t stageMutex = nullptr;
static bool configStaged = false;
static String stagedSsid, stagedPass;
static bool resetStaged = false;
static bool scanRequested = false;
static String scanJson;
static bool scanInFlight = false;
static String portalRedirectUrl;  // "" until the portal AP is up

struct StageLock {
  StageLock() { xSemaphoreTake(stageMutex, portMAX_DELAY); }
  ~StageLock() { xSemaphoreGive(stageMutex); }
  StageLock(const StageLock&) = delete;
  StageLock& operator=(const StageLock&) = delete;
};

// #328 supplement — event-driven STA re-kick, on top of the policy watchdog.
// WiFi.setAutoReconnect(true) is supposed to own reconnection, but its esp_wifi
// handler can give up or wedge after an AP power-cycle. This re-issues a
// connect on every STA disconnect while we intend to be online, throttled so it
// neither floods the log nor fights the driver's own retries. It recovers most
// drops in seconds; the WifiPolicy watchdog reboot stays the guaranteed backstop
// if even this cannot. Runs in the Arduino event task (not netTask): the reads
// "do we want the link up?" answer from an atomic that netTask publishes each
// tick — same cross-task discipline as the stageMutex-guarded fields, without a
// lock the driver's event task must never block on.
static const uint32_t WIFI_RECONNECT_KICK_MS = 5000;
static uint32_t lastReconnectKickMs = 0;                 // event-task-private
static std::atomic<bool> staReconnectWanted{false};      // netTask -> event task
// #505: radio in a high-draw phase (join, reconnect, OTA write) — netTask ->
// displayTask's motion gate. Starts busy: boot is a join until proven otherwise.
static std::atomic<bool> radioBusyFlag{true};

bool wifiRadioBusy() { return radioBusyFlag.load(std::memory_order_relaxed); }

static void onWifiStaEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;
  lastJoinDisconnectReason.store((uint8_t)info.wifi_sta_disconnected.reason,
                                 std::memory_order_relaxed);
  // Only re-kick when a live association is what we want (Connected, no reboot
  // pending). Portal/Boot/Joining leave STA retries to WifiPolicy.
  if (!staReconnectWanted.load(std::memory_order_relaxed)) return;
  uint32_t now = millis();
  if (now - lastReconnectKickMs < WIFI_RECONNECT_KICK_MS) return;
  lastReconnectKickMs = now;
  SerialPrintf("wifi: STA disconnected (reason %u) — re-issuing connect\n",
               (unsigned)info.wifi_sta_disconnected.reason);
  WiFi.reconnect();
}

void wifiServiceInit(AsyncWebServer& server, MasterSettings& settings,
                     SettingsStore& store, const String& effectiveDeviceName) {
  stageMutex = xSemaphoreCreateMutex();
  if (stageMutex == nullptr) {
    Serial.println(F("FATAL: wifi stageMutex allocation failed"));
    abort();
  }
  webServer = &server;
  liveSettings = &settings;
  settingsStore = &store;
  deviceName = effectiveDeviceName;
  policy.failedJoinBoots = wifiJoinRetryDecode(joinRetry);
  if (policy.failedJoinBoots > 0) {
    SerialPrintf("wifi: join attempt %u of %u (the boot(s) before this one "
                 "did not join)\n",
                 (unsigned)policy.failedJoinBoots + 1,
                 (unsigned)WIFI_JOIN_ATTEMPTS);
  }
  // #328: supplement setAutoReconnect with the event-driven re-kick above.
  WiFi.onEvent(onWifiStaEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
}

void wifiStagePortalConfig(const String& ssid, const String& pass) {
  StageLock lock;
  stagedSsid = ssid;
  stagedPass = pass;
  configStaged = true;
}

void wifiStageReset() {
  StageLock lock;
  resetStaged = true;
}

void wifiStageScan() {
  StageLock lock;
  scanRequested = true;
  scanJson = "";  // a new request invalidates the cached list
}

String wifiScanResultJson() {
  StageLock lock;
  return scanJson;
}

String wifiPortalRedirectUrl() {
  StageLock lock;
  return portalRedirectUrl;
}

// --- tick helpers (netTask context) ------------------------------------------

static void scheduleRestart(const String& why) {
  SerialPrintln(String(F("Rebooting: ")) + why);
  // #432: the log line above outlives the flash-log ring only as this NVS
  // breadcrumb (served as lastRebootCause next boot). Tick context = netTask,
  // so the write is legal here.
  rebootCauseStamp(why);
  restartPending = true;
  restartRequestedAtMs = millis();
}

static void scheduleRestart(const __FlashStringHelper* why) {
  scheduleRestart(String(why));
}

// #505/#506: TX bursts are the S3's own current peak on the shared 5 V rail —
// a plain software restart came back as a brownout with every stepper idle,
// and a boot that restored full power once online died on a corrupted
// instruction fetch right after. The radio therefore starts every boot at
// the lowest level and only ever moves one level at a time (WifiTxPolicy.h,
// #507). Can only apply after the interface starts, so the RF calibration
// inside WiFi.mode() still runs at full power.
static WifiTxState txState;
static int8_t txAppliedIndex = -1;  // -1 = nothing applied yet
static bool txReassertLogged = false;
static bool txRefusedLogged = false;  // one line per refusal streak
static uint32_t txNextStepMs = 0;
static const uint8_t TX_INDEX_UNKNOWN = 0xFF;
static std::atomic<uint8_t> txPublishedIndex{TX_INDEX_UNKNOWN};
static std::atomic<uint32_t> txConfirmedAtMs{0};  // 0 = never (#515)

void wifiNoteConfirmedTraffic() {
  uint32_t now = millis();
  txConfirmedAtMs.store(now != 0 ? now : 1, std::memory_order_relaxed);
}

int wifiTxPowerDbm10() {
  uint8_t index = txPublishedIndex.load(std::memory_order_relaxed);
  return index == TX_INDEX_UNKNOWN ? 0 : wifiTxLevelDbm10(index);
}

static bool txPowerApply(uint8_t index) {
  if (!WiFi.setTxPower((wifi_power_t)wifiTxLevelRaw(index))) return false;
  txAppliedIndex = (int8_t)index;
  txPublishedIndex.store(index, std::memory_order_relaxed);
  return true;
}

// Runs the ladder for the given phase and pushes a changed level to the
// radio. Returns false when the radio refused the level: the ladder is then
// held at the level the radio really has, so a later success is still a
// single step.
static bool txPowerStep(WifiTxPhase phase) {
  WifiTxInput in;
  in.phase = phase;
  in.linkUp = WiFi.status() == WL_CONNECTED;
  in.rssiDbm = in.linkUp ? WiFi.RSSI() : 0;
  // RSSI() reads 0 when the link fell between the two calls; fed to the
  // ladder it would count as a perfect signal.
  if (in.linkUp && in.rssiDbm >= 0) return true;
  in.unitsIdle = !displaySnapshotGet().busy;
  uint32_t nowMs = millis();
  in.trafficConfirmed = wifiTxTrafficFresh(
      txConfirmedAtMs.load(std::memory_order_relaxed), nowMs);
  uint8_t index = wifiTxPolicyStep(txState, in, nowMs);

  if ((int8_t)index == txAppliedIndex) {
    // The driver owns the value; a silent reset there would put the radio
    // back at the SDK default with nothing here noticing.
    if ((int8_t)WiFi.getTxPower() != wifiTxLevelRaw(index)) {
      if (!txReassertLogged) {
        SerialPrintln("wifi: TX power drifted from the ladder — re-asserting");
        txReassertLogged = true;
      }
      return txPowerApply(index);
    }
    return true;
  }

  int8_t was = txAppliedIndex;
  if (!txPowerApply(index)) {
    if (!txRefusedLogged) {
      SerialPrintln("wifi: TX power level refused by the radio");
      txRefusedLogged = true;
    }
    if (was >= 0) txState.index = (uint8_t)was;
    return false;
  }
  txRefusedLogged = false;
  char line[112];
  int dbm10 = wifiTxLevelDbm10(index);
  if (was < 0) {
    snprintf(line, sizeof(line), "wifi: TX power %d.%d dBm", dbm10 / 10,
             dbm10 % 10);
  } else {
    // #515: a step says why it happened. The estimate is the one the policy
    // judged, i.e. at the level it stepped FROM.
    int was10 = wifiTxLevelDbm10((uint8_t)was);
    int n = snprintf(line, sizeof(line), "wifi: TX power %d.%d -> %d.%d dBm (%s",
                     was10 / 10, was10 % 10, dbm10 / 10, dbm10 % 10,
                     wifiTxStepReasonName(txState.lastReason));
    if (in.linkUp && n > 0 && n < (int)sizeof(line)) {
      n += snprintf(line + n, sizeof(line) - n, ", rssi %d, uplink est %d dBm",
                    in.rssiDbm, wifiTxUplinkRaw(in.rssiDbm, (uint8_t)was) / 4);
    }
    if (n > 0 && n < (int)sizeof(line)) snprintf(line + n, sizeof(line) - n, ")");
  }
  SerialPrintln(line);
  return true;
}

static void startJoin() {
  // esp_wifi keeps its credential copy in RAM only — our NVS namespace is
  // the single store, so the v1 persistent()/disconnect() foot-gun class
  // cannot exist here.
  bootTraceMarkStage(BOOT_STAGE_JOIN);  // #504
  lastJoinDisconnectReason.store(0, std::memory_order_relaxed);  // 0 = none seen
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  // Before begin(): no join burst above the ladder. A refusal is retried
  // briefly; joining at the SDK default beats a board with no network.
  for (int attempt = 0; attempt < 3 && !txPowerStep(WifiTxPhase::Joining);
       attempt++) {
    delay(20);
  }
  WiFi.setHostname(deviceName.c_str());
  WiFi.setAutoReconnect(true);
  SerialPrintln("Joining WiFi \"" + liveSettings->wifiSsid + "\" ...");
  if (liveSettings->wifiPass.length() > 0) {
    WiFi.begin(liveSettings->wifiSsid.c_str(), liveSettings->wifiPass.c_str());
  } else {
    WiFi.begin(liveSettings->wifiSsid.c_str());  // open network
  }
}

static void startPortal() {
  // The boot after the portal starts over with a full set of join attempts.
  wifiJoinRetryEncode(joinRetry, 0);
  // AP_STA, not AP: the portal page's scan needs the STA half alive.
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  txPowerStep(WifiTxPhase::Portal);  // before the AP starts beaconing
  String apName = deviceName + AP_SUFFIX_SETUP;
  WiFi.softAP(apName.c_str());  // open AP, v1 portal parity
  // Catch-all DNS: every hostname resolves to us; onNotFound() then
  // redirects to /wifi-setup, which is what pops the OS captive sheet.
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());
  portalUp = true;
  {
    // Stage the captive-redirect target here so onNotFound never has to
    // touch the WiFi class from the async_tcp task (async-context rule).
    StageLock lock;
    portalRedirectUrl = "http://" + WiFi.softAPIP().toString() + "/wifi-setup";
  }
  webEndpointsStart(*webServer);  // LWIP is up on the AP netif
  // Fallback confirm (#305 moved the primary to setup() pre-inrush): no-op if
  // already confirmed, but retries should the pre-inrush otadata write have
  // failed. A portal boot is a healthy boot (#190).
  otaHealthConfirm();
  SerialPrintln("WiFi setup portal up: " + apName + " (" +
                WiFi.softAPIP().toString() + ")");
}

static void startOnline() {
  // #515: which access point and how well we hear it — a mesh hands a board
  // to a different node across reconnects, and the TX ladder follows that.
  SerialPrintln("WiFi connected. IP: " + WiFi.localIP().toString() + ", AP " +
                WiFi.BSSIDstr() + " ch " + String(WiFi.channel()) + ", rssi " +
                String(WiFi.RSSI()));
  bootTraceMarkStage(BOOT_STAGE_ONLINE);  // #504
  wifiJoinRetryEncode(joinRetry, 0);
  webEndpointsStart(*webServer);
  otaHealthConfirm();  // #305 fallback: primary confirm is setup() pre-inrush
  clockServiceApplyTz(*liveSettings);  // v1 parity: NTP kicked after join
  if (MDNS.begin(deviceName.c_str())) {
    MDNS.addService("http", "tcp", 80);
    // Cluster discovery (#274): every v2 master advertises itself so a
    // leader's Cluster card can browse for candidates. TXT width is the
    // at-advertise-time hint only (0 if the boot probe hasn't finished);
    // the join handshake stays the authoritative width fact.
    MDNS.addService("splitflap", "tcp", 80);
    MDNS.addServiceTxt("splitflap", "tcp", "name", deviceName.c_str());
    MDNS.addServiceTxt("splitflap", "tcp", "rev", GIT_REV);
    MDNS.addServiceTxt("splitflap", "tcp", "width",
                       String((int)displaySnapshotGet().displayWidth).c_str());
    SerialPrintln("mDNS up: " + deviceName + ".local");
  } else {
    SerialPrintln(F("mDNS start failed"));
  }
}

static void pumpScan() {
  bool wantScan;
  {
    StageLock lock;
    wantScan = scanRequested;
  }
  if (wantScan && !scanInFlight) {
    WiFi.scanNetworks(/*async=*/true);
    scanInFlight = true;
    StageLock lock;
    scanRequested = false;
  }
  if (!scanInFlight) return;

  int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  scanInFlight = false;
  if (n < 0) {  // WIFI_SCAN_FAILED
    StageLock lock;
    // A request queued mid-scan keeps the "" pending sentinel: its fresh
    // scan starts next tick and pollers must not see this stale result.
    if (!scanRequested) scanJson = "{\"networks\":[]}";
    return;
  }
  // Bound the adapter copy: buildWifiScanJson caps its output at
  // WIFI_SCAN_JSON_MAX anyway, but entries[] must not scale with a dense
  // environment's AP count. Results come back RSSI-sorted from the SDK, so
  // taking the first 2*MAX keeps every candidate the cap could show unless
  // 20+ same-ssid duplicates crowd the prefix (the 2x slack is a heuristic
  // for dedup losses, not a guarantee). Static — ~1 KB does not belong on
  // netTask's 4 KB stack; netTask is the sole toucher.
  static WifiScanEntry entries[2 * WIFI_SCAN_JSON_MAX];
  int keep = n;
  if (keep > 2 * WIFI_SCAN_JSON_MAX) keep = 2 * WIFI_SCAN_JSON_MAX;
  for (int i = 0; i < keep; i++) {
    entries[i].ssid = WiFi.SSID(i);
    entries[i].rssi = WiFi.RSSI(i);
    entries[i].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  String json = buildWifiScanJson(entries, keep);
  WiFi.scanDelete();
  for (int i = 0; i < keep; i++) entries[i].ssid = "";  // release heap
  SerialPrintln("WiFi scan finished: " + String(n) + " network(s)");
  StageLock lock;
  if (!scanRequested) scanJson = json;  // see the failure-path comment above
}

// --- the tick -----------------------------------------------------------------

WifiPhase wifiServicePhase() { return policy.phase; }

void wifiServiceTick() {
  if (liveSettings == nullptr) return;  // init hasn't run

  if (portalUp) dnsServer.processNextRequest();
  pumpScan();

  // Snapshot staging under the lock; act on the copies outside it.
  bool doReset, submitted;
  String ssid, pass;
  {
    StageLock lock;
    doReset = resetStaged;
    submitted = configStaged;
    ssid = stagedSsid;
    pass = stagedPass;
  }

  if (doReset && !restartPending) {
    clearWifiCredentials(*settingsStore);
    SerialPrintln(F("WiFi credentials erased."));
    scheduleRestart(F("reset-wifi — next boot opens the setup portal"));
    StageLock lock;
    resetStaged = false;  // consumed — must not replay if a restart is ever cancelled
  }

  if (!restartPending) {
    WifiAction action =
        wifiPolicyStep(policy, millis(), WiFi.status() == WL_CONNECTED,
                       liveSettings->wifiSsid.length() > 0, submitted);
    if ((action == WifiAction::RetryJoin || action == WifiAction::Reboot) &&
        !wifiRestartMayProceed(
            action, policy.phase,
            reflashInProgress(displaySnapshotGet().reflash))) {
      action = WifiAction::None;  // asked again next tick, once the job is done
    }
    switch (action) {
      case WifiAction::StartJoin:
        startJoin();
        break;
      case WifiAction::StartPortal:
        startPortal();
        break;
      case WifiAction::StartOnline:
        startOnline();
        break;
      case WifiAction::RetryJoin: {
        uint8_t attempt = (uint8_t)(policy.failedJoinBoots + 1);
        wifiJoinRetryEncode(joinRetry, attempt);
        uint8_t reason =
            lastJoinDisconnectReason.load(std::memory_order_relaxed);
        scheduleRestart(
            String(F("WiFi join failed (attempt ")) + String(attempt) +
            F(" of ") + String(WIFI_JOIN_ATTEMPTS) +
            F(", last disconnect reason ") +
            (reason == 0 ? String(F("none")) : String(reason)) +
            F(") — rebooting to retry"));
        break;
      }
      case WifiAction::SaveAndReboot: {
        saveWifiCredentials(*settingsStore, ssid, pass);
        scheduleRestart(F("new WiFi configuration saved"));
        StageLock lock;
        configStaged = false;  // consumed, same rationale as resetStaged
        break;
      }
      case WifiAction::Reboot:
        // Same action, two origins: a Connected-phase Reboot is the #328
        // reconnect watchdog (link wedged after an AP power-cycle); otherwise
        // it is the portal-timeout retry. Distinguish them in the log.
        scheduleRestart(policy.phase == WifiPhase::Connected
                            ? F("WiFi link lost too long — rebooting to re-join (#328)")
                            : F("setup portal timed out — retrying stored WiFi"));
        break;
      case WifiAction::None:
        break;
    }
  }

  // #507: the TX ladder, once a second. Boot has no radio to set yet.
  if (policy.phase != WifiPhase::Boot &&
      (int32_t)(millis() - txNextStepMs) >= 0) {
    txNextStepMs = millis() + 1000;
    txPowerStep(policy.phase == WifiPhase::Connected
                    ? WifiTxPhase::Online
                    : policy.phase == WifiPhase::Portal ? WifiTxPhase::Portal
                                                        : WifiTxPhase::Joining);
  }

  if (restartPending && millis() - restartRequestedAtMs > RESTART_GRACE_MS) {
    Serial.flush();
    ESP.restart();
  }

  // #328: publish the event-task re-kick gate (atomic hand-off) — we only want
  // the STA-disconnect handler firing while Connected and not mid-reboot.
  staReconnectWanted.store(
      policy.phase == WifiPhase::Connected && !restartPending,
      std::memory_order_relaxed);
  bool linkUp = WiFi.status() == WL_CONNECTED;
  radioBusyFlag.store(policy.phase == WifiPhase::Boot ||
                          policy.phase == WifiPhase::Joining ||
                          (policy.phase == WifiPhase::Connected && !linkUp) ||
                          Update.isRunning(),
                      std::memory_order_relaxed);
}
