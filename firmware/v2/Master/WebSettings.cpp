// The settings document and the WiFi portal flow — split from
// WebEndpoints.cpp (#338); async-context rules in WebEndpoints.cpp's header:
// handlers stage into WifiService, the netTask drain mutates.

#include "WebEndpoints.h"
#include "WebEndpointsInternal.h"

#include <ESPAsyncWebServer.h>

#include "HelpersSerialHandling.h"
#include "WifiService.h"

void webSettingsRegister(AsyncWebServer& server) {
  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", buildCurrentSettingsJson());
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
}
