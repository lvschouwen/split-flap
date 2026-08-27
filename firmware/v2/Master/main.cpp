// v2 master — Phase 1 (#58, epic #183).
//
// Boots on an ESP32-S3 devkit, loads settings from NVS, prints an identity
// banner, registers the full v1 web endpoint surface (#186), and starts the
// dual-core task skeleton (#187): display domain on core 1, network domain
// on core 0, queues and snapshot copies in between. setup() is the
// composition root; loop() survives only as the observability heartbeat.

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "BuildVersion.h"  // GIT_REV — boot banner
#include "ClockService.h"
#include "ClusterFollower.h"
#include "ClusterLeader.h"
#include "DeviceIdentity.h"
#include "FactorySlot.h"
#include "FlashLog.h"
#include "FollowerImageStore.h"
#include "HelpersSerialHandling.h"
#include "MqttService.h"
#include "NvsSettingsStore.h"
#include "OtaService.h"
#include "RebootCause.h"  // #432
#include "Settings.h"
#include "StatusLed.h"
#include "SystemStats.h"
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

static const char* otaStateName(esp_ota_img_states_t state) {
  switch (state) {
    case ESP_OTA_IMG_NEW:            return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:          return "VALID";
    case ESP_OTA_IMG_INVALID:        return "INVALID";
    case ESP_OTA_IMG_ABORTED:        return "ABORTED";
    default:                         return "UNDEFINED";
  }
}

// Boot-time partition diagnostics (#198), tee'd to the web/flash log like
// the rest of the banner (#212): which A/B slot is running and in what
// esp_ota state, what otadata will boot next, whether the factory rescue
// slot holds an image, and the live partition table (must match
// partitions_splitflap_16MB.csv).
static void printBootDiagnostics() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (running) esp_ota_get_state_partition(running, &state);
  SerialPrintf("boot: running %s @ 0x%06x state=%s, otadata -> %s\n",
               running ? running->label : "?",
               running ? (unsigned)running->address : 0, otaStateName(state),
               boot ? boot->label : "?");

  // #391: warm the rescue-slot cache before anything can read it. Runs on
  // loopTask in setup(), strictly before the web server and the other tasks
  // exist, so the async readers below can never race the first computation
  // or pay its ~60 ms. Unconditional: with no factory partition it caches
  // the honest "absent" verdict.
  rescueSlotRefresh();

  const esp_partition_t* factory = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
  if (factory) {
    // Erased flash reads 0xFF; any flashed image starts with magic 0xE9.
    uint8_t magic = 0xFF;
    esp_partition_read(factory, 0, &magic, 1);
    SerialPrintf("rescue: %s @ 0x%06x (%u KB) — %s\n", factory->label,
                 (unsigned)factory->address, factory->size / 1024,
                 magic == 0xE9 ? "image present" : "empty");
    // #391: "image present" alone hid a months-old rescue build. Say what it
    // is. The cache is warmed just below, unconditionally.
    RescueSlotFacts rescue = rescueSlotCurrent();
    if (rescue.identified) {
      SerialPrintf("rescue: image rev %s%s\n", rescue.rev,
                   rescue.stale ? " — OLDER than the running app, reinstall "
                                  "via POST /firmware/rescue"
                                : "");
    } else if (rescue.valid) {
      SerialPrintln(F("rescue: image rev unknown (installed out of band) — "
                      "reinstall via POST /firmware/rescue to record it"));
    }
  } else {
    SerialPrintln(F("rescue: no factory partition (!)"));
  }

  SerialPrintln(F("partition table:"));
  esp_partition_iterator_t it = esp_partition_find(
      ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
  for (; it != NULL; it = esp_partition_next(it)) {
    const esp_partition_t* p = esp_partition_get(it);
    SerialPrintf("  %-9s %-4s @ 0x%06x %5u KB\n", p->label,
                 p->type == ESP_PARTITION_TYPE_APP ? "app" : "data",
                 (unsigned)p->address, p->size / 1024);
  }
  esp_partition_iterator_release(it);
}

static NvsSettingsStore settingsStore;
static MasterSettings settings;
static String deviceName;
static AsyncWebServer webServer(80);

void setup() {
  Serial.begin(115200);
  delay(2000);  // native USB-CDC needs a moment before the first prints land
  // #432: consume the deliberate-reboot breadcrumb before ANYTHING that can
  // panic — a stamp surviving a crashed init would be blamed on the wrong
  // boot. Caches; webEndpointsInit reads the cached copy.
  rebootCauseConsume();
  webLogInit();  // before the first SerialPrint*, or those lines never
                 // reach GET /log
  flashLogInit();  // #206: mounts `storage`, writes the boot marker; from
                   // here every SerialPrint* also lands in /log.txt
  followerImageStoreInit();  // #304: read the stored ESP-01 image rev/presence

  settingsStore.begin();
  settings = loadSettings(settingsStore);
  // Lifetime boot counter (#224): v2's stand-in for v1's RTC counter — it
  // only feeds the HA diag/boots sensor, so NVS wear-leveled u32 is plenty.
  uint32_t bootCount = (uint32_t)settingsStore.getInt("bootCount", 0) + 1;
  settingsStore.putInt("bootCount", (int)bootCount);
  deviceName = resolveDeviceName(true, settings.deviceName, MDNS_NAME_PREFIX,
                                 chipIdFromEfuseMac());
  statusLedInit(settings);  // boot white from here on (#199)
  systemStatsInit();        // #245: before tasksInit() starts netTask
  // #272: loads the persisted cluster membership — a clustered follower
  // boots gated in Grace. Before tasksInit(): netTask ticks the service
  // and clockTask reads its view.
  clusterFollowerInit(settingsStore);
  // #273: loads the member table if this master leads a cluster wall, and
  // mints the boot epoch. Before tasksInit() — clusterTask ticks it.
  clusterLeaderInit(settingsStore, deviceName);

  SerialPrintln(F(""));
  SerialPrintln(F("split-flap v2 master — " GIT_REV));
  SerialPrintf("chip: %s rev %d, %d cores @ %d MHz\n", ESP.getChipModel(),
               ESP.getChipRevision(), ESP.getChipCores(), ESP.getCpuFreqMHz());
  SerialPrintf("flash: %u KB, free heap: %u KB\n",
               ESP.getFlashChipSize() / 1024, ESP.getFreeHeap() / 1024);
  // First-boot verification that the N16R8 flags match the silicon: an
  // N16R8 must report ~8 MB here; 0 means the qio_opi/PSRAM flags are wrong
  // for whatever module is actually fitted.
  SerialPrintf("psram: %u KB (%u KB free)\n", ESP.getPsramSize() / 1024,
               ESP.getFreePsram() / 1024);
  SerialPrintf("identity: %s\n", deviceName.c_str());
  SerialPrintf("settings: align=%s speed=%d mode=%s tz=%s\n",
               settings.alignment.c_str(), settings.flapSpeed,
               settings.deviceMode.c_str(), settings.timezonePosix.c_str());
  SerialPrintf("mqtt: %s\n",
               settings.mqttHost.length()
                   ? (settings.mqttHost + ":" + settings.mqttPort).c_str()
                   : "(disabled)");
  printBootDiagnostics();

  // Snapshot this boot's A/B partition state; a PENDING_VERIFY image is
  // confirmed pre-inrush at the end of setup() (#305, below), with the
  // netif-up call in WifiService as a fallback.
  otaServiceInit();

  // #390: after an out-of-band recovery the stored ?v= diagnostic names an
  // image this boot is not running — blank it (one NVS write, only on the
  // boot after such a recovery). A genuine revert keeps the mismatch.
  if (otaShouldBlankIntendedVersion(settings.intendedVersion, GIT_REV,
                                    otaVerdictSnapshot())) {
    SerialPrintln("intendedVersion \"" + settings.intendedVersion +
                  "\" != running " GIT_REV " without a revert — blanking "
                  "(out-of-band recovery, #390)");
    settings.intendedVersion = "";
    saveIntendedVersion(settingsStore, "");
  }

  // TZ correct from the first log line; SNTP starts now and syncs once a
  // netif exists (startOnline re-applies for the immediate kick, #192).
  clockServiceApplyTz(settings);

  // Routes registered now; server.begin() happens from WifiService once a
  // netif exists (STA join or portal AP) — LWIP isn't up before that.
  webEndpointsInit(webServer, settings, settingsStore, deviceName);
  SerialPrintln(F("web endpoints registered (server starts with the netif)"));

  // Stable broker-identity copies + client config (#224); the connection
  // itself is mqttTask's business once tasksInit() starts it. Must run
  // after webEndpointsInit (the service reads web-domain snapshots).
  mqttServiceInit(settings, settingsStore, deviceName, bootCount);

  // Wiring only — the radio comes up on netTask's first wifiServiceTick(),
  // keeping every WiFi call on core 0 (#188).
  wifiServiceInit(webServer, settings, settingsStore, deviceName);
  SerialPrintf("wifi: %s\n", settings.wifiSsid.length()
                                 ? ("join \"" + settings.wifiSsid + "\"").c_str()
                                 : "unprovisioned -> setup portal");

  // #305: confirm a PENDING_VERIFY image HERE — after the whole
  // single-threaded init sequence proved it doesn't hard-crash, but BEFORE
  // tasksInit() starts the display unit-move + WiFi-PA inrush. That inrush
  // can dip the rail past the S3 brownout detector on a post-OTA verify-boot;
  // confirming at the old netif-up bar (which is on the far side of the
  // inrush) let the bootloader roll back a perfectly good image over a
  // millisecond sag it survives at steady state. A hard crash in any init
  // above still leaves the image unconfirmed and rolls back; the netif-up
  // call is now a no-op fallback.
  otaHealthConfirm();

  // After webEndpointsInit/wifiServiceInit: netTask ticks both and needs
  // their mutexes to exist before its first pass.
  tasksInit(settings, settingsStore);
  SerialPrintln(F("task skeleton up: display+clock on core 1, net+mqtt on core 0"));
}

void loop() {
  // All real work lives in the domain tasks (Tasks.cpp); loopTask just
  // reports. The delay yields core 1 to displayTask between beats.
  tasksHeartbeatReport();
  delay(5000);
}
