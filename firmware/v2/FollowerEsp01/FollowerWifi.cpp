// FollowerWifi.cpp — WiFi/identity/SNTP/mDNS glue (#298). Contract in
// FollowerWifi.h; the portal flow is v1's ServiceWifiFunctions.ino trimmed.

// Deliberately first, in this order (v1 rule — conflicts with other libs).
#include <DNSServer.h>
#include <ESPAsyncWiFiManager.h>

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <time.h>

#include "BuildVersion.h"
#include "FollowerConfig.h"
#include "FollowerJson.h"  // FOLLOWER_PLAT
#include "FollowerWeb.h"   // isPendingReboot
#include "FollowerWifi.h"

String effectiveDeviceName;
bool isWifiConfigured = false;

static DNSServer dnsServer;

static bool waitForWifiConnected(int timeoutSeconds) {
  for (int elapsed = 0; elapsed < timeoutSeconds; elapsed++) {
    if (WiFi.status() == WL_CONNECTED) {
      SerialPrint(F("connected. IP Address: "));
      SerialPrintln(WiFi.localIP());
      return true;
    }
    SerialPrint('.');
    delay(1000);
  }
  SerialPrintln(F(" timed out"));
  return false;
}

static bool tryJoinKnownWifi(int timeoutSeconds) {
  WiFi.mode(WIFI_STA);
  WiFi.hostname(effectiveDeviceName.c_str());
  WiFi.setAutoReconnect(true);
  if (WiFi.SSID().length() == 0) {
    SerialPrintln(F("No WiFi credentials persisted in SDK flash"));
    return false;
  }
  SerialPrint(F("Joining known WiFi "));
  WiFi.begin();  // no args = the SDK-persisted credentials
  return waitForWifiConnected(timeoutSeconds);
}

void wifiInit(AsyncWebServer& server) {
  effectiveDeviceName = "split-flap-" + String(ESP.getChipId(), HEX);
  SerialPrint(F("Device identity: "));
  SerialPrintln(effectiveDeviceName);

  if (tryJoinKnownWifi(30)) {
    isWifiConfigured = true;
    return;
  }

  SerialPrintln(F("Starting WiFi setup portal..."));
  // Function-local static: the manager registers handlers on the shared
  // server, so it must outlive this call (v1 keeps its instance global).
  static AsyncWiFiManager wifiManager(&server, &dnsServer);
  wifiManager.setSaveConfigCallback([]() {
    // Reboot after the portal saves so the async server rebinds cleanly to
    // the STA interface (v1 rule).
    isPendingReboot = true;
  });
  wifiManager.setConfigPortalTimeout(300);
  wifiManager.setConnectTimeout(30);
  if (wifiManager.startConfigPortal(
          (effectiveDeviceName + "-setup").c_str())) {
    isWifiConfigured = true;
    return;
  }
  // Portal window elapsed unconfigured: reboot and retry (the router may
  // just have been down).
  SerialPrintln(F("Setup portal timed out — rebooting to retry"));
  isPendingReboot = true;
}

void wifiServicesInit(int rowWidth) {
  // Epoch-only SNTP: commitAt flips stay in unison with the wall; unsynced
  // renders fall back to immediate (FollowerPolicy rule). No blocking wait
  // — the row is useful before sync.
  configTime(0, 0, "pool.ntp.org");

  if (MDNS.begin(effectiveDeviceName.c_str())) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("splitflap", "tcp", 80);
    MDNS.addServiceTxt("splitflap", "tcp", "name",
                       effectiveDeviceName.c_str());
    MDNS.addServiceTxt("splitflap", "tcp", "rev", GIT_REV);
    MDNS.addServiceTxt("splitflap", "tcp", "width",
                       String(rowWidth).c_str());
    // #297: the plat tag is what keeps the S3 leader's firmware rollout
    // away from this board and tags it in the discovery scan.
    MDNS.addServiceTxt("splitflap", "tcp", "plat", FOLLOWER_PLAT);
    SerialPrintln(F("mDNS responder started"));
  } else {
    SerialPrintln(F("Error setting up mDNS responder"));
  }
}
