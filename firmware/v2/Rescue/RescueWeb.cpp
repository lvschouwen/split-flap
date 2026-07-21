#include "RescueWeb.h"

#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <atomic>

#include "BuildVersion.h"
#include "RescueAssets.h"
#include "RescueCors.h"  // CSRF origin gate on mutating POSTs (#349)
#include "RescueOta.h"
#include "RescueSlotRecord.h"
#include "RescueSlots.h"
#include "WebBodyLimitGuard.h"  // pre-auth body-size guard (#347)

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
// #347: which request actually began an Update, so onRequest never reports a
// flash (and reboots) for a POST that carried no multipart file part.
// #349/#359: doubles as the single-owner session lock — a second concurrent
// upload is 409'd instead of stealing/aborting a live recovery flash, and
// every chunk is gated on ownership so an orphaned request can't interleave
// bytes into the live Update session.
static AsyncWebServerRequest* masterOtaOwnerRequest = nullptr;

// CSRF gate (#349), copy of Master's webUploadCsrfRejected shape: multipart
// POSTs are CORS-safelisted, so without this a visited web page could
// blind-flash firmware or force a reboot while rescue is STA-joined.
static bool rescueUploadCsrfRejected(AsyncWebServerRequest* request) {
  bool hasOrigin = request->hasHeader("Origin");
  return rescueCsrfRejectPost(true, hasOrigin,
                              hasOrigin ? request->header("Origin") : String());
}

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

// #200 slot ranking, pinned once in rescueWebInit: Master's NVS confirm
// records, each accepted only when its sha256 still matches the slot's
// actual image (a reflash behind the record's back demotes that slot to the
// stamp fallback). Slot contents can't change under rescue without a reboot
// — its own upload path reboots — so once is enough.
static SlotRecord slotRec[2];
static bool slotRecMatches[2] = {false, false};

static void pinSlotRecord(int idx, const SlotProbe& p, const String& raw) {
  slotRec[idx] = parseSlotRecord(raw.c_str());
  // The sha check reads the slot's actual image length, not the whole 4 MB
  // partition (IDF verifies the appended digest over image_len); the ms
  // print is the bench check that setup() stays fast.
  uint32_t shaStart = millis();
  uint8_t sha[32];
  slotRecMatches[idx] = p.valid && slotRec[idx].ok &&
                        esp_partition_get_sha256(p.part, sha) == ESP_OK &&
                        slotRecordShaMatches(slotRec[idx], sha);
  if (slotRec[idx].ok) {
    Serial.printf("rescue: app%d confirm record seq %lu rev %s — %s (sha %lu ms)\n",
                  idx, (unsigned long)slotRec[idx].seq, slotRec[idx].rev,
                  slotRecMatches[idx] ? "matches image" : "STALE, ignored",
                  (unsigned long)(millis() - shaStart));
  }
}

static ExitSlotCandidate exitCandidate(const SlotProbe& p, int idx) {
  ExitSlotCandidate c;
  c.valid = p.valid;
  c.stamp = p.stamp;
  c.confirmed = p.valid && slotRecMatches[idx];
  c.seq = slotRec[idx].seq;
  return c;
}

// slotIdx: OTA slot index for the #200 record fields, -1 for factory.
static void appendSlotJson(String& out, const SlotProbe& p, const char* label,
                           bool running, int slotIdx) {
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
    // The descriptor version/built are frozen at framework-assembly time
    // (#200) — the confirm record's rev is the trustworthy per-build label.
    if (slotIdx >= 0 && slotRecMatches[slotIdx]) {
      out += ",\"rev\":\"";
      out += slotRec[slotIdx].rev;  // parse enforces JSON-safe charset
      out += "\",\"seq\":";
      out += String((unsigned long)slotRec[slotIdx].seq);
    }
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
  int exitSlot = pickExitSlot(exitCandidate(app0, 0), exitCandidate(app1, 1));

  String out;
  out.reserve(512);
  out += "{\"name\":\"";
  out += deviceName;  // validated identity ([a-z0-9-]) — JSON-safe as-is
  out += "\",\"rescue\":\"";
  out += GIT_REV;
  out += "\",\"slots\":[";
  appendSlotJson(out, app0, "app0", running == app0.part, 0);
  out += ',';
  appendSlotJson(out, app1, "app1", running == app1.part, 1);
  out += ',';
  appendSlotJson(out, factory, "factory", running == factory.part, -1);
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

void rescueWebInit(AsyncWebServer& server, const String& effectiveDeviceName,
                   const String& slotRec0, const String& slotRec1) {
  deviceName = effectiveDeviceName;
  pinSlotRecord(0, probeSlot(ESP_PARTITION_SUBTYPE_APP_OTA_0), slotRec0);
  pinSlotRecord(1, probeSlot(ESP_PARTITION_SUBTYPE_APP_OTA_1), slotRec1);

  // Pre-auth body-size guard (#347) — before any route so it wins the
  // first-match-wins scan for an oversized body.
  attachBodyLimitGuard(server);

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
        // Overlap-rejected upload (#349/#359), or a bodyless POST racing a
        // live session: answer 409 without touching the owner's shared state
        // or Update session.
        if (request->_tempObject != nullptr ||
            (masterOtaOwnerRequest != nullptr &&
             masterOtaOwnerRequest != request)) {
          request->send(409, "text/plain",
                        F("Another rescue flash is already in progress — "
                          "retry when it finishes"));
          return;
        }
        // #347: did onUpload begin an Update for THIS request? A POST with no
        // multipart file part never enters onUpload, so Update.isFinished()
        // would report success on a never-begun Update and reboot the device.
        bool uploadRan = (masterOtaOwnerRequest == request);
        if (uploadRan) masterOtaOwnerRequest = nullptr;
        // Consume the rejection verdict: left set, a later POST whose
        // upload callback never runs would echo this stale status instead
        // of its own "no firmware in request" 400.
        int rejStatus = otaRejectionStatus;
        String rejReason = otaRejectionReason;
        otaRejectionStatus = 0;
        otaRejectionReason = "";
        if (rejStatus != 0) {
          request->send(rejStatus, "text/plain", rejReason);
          return;
        }
        if (Update.hasError()) {
          request->send(500, "text/plain", String("Rescue flash failed: ") +
                                               Update.errorString());
          return;
        }
        if (!uploadRan) {
          request->send(400, "text/plain",
                        F("No firmware in request (a multipart file part is "
                          "required)"));
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
          // Concurrent-upload guard (#349/#359): a live session owns the
          // Update singleton and the shared rejection state — mark this
          // request rejected (per-request _tempObject; malloc pairs with the
          // free in the request destructor) and leave both alone. Stealing
          // the session here (the old abort+begin) DoS'd a legitimate
          // recovery flash and interleaved the two requests' chunks.
          if (masterOtaOwnerRequest != nullptr &&
              masterOtaOwnerRequest != request) {
            request->_tempObject = malloc(1);
            return;
          }
          otaRejectionStatus = 0;
          otaRejectionReason = "";

          // CSRF gate (#349), INLINE before Update.begin: the body-parsing
          // upload callback is the first code to run for this route, so a
          // forged cross-site POST would otherwise flash + arm its image
          // before any post-body check.
          if (rescueUploadCsrfRejected(request)) {
            otaRejectionStatus = 403;
            otaRejectionReason = "Cross-origin flash refused (CSRF guard)";
            return;
          }

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
          // #347: a real flash began here; #349: session is live from here.
          // onDisconnect is the backstop for a client that dies mid-upload:
          // free the slot; the stale Update session is aborted by the next
          // upload's begin path above.
          masterOtaOwnerRequest = request;
          request->onDisconnect([request]() {
            if (masterOtaOwnerRequest == request) masterOtaOwnerRequest = nullptr;
          });
        }

        // Not (or no longer) the live owner (#359): covers overlap-rejected
        // requests whose client keeps streaming — without this gate their
        // leftover chunks would interleave into the live owner's session.
        if (masterOtaOwnerRequest != request) return;
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

  // Accidental-entry escape: re-arm the most recently confirmed valid OTA
  // slot (#200 records; stamp fallback) and reboot — no flash write beyond
  // otadata. Also marks the slot ESP_OTA_IMG_NEW (rollback config), so the
  // exited-to image re-proves itself against the health gate; it was
  // booting fine before, so that's a free self-test.
  server.on("/rescue/exit", HTTP_POST, [](AsyncWebServerRequest* request) {
    // CSRF gate (#349): a forged cross-site POST must not reboot the device
    // out of a recovery session.
    if (rescueUploadCsrfRejected(request)) {
      request->send(403, "text/plain",
                    F("Cross-origin request refused (CSRF guard)"));
      return;
    }
    SlotProbe app0 = probeSlot(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    SlotProbe app1 = probeSlot(ESP_PARTITION_SUBTYPE_APP_OTA_1);
    int slot = pickExitSlot(exitCandidate(app0, 0), exitCandidate(app1, 1));
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
