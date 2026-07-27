#include "WifiService.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "BuildVersion.h"
#include "ClockService.h"
#include "DeviceIdentity.h"
#include "HelpersSerialHandling.h"
#include "OtaService.h"
#include "Tasks.h"
#include "WifiPolicy.h"
#include "WifiScanJson.h"

// Wiring from setup(); only netTask touches the radio afterwards.
static MasterSettings* liveSettings = nullptr;
static SettingsStore* settingsStore = nullptr;
static String deviceName;

static WifiPolicyState policy;
static DNSServer dnsServer;
static bool portalUp = false;  // netTask-private: gates the DNS pump only

// Grace-delayed restart so the HTTP response that triggered it flushes
// (same 750 ms rule as ContentState's staged reboot).
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

static void onWifiStaEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;
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

void wifiServiceInit(MasterSettings& settings,
                     SettingsStore& store, const String& effectiveDeviceName) {
  stageMutex = xSemaphoreCreateMutex();
  if (stageMutex == nullptr) {
    Serial.println(F("FATAL: wifi stageMutex allocation failed"));
    abort();
  }
  liveSettings = &settings;
  settingsStore = &store;
  deviceName = effectiveDeviceName;
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

static void scheduleRestart(const __FlashStringHelper* why) {
  SerialPrintln(String(F("Rebooting: ")) + String(why));
  restartPending = true;
  restartRequestedAtMs = millis();
}

static void startJoin() {
  // esp_wifi keeps its credential copy in RAM only — our NVS namespace is
  // the single store, so the v1 persistent()/disconnect() foot-gun class
  // cannot exist here.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
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
  // AP_STA, not AP: the portal page's scan needs the STA half alive.
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  String apName = deviceName + AP_SUFFIX_SETUP;
  WiFi.softAP(apName.c_str());  // open AP, v1 portal parity
  // Catch-all DNS so the AP is reachable by any hostname. There is no
  // captive redirect any more: the page it pointed at went with the web UI,
  // and redirecting the OS sign-in sheet at a 404 is worse than not popping
  // it. Credentials go in over POST /wifi/config; portalRedirectUrl stays
  // empty, which keeps onNotFound on its plain-404 branch.
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());
  portalUp = true;
  // Fallback confirm (#305 moved the primary to setup() pre-inrush): no-op if
  // already confirmed, but retries should the pre-inrush otadata write have
  // failed. A portal boot is a healthy boot (#190).
  otaHealthConfirm();
  SerialPrintln("WiFi setup portal up: " + apName + " (" +
                WiFi.softAPIP().toString() + ")");
}

static void startOnline() {
  SerialPrintln("WiFi connected. IP: " + WiFi.localIP().toString());
  otaHealthConfirm();  // #305 fallback: primary confirm is setup() pre-inrush
  clockServiceApplyTz(*liveSettings);  // v1 parity: NTP kicked after join
  // Nothing is served on port 80 right now, so nothing advertises an HTTP
  // service; mDNS stays up purely so the board is reachable by name. The
  // _splitflap._tcp advert went with the cluster discovery it existed for.
  if (MDNS.begin(deviceName.c_str())) {
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

  if (restartPending && millis() - restartRequestedAtMs > RESTART_GRACE_MS) {
    Serial.flush();
    ESP.restart();
  }

  // #328: publish the event-task re-kick gate (atomic hand-off) — we only want
  // the STA-disconnect handler firing while Connected and not mid-reboot.
  staReconnectWanted.store(
      policy.phase == WifiPhase::Connected && !restartPending,
      std::memory_order_relaxed);
}
