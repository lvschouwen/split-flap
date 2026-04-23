#include "wifi_setup.h"

#include <Preferences.h>
#include <WiFi.h>

namespace {

constexpr const char* kNsName         = "wifi";
constexpr const char* kKeySsid        = "ssid";
constexpr const char* kKeyPass        = "pass";
constexpr const char* kKeyDeviceName  = "name";
constexpr const char* kDefaultName    = "splitflap-s3";
constexpr const char* kApSsid         = "SplitFlap-Setup";
constexpr uint32_t    kConnectTimeoutMs = 15000;

bool g_reboot_pending = false;

String loadPref(const char* key, const char* fallback = "") {
  Preferences p;
  if (!p.begin(kNsName, /*readOnly=*/true)) return String(fallback);
  String v = p.getString(key, fallback);
  p.end();
  return v;
}

void savePref(const char* key, const String& value) {
  Preferences p;
  if (!p.begin(kNsName, /*readOnly=*/false)) return;
  p.putString(key, value);
  p.end();
}

void bringUpAp() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(kApSsid);
  Serial.printf("[wifi] AP up: SSID=\"%s\" IP=%s\n",
                kApSsid, WiFi.softAPIP().toString().c_str());
}

}  // namespace

namespace wifiSetup {

void begin() {
  const String ssid = loadPref(kKeySsid);
  const String pass = loadPref(kKeyPass);

  if (ssid.isEmpty()) {
    Serial.println("[wifi] no saved SSID — starting AP for provisioning");
    bringUpAp();
    return;
  }

  Serial.printf("[wifi] STA connect: SSID=\"%s\"\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(deviceName().c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         (millis() - start) < kConnectTimeoutMs) {
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] STA connected: IP=%s RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println("[wifi] STA connect timed out — falling back to AP");
    WiFi.disconnect(true);
    bringUpAp();
  }
}

bool isStaConnected() { return WiFi.status() == WL_CONNECTED; }
bool isApMode()       { return WiFi.getMode() == WIFI_AP; }

IPAddress currentIP() {
  return isApMode() ? WiFi.softAPIP() : WiFi.localIP();
}

String ssid() {
  return isApMode() ? String(kApSsid) : WiFi.SSID();
}

String deviceName() {
  return loadPref(kKeyDeviceName, kDefaultName);
}

void saveCredentials(const String& new_ssid, const String& new_pass) {
  savePref(kKeySsid, new_ssid);
  savePref(kKeyPass, new_pass);
  Serial.printf("[wifi] saved SSID=\"%s\" (reboot pending)\n",
                new_ssid.c_str());
  g_reboot_pending = true;
}

void saveDeviceName(const String& name) {
  savePref(kKeyDeviceName, name);
  Serial.printf("[wifi] saved device name=\"%s\"\n", name.c_str());
}

void markRebootPending() { g_reboot_pending = true; }
bool rebootPending()     { return g_reboot_pending; }

void applyPendingReboot() {
  if (!g_reboot_pending) return;
  Serial.println("[wifi] rebooting...");
  delay(250);  // let HTTP response / log flush
  ESP.restart();
}

}  // namespace wifiSetup
