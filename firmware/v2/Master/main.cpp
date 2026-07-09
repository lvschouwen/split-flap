// v2 master — Phase 1 (#58, epic #183).
//
// Boots on an ESP32-S3 devkit, loads settings from NVS, prints an identity
// banner, registers the full v1 web endpoint surface (#186), and starts the
// dual-core task skeleton (#187): display domain on core 1, network domain
// on core 0, queues and snapshot copies in between. setup() is the
// composition root; loop() survives only as the observability heartbeat.

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

#include "DeviceIdentity.h"
#include "DisplayWidth.h"
#include "MdnsDiscovery.h"
#include "MqttHelpers.h"
#include "NvsSettingsStore.h"
#include "Settings.h"
#include "Tasks.h"
#include "WebEndpoints.h"
#include "WebLog.h"
#include "WifiService.h"

// v1 derives its chip id from ESP.getChipId() = last 3 octets of the MAC.
// The ESP32 core has no getChipId(); take the same last-3-octets slice of
// the efuse base MAC so a device keeps one identity across the port.
// TODO(#58): lock byte-order parity against a v1 device before the
// identity/EEPROM migration lands.
static uint32_t chipIdFromEfuseMac() {
  const uint64_t mac = ESP.getEfuseMac();  // base MAC, byte 0 = first octet
  return (uint32_t)((mac >> 24) & 0xFFFFFF);
}

static const char* MDNS_NAME_PREFIX = "split-flap";

static NvsSettingsStore settingsStore;
static MasterSettings settings;
static String deviceName;
static AsyncWebServer webServer(80);

void setup() {
  Serial.begin(115200);
  delay(2000);  // native USB-CDC needs a moment before the first prints land
  webLogInit();  // before the first SerialPrint*, or those lines never
                 // reach GET /log

  settingsStore.begin();
  settings = loadSettings(settingsStore);
  deviceName = resolveDeviceName(true, settings.deviceName, MDNS_NAME_PREFIX,
                                 chipIdFromEfuseMac());

  Serial.println();
  Serial.println(F("split-flap v2 master — Phase 1 (#58)"));
  Serial.printf("chip: %s rev %d, %d cores @ %d MHz\n", ESP.getChipModel(),
                ESP.getChipRevision(), ESP.getChipCores(), ESP.getCpuFreqMHz());
  Serial.printf("flash: %u KB, free heap: %u KB\n",
                ESP.getFlashChipSize() / 1024, ESP.getFreeHeap() / 1024);
  // First-boot verification that the N16R8 flags match the silicon: an
  // N16R8 must report ~8 MB here; 0 means the qio_opi/PSRAM flags are wrong
  // for whatever module is actually fitted.
  Serial.printf("psram: %u KB (%u KB free)\n", ESP.getPsramSize() / 1024,
                ESP.getFreePsram() / 1024);
  Serial.printf("identity: %s\n", deviceName.c_str());
  Serial.printf("settings: align=%s speed=%d mode=%s tz=%s\n",
                settings.alignment.c_str(), settings.flapSpeed,
                settings.deviceMode.c_str(), settings.timezonePosix.c_str());
  Serial.printf("mqtt: %s\n",
                settings.mqttHost.length()
                    ? (settings.mqttHost + ":" + settings.mqttPort).c_str()
                    : "(disabled)");

  // Routes registered now; server.begin() happens from WifiService once a
  // netif exists (STA join or portal AP) — LWIP isn't up before that.
  webEndpointsInit(webServer, settings, settingsStore, deviceName);
  Serial.println(F("web endpoints registered (server starts with the netif)"));

  // Wiring only — the radio comes up on netTask's first wifiServiceTick(),
  // keeping every WiFi call on core 0 (#188).
  wifiServiceInit(webServer, settings, settingsStore, deviceName);
  Serial.printf("wifi: %s\n", settings.wifiSsid.length()
                                  ? ("join \"" + settings.wifiSsid + "\"").c_str()
                                  : "unprovisioned -> setup portal");

  // After webEndpointsInit/wifiServiceInit: netTask ticks both and needs
  // their mutexes to exist before its first pass.
  tasksInit(settings, settingsStore);
  Serial.println(F("task skeleton up: display+clock on core 1, net+mqtt on core 0"));
}

void loop() {
  // All real work lives in the domain tasks (Tasks.cpp); loopTask just
  // reports. The delay yields core 1 to displayTask between beats.
  tasksHeartbeatReport();
  delay(5000);
}
