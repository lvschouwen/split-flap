#include "RescueWeb.h"

#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <atomic>

#include "BuildVersion.h"
#include "RescueAssets.h"
#include "RescueOta.h"
#include "RescueSlots.h"

static String deviceName;
static bool serverStarted = false;
static String captiveRedirectUrl;

// Reboot staging: async_tcp handlers (core 0) set it, loop()'s
// rescueWebTick() executes it. Atomics, timestamp written before the flag —
// the tick can never pair a raised flag with a stale timestamp, which would
// shrink the 750 ms flush-grace (same rule as Master) to zero.
static std::atomic<bool> rebootPending{false};
static std::atomic<uint32_t> rebootRequestedAtMs{0};
static const uint32_t REBOOT_GRACE_MS = 750;

// Upload gating state, same shape as Master's /firmware/master handler.
static int otaRejectionStatus = 0;
static String otaRejectionReason;

static void stageReboot() {
  rebootRequestedAtMs.store(millis());
  rebootPending.store(true);
}

// esp_app_desc strings are compiler-controlled but a hand-flashed image
// could hold anything: keep JSON output printable-ASCII, drop quote/backslash.
static String jsonSanitize(const char* raw, size_t maxLen) {
  String out;
  for (size_t i = 0; i < maxLen && raw[i] != '\0'; i++) {
    char c = raw[i];
    if (c >= 0x20 && c <= 0x7E && c != '"' && c != '\\') out += c;
  }
  return out;
}

struct SlotProbe {
  const esp_partition_t* part = nullptr;
  bool valid = false;
  esp_app_desc_t desc = {};
  uint64_t stamp = 0;
};

static SlotProbe probeSlot(esp_partition_subtype_t subtype) {
  SlotProbe p;
  p.part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, nullptr);
  if (p.part != nullptr &&
      esp_ota_get_partition_description(p.part, &p.desc) == ESP_OK) {
    p.valid = true;
    p.stamp = parseAppBuildStamp(p.desc.date, p.desc.time);
  }
  return p;
}

static void appendSlotJson(String& out, const SlotProbe& p, const char* label,
                           bool running) {
  out += "{\"label\":\"";
  out += label;
  out += "\",\"valid\":";
  out += p.valid ? "true" : "false";
  out += ",\"running\":";
  out += running ? "true" : "false";
  if (p.valid) {
    out += ",\"version\":\"";
    out += jsonSanitize(p.desc.version, sizeof(p.desc.version));
    out += "\",\"built\":\"";
    out += jsonSanitize(p.desc.date, sizeof(p.desc.date));
    out += ' ';
    out += jsonSanitize(p.desc.time, sizeof(p.desc.time));
    out += '"';
  }
  out += '}';
}

// Slot inventory + exit target for the page. Flash reads from the async
// task — quick bounded reads, same accepted exception class as the upload
// path's flash writes (there is no other task to stage through here).
static String statusJson() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  SlotProbe app0 = probeSlot(ESP_PARTITION_SUBTYPE_APP_OTA_0);
  SlotProbe app1 = probeSlot(ESP_PARTITION_SUBTYPE_APP_OTA_1);
  SlotProbe factory = probeSlot(ESP_PARTITION_SUBTYPE_APP_FACTORY);
  int exitSlot =
      pickExitSlot(app0.valid, app0.stamp, app1.valid, app1.stamp);

  String out;
  out.reserve(512);
  out += "{\"name\":\"";
  out += deviceName;  // validated identity ([a-z0-9-]) — JSON-safe as-is
  out += "\",\"rescue\":\"";
  out += GIT_REV;
  out += "\",\"slots\":[";
  appendSlotJson(out, app0, "app0", running == app0.part);
  out += ',';
  appendSlotJson(out, app1, "app1", running == app1.part);
  out += ',';
  appendSlotJson(out, factory, "factory", running == factory.part);
  out += "],\"exit\":";
  if (exitSlot == 0) {
    out += "\"app0\"";
  } else if (exitSlot == 1) {
    out += "\"app1\"";
  } else {
    out += "null";
  }
  out += '}';
  return out;
}

static void serveGzipAsset(AsyncWebServerRequest* request,
                           const char* contentType, const uint8_t* asset,
                           size_t assetLen) {
  AsyncWebServerResponse* resp =
      request->beginResponse(200, contentType, asset, assetLen);
  resp->addHeader("Content-Encoding", "gzip");
  resp->addHeader("Cache-Control", "no-cache");
  request->send(resp);
}

void rescueWebInit(AsyncWebServer& server, const String& effectiveDeviceName) {
  deviceName = effectiveDeviceName;

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    serveGzipAsset(request, "text/html", RESCUE_HTML_GZ, RESCUE_HTML_GZ_LEN);
  });
  server.on("/md5.js", HTTP_GET, [](AsyncWebServerRequest* request) {
    serveGzipAsset(request, "application/javascript", MD5_JS_GZ, MD5_JS_GZ_LEN);
  });

  server.on("/rescue/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", statusJson());
  });

  // Same wire contract as normal firmware (multipart field "firmware" +
  // mandatory ?md5=) so flashing/ota-master.sh and the page JS work against
  // either image. Update targets the next update partition — running from
  // factory that is app0 — and Update.end() arms it via
  // esp_ota_set_boot_partition, which marks it ESP_OTA_IMG_NEW: the image
  // boots PENDING_VERIFY and rides Master's #190 health gate. A bad image
  // is rolled back by the bootloader; worst case is rescue again.
  server.on(
      "/firmware/master", HTTP_POST,
      [](AsyncWebServerRequest* request) {
        if (otaRejectionStatus != 0) {
          request->send(otaRejectionStatus, "text/plain", otaRejectionReason);
          return;
        }
        if (Update.hasError()) {
          request->send(500, "text/plain", String("Rescue flash failed: ") +
                                               Update.errorString());
          return;
        }
        if (!Update.isFinished()) {
          request->send(500, "text/plain",
                        F("Rescue flash incomplete: upload ended before the "
                          "image was complete."));
          return;
        }
        request->send(200, "text/plain",
                      F("Master firmware flashed; rebooting into it…"));
        stageReboot();
      },
      [](AsyncWebServerRequest* request, String filename, size_t index,
         uint8_t* data, size_t len, bool final) {
        if (index == 0) {
          otaRejectionStatus = 0;
          otaRejectionReason = "";

          String md5 = request->hasParam("md5")
                           ? request->getParam("md5")->value()
                           : String();
          if (md5.length() == 0) {
            otaRejectionStatus = 400;
            otaRejectionReason =
                "md5 query parameter is required (compute it over the .bin "
                "and pass ?md5=...)";
            return;
          }
          if (!normalizeOtaMd5(md5)) {
            otaRejectionStatus = 400;
            otaRejectionReason = "md5 must be exactly 32 hex characters";
            return;
          }

          Serial.println("Rescue flash started (md5 " + md5 + ")");
          if (Update.isRunning()) {
            Update.abort();  // stale aborted upload must not wedge this one
          }
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            otaRejectionStatus = 500;
            otaRejectionReason =
                String("Rescue flash could not start: ") + Update.errorString();
            return;
          }
          Update.setMD5(md5.c_str());
        }

        if (otaRejectionStatus != 0) return;

        if (len > 0 && Update.write(data, len) != len) {
          return;  // error latched inside Update; completion callback reports
        }
        if (final) {
          if (Update.end(true)) {
            Serial.println(F("Rescue flash verified and armed — reboot boots "
                             "it PENDING_VERIFY"));
          } else {
            Serial.println(String("Rescue flash failed at end: ") +
                           Update.errorString());
          }
        }
      });

  // Accidental-entry escape: re-arm the newest valid OTA slot and reboot —
  // no flash write beyond otadata. Also marks the slot ESP_OTA_IMG_NEW
  // (rollback config), so the exited-to image re-proves itself against the
  // health gate; it was booting fine before, so that's a free self-test.
  server.on("/rescue/exit", HTTP_POST, [](AsyncWebServerRequest* request) {
    SlotProbe app0 = probeSlot(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    SlotProbe app1 = probeSlot(ESP_PARTITION_SUBTYPE_APP_OTA_1);
    int slot = pickExitSlot(app0.valid, app0.stamp, app1.valid, app1.stamp);
    if (slot < 0) {
      request->send(409, "text/plain",
                    F("No valid firmware in either OTA slot — upload an "
                      "image instead."));
      return;
    }
    const esp_partition_t* target = (slot == 0) ? app0.part : app1.part;
    esp_err_t err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
      request->send(500, "text/plain",
                    String("Could not arm ") + target->label + ": " +
                        esp_err_to_name(err));
      return;
    }
    Serial.printf("rescue exit: booting %s\n", target->label);
    request->send(200, "text/plain",
                  String("Rebooting into ") + target->label + "…");
    stageReboot();
  });

  server.onNotFound([](AsyncWebServerRequest* request) {
    if (captiveRedirectUrl.length() > 0) {
      request->redirect(captiveRedirectUrl);
    } else {
      request->send(404, "text/plain", F("Not found"));
    }
  });
}

void rescueWebStart(AsyncWebServer& server) {
  if (serverStarted) return;
  serverStarted = true;
  server.begin();
  Serial.println(F("rescue web server started"));
}

void rescueWebSetCaptiveRedirect(const String& url) {
  captiveRedirectUrl = url;
}

void rescueWebTick() {
  if (rebootPending.load() &&
      millis() - rebootRequestedAtMs.load() > REBOOT_GRACE_MS) {
    Serial.flush();
    ESP.restart();
  }
}
