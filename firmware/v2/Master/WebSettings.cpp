// Settings/message form, reboot, WiFi portal flow and MQTT broker discovery —
// split from WebEndpoints.cpp (#338); async-context rules in
// WebEndpoints.cpp's header: handlers stage (pendingPost, pendingReboot,
// WifiService, the discover flag), the netTask drain mutates.

#include "WebEndpoints.h"
#include "WebEndpointsInternal.h"

#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>

#include "ClusterFollower.h"
#include "HelpersSerialHandling.h"
#include "MdnsDiscovery.h"
#include "MqttService.h"
#include "ReflashPlan.h"
#include "SettingsJson.h"
#include "Tasks.h"
#include "WifiService.h"

// MQTT broker mDNS discovery staging (#224, v1 /mqtt/discover contract):
// the POST arms the flag, netTask's drain runs the blocking query and
// caches the JSON, the GET answers 202 while pending / 200 from the cache.
static bool mqttDiscoverPending = false;
static String mqttDiscoverResultJson;

void webSettingsRegister(AsyncWebServer& server) {
  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest* request) {
    // Full gather extracted to buildCurrentSettingsJson() so GET /status
    // renders the identical settings object (#307).
    request->send(200, "application/json", buildCurrentSettingsJson());
  });

  // --- settings/message form (v1 wire contract) ----------------------------
  server.on("/", HTTP_POST, [](AsyncWebServerRequest* request) {
    // All-or-nothing per POST: parse into a local post, merge only when
    // every provided field validated (v1 #128/#153 semantics, pure logic
    // in PendingSettingsPost.h — natively tested).
    PendingSettingsPost local;
    bool submissionError = false;

    int params = request->params();
    for (int i = 0; i < params; i++) {
      const AsyncWebParameter* p = request->getParam(i);
      if (!p->isPost()) continue;
      SettingsParamResult result =
          stageSettingsParam(local, p->name(), p->value());
      if (result == SettingsParamResult::Invalid) {
        SerialPrintln("Invalid value for '" + p->name() + "': " + p->value());
        submissionError = true;
      }
    }
    if (!settingsPostConsistent(local)) {
      SerialPrintln(F("Transient dwell provided without transient text."));
      submissionError = true;
    }

    // Per-card fetch() saves (#128) send ajax=1 and want a status code
    // instead of the classic redirect.
    bool isAjax = request->hasParam("ajax", true);

    if (submissionError) {
      if (isAjax) request->send(400, "text/plain", F("invalid"));
      else request->redirect("/?invalid-submission=true");
      return;
    }

    // Message/transient sends become display commands at drain time; report
    // a full queue now instead of accepting one that would be dropped. (The
    // stub worker drains instantly, so this only fires if something wedges.)
    // The reflash gate (#205) applies only to the display-bound part — pure
    // settings saves don't touch the display queue and stay allowed.
    if ((local.inputTextProvided || local.transientTextProvided) &&
        reflashInProgress(displaySnapshotGet().reflash)) {
      if (isAjax) {
        request->send(409, "text/plain", F("reflash in progress"));
      } else {
        request->redirect("/?display-busy=true");
      }
      return;
    }
    // Cluster producer gate (#272): a clustered follower's text/mode belong
    // to the leader — 409; the banner explains why. Transients stay allowed
    // (they are the calibration vehicle — maintenance is local), and so do
    // pure settings saves.
    if ((local.inputTextProvided || local.deviceModeProvided) &&
        clusterFollowerViewGet().gated) {
      if (isAjax) request->send(409, "text/plain", F("clustered"));
      else request->redirect("/?clustered=true");
      return;
    }
    if ((local.inputTextProvided || local.transientTextProvided) &&
        displayQueueFull()) {
      SerialPrintln(F("Display command queue full — message rejected."));
      if (isAjax) request->send(503, "text/plain", F("display busy"));
      else request->redirect("/?display-busy=true");
      return;
    }

    // Verdict against the live values, then stage; the apply itself runs in
    // webEndpointsLoop(). Verdict + merge sit in one locked section so the
    // comparison can't race a half-applied post.
    bool needsReboot;
    bool deviceNameChanged;
    {
      WebStateLock lock;
      needsReboot = settingsPostNeedsReboot(local, *liveSettings);
      deviceNameChanged = local.deviceNameProvided &&
                          local.deviceName != liveSettings->deviceName;
      mergeSettingsPost(pendingPost, local);
    }

    // Device renamed (#125): flag only from async context — mqttTask blanks
    // the old identity's retained discovery configs before the reboot swaps
    // identities.
    if (deviceNameChanged) mqttRequestDiscoveryClear();

    if (isAjax) {
      request->send(200, "text/plain", needsReboot ? F("ok-reboot") : F("ok"));
    } else {
      request->redirect(deviceNameChanged ? "/?device-name-saved=true"
                        : needsReboot     ? "/?mqtt-saved=true"
                                          : "/");
    }
  });

  // POST, not GET (v1 #145): state-changing actions must not be triggerable
  // by a drive-by <img src> on the LAN.
  server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest* request) {
    SerialPrintln(F("Reboot requested from web UI"));
    request->send(200, "text/plain",
                  "Reboot pending — this takes a few seconds. Reload the home "
                  "page afterwards.");
    WebStateLock lock;
    pendingReboot = true;
    rebootRequestedAtMs = millis();
  });

  // --- WiFi portal + credentials (#188) -------------------------------------
  // Handlers stage into WifiService; all radio/NVS work runs in netTask's
  // wifiServiceTick(). (The /wifi-setup portal page itself is a PROGMEM
  // asset — served from WebContent.cpp.)
  server.on("/wifi/scan", HTTP_POST, [](AsyncWebServerRequest* request) {
    wifiStageScan();
    request->send(200, "text/plain", F("scanning"));
  });
  server.on("/wifi/scan", HTTP_GET, [](AsyncWebServerRequest* request) {
    String json = wifiScanResultJson();
    if (json.length() == 0) {
      request->send(202, "text/plain", F("pending"));
    } else {
      request->send(200, "application/json", json);
    }
  });

  server.on("/wifi/config", HTTP_POST, [](AsyncWebServerRequest* request) {
    SerialPrintln(F("WiFi credentials submitted from web"));
    if (!request->hasParam("ssid", true)) {
      request->send(400, "text/plain", F("invalid"));
      return;
    }
    String ssid = request->getParam("ssid", true)->value();
    String pass = request->hasParam("pass", true)
                      ? request->getParam("pass", true)->value()
                      : String();
    if (!isValidWifiSsidValue(ssid, LEN_WIFI_SSID) ||
        !isValidWifiPasswordValue(pass, LEN_WIFI_PASSWORD)) {
      SerialPrintln(F("WiFi config rejected: invalid ssid/password"));
      request->send(400, "text/plain", F("invalid"));
      return;
    }
    wifiStagePortalConfig(ssid, pass);
    request->send(200, "text/plain", F("ok-reboot"));
  });

  // POST, not GET (v1 #145): erasing WiFi credentials is the sharpest edge.
  server.on("/reset-wifi", HTTP_POST, [](AsyncWebServerRequest* request) {
    SerialPrintln(F("WiFi credential erase requested from web UI"));
    request->send(200, "text/plain",
                  "WiFi credentials erased. The display is rebooting into its "
                  "setup portal — reconnect to the device's setup AP to "
                  "configure a network.");
    wifiStageReset();
  });

  // Captive-portal hook: while the setup portal is up, the DNS catch-all
  // funnels every hostname here and this redirect pops the OS sign-in sheet
  // (/generate_204, /hotspot-detect.html, /connecttest.txt all land in
  // onNotFound). Outside portal mode: a plain 404, as v1.
  server.onNotFound([](AsyncWebServerRequest* request) {
    String redirectUrl = wifiPortalRedirectUrl();  // "" = portal not up
    if (redirectUrl.length() > 0) {
      request->redirect(redirectUrl);
    } else {
      request->send(404, "text/plain", F("Not found"));
    }
  });

  // --- MQTT broker mDNS discovery (#224, v1 /mqtt/discover contract) --------
  // POST arms the staged flag (re-POST while pending is a no-op — the flag
  // is the re-entry guard); the blocking MDNS.queryService pass runs from
  // netTask's drain, never here.
  server.on("/mqtt/discover", HTTP_POST, [](AsyncWebServerRequest* request) {
    {
      WebStateLock lock;
      if (!mqttDiscoverPending) {
        mqttDiscoverResultJson = "";
        mqttDiscoverPending = true;
      }
    }
    request->send(200, "text/plain", F("Broker discovery started"));
  });
  server.on("/mqtt/discover", HTTP_GET, [](AsyncWebServerRequest* request) {
    // Snapshot under the lock, send outside it (file convention — send()
    // schedules through AsyncTCP and its lock domain must not nest ours).
    bool pending;
    String json;
    {
      WebStateLock lock;
      pending = mqttDiscoverPending;
      json = mqttDiscoverResultJson;
    }
    if (pending) {
      request->send(202, "text/plain", F("Discovery running"));
      return;
    }
    if (json.length() == 0) {
      request->send(404, "text/plain", F("No discovery has run yet"));
      return;
    }
    request->send(200, "application/json", json);
  });
}

// MQTT broker discovery drain (#224): the blocking MDNS.queryService pass
// runs here in netTask — mDNS takes LWIP locks, which must never nest inside
// webStateMutex. The ~1-2 s stall of netTask while it runs is the v1 loop()
// behavior.
void webSettingsDiscoverLoop() {
  bool discoverDue;
  {
    WebStateLock lock;
    discoverDue = mqttDiscoverPending;
  }
  if (!discoverDue) return;
  MdnsBrokerCandidate candidates[4];
  size_t count = 0;
  int n = MDNS.queryService("mqtt", "tcp");
  for (int i = 0; i < n && count < 4; i++) {
    MdnsBrokerCandidate& c = candidates[count++];
    c.name = normalizeMdnsHostname(MDNS.hostname(i));
    IPAddress a = MDNS.address(i);
    c.ip = a == IPAddress() ? String() : a.toString();
    c.advertisedPort = MDNS.port(i);
    c.fromHomeAssistant = false;
  }
  if (count == 0) {
    // No broker advertised itself — Home Assistant's zeroconf record
    // locates the host running the Mosquitto add-on (v1 fallback).
    n = MDNS.queryService("home-assistant", "tcp");
    for (int i = 0; i < n && count < 4; i++) {
      MdnsBrokerCandidate& c = candidates[count++];
      c.name = normalizeMdnsHostname(MDNS.hostname(i));
      IPAddress a = MDNS.address(i);
      c.ip = a == IPAddress() ? String() : a.toString();
      c.advertisedPort = MDNS.port(i);
      c.fromHomeAssistant = true;
    }
  }
  String json = buildDiscoverJson(candidates, count);
  SerialPrintf("MQTT discover: %u candidate(s)\n", (unsigned)count);
  WebStateLock lock;
  mqttDiscoverResultJson = json;
  mqttDiscoverPending = false;
}
