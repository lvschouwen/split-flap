// v2 rescue app (#195) — the break-glass image in the factory partition.
//
// Entered via the #193 factory-reset pin (GPIO 4 held low 5 s through a
// reset erases otadata; the bootloader then boots this slot) or Master's
// POST /firmware/rescue-boot. Job: get a working master image back onto the
// board without USB. Reads identity + WiFi credentials from NVS (READ-ONLY —
// rescue never writes settings), joins STA for 30 s, else opens the
// "<deviceName>-rescue" AP with a captive redirect, and serves RescueWeb's
// upload/exit surface. Single-loop by design: no task decomposition, no
// display/I2C, no MQTT, no clock, no settings UI, no OTA-of-itself.

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_ota_ops.h>

#include "BuildVersion.h"
#include "RescueIdentity.h"
#include "RescueWeb.h"
#include "RescueWifiPolicy.h"

static const char* NAME_PREFIX = "split-flap";

static AsyncWebServer webServer(80);
static DNSServer dnsServer;
static RescuePolicyState policy;
static String deviceName;
static String wifiSsid, wifiPass;
static bool apUp = false;

// Same identity derivation as Master: last 3 octets of the efuse base MAC.
static uint32_t chipIdFromEfuseMac() {
  const uint64_t mac = ESP.getEfuseMac();
  return (uint32_t)((mac >> 24) & 0xFFFFFF);
}

void setup() {
  Serial.begin(115200);
  delay(2000);  // native USB-CDC needs a moment before the first prints land

  // Read-only NVS peek: begin() fails on a virgin device (namespace never
  // created) — that's fine, empty credentials just mean straight to the AP.
  Preferences prefs;
  String storedName, slotRec0, slotRec1;
  bool nvsOk = prefs.begin("splitflap", /*readOnly=*/true);
  if (nvsOk) {
    storedName = prefs.getString("deviceName", "");
    wifiSsid = prefs.getString("wifiSsid", "");
    wifiPass = prefs.getString("wifiPass", "");
    // #200: Master's per-slot OTA confirm records — /rescue/exit ranks by
    // these (the app-descriptor build stamp is constant under pioarduino
    // hybrid builds and cannot order images).
    slotRec0 = prefs.getString("slotRec0", "");
    slotRec1 = prefs.getString("slotRec1", "");
    prefs.end();
  }
  deviceName =
      resolveDeviceName(nvsOk, storedName, NAME_PREFIX, chipIdFromEfuseMac());

  const esp_partition_t* running = esp_ota_get_running_partition();
  Serial.println();
  Serial.println(F("split-flap v2 RESCUE — factory-slot recovery image (#195)"));
  Serial.printf("build: %s, running from: %s\n", GIT_REV,
                running ? running->label : "?");
  if (running == nullptr ||
      running->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY) {
    // Bench aid: someone flashed this image into an OTA slot (or `pio run
    // -t upload` put it at app0). Everything still works, but say so.
    Serial.println(F("WARNING: not running from the factory slot — this "
                     "image belongs at 0x830000"));
  }
  Serial.printf("identity: %s\n", deviceName.c_str());
  Serial.printf("wifi: %s\n", wifiSsid.length()
                                  ? ("join \"" + wifiSsid + "\"").c_str()
                                  : "no stored credentials -> rescue AP");

  rescueWebInit(webServer, deviceName, slotRec0, slotRec1);
}

static void startJoin() {
  // esp_wifi credential copy stays in RAM (same rule as Master): NVS is the
  // single store and rescue must never write it.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(deviceName.c_str());
  WiFi.setAutoReconnect(true);
  Serial.println("Joining WiFi \"" + wifiSsid + "\" ...");
  if (wifiPass.length() > 0) {
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  } else {
    WiFi.begin(wifiSsid.c_str());  // open network
  }
}

static void startAp() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  String apName = deviceName + AP_SUFFIX_RESCUE;
  WiFi.softAP(apName.c_str());  // open AP, same trust model as v1/-setup
  // Catch-all DNS + captive redirect: joining the AP from a phone pops the
  // rescue page without typing an address.
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());
  rescueWebSetCaptiveRedirect("http://" + WiFi.softAPIP().toString() + "/");
  apUp = true;
  rescueWebStart(webServer);
  Serial.println("Rescue AP up: " + apName + " (" +
                 WiFi.softAPIP().toString() + ")");
}

static void startOnline() {
  Serial.println("WiFi connected. IP: " + WiFi.localIP().toString());
  rescueWebStart(webServer);
  if (MDNS.begin(deviceName.c_str())) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS up: " + deviceName + ".local");
  }
}

void loop() {
  if (apUp) dnsServer.processNextRequest();

  RescueAction action = rescuePolicyStep(
      policy, millis(), WiFi.status() == WL_CONNECTED, wifiSsid.length() > 0);
  switch (action) {
    case RescueAction::StartJoin:
      startJoin();
      break;
    case RescueAction::StartAp:
      startAp();
      break;
    case RescueAction::StartOnline:
      startOnline();
      break;
    case RescueAction::None:
      break;
  }

  rescueWebTick();  // staged reboot after upload/exit
  delay(20);
}
