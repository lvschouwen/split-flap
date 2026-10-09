// FollowerWeb.cpp — the routes this board keeps without a master. Contract in
// FollowerWeb.h. /firmware/master is v1's ESP8266 Update flow trimmed (no RTC
// verdict cookie — ota-flash.sh's version comparison is the revert detector
// on this board).

#include "FollowerWeb.h"

#include <Updater.h>
#include <flash_hal.h>  // FS_start: the end of the app area

#include "BuildVersion.h"
#include "FollowerBus.h"       // displayWidth
#include "FollowerCluster.h"
#include "FollowerConfig.h"
#include "FollowerJson.h"
#include "FollowerLink.h"
#include "FollowerOtaImage.h"  // #540: gzip upload checks
#include "FollowerPairPolicy.h"
#include "FollowerRescue.h"
#include "FollowerUnitJobs.h"  // an upload waits for a unit update
#include "FollowerUpdate.h"    // the wall link's download owns the updater meanwhile
#include "FollowerWifi.h"
#include "LanOrigin.h"
#include "OtaUploadGate.h"     // shared gate / completion / stall rules
#include "WebBodyLimitGuard.h" // pre-auth body-size guard (#347)

volatile bool isPendingReboot = false;
static volatile bool masterOtaUploadActive = false;
static volatile unsigned long masterOtaLastChunkMs = 0;

// --- OTA session state (v1 #191 conventions) ----------------------------------------

static AsyncWebServerRequest* volatile masterOtaOwnerRequest = nullptr;

static OtaRejection otaRejection;
static bool otaTxPowerReduced = false;
// #540: the upload in progress is gzip-packed; its tail carries the length
// eboot will unpack.
static bool otaUploadIsGzip = false;
static OtaGzipTail otaGzipTail;
static uint32_t otaReservedBytes = 0;  // what Update.begin was given

// Running image + stored upload share [0, this); the EEPROM sector follows.
uint32_t appAreaBytes() { return FS_start - 0x40200000; }

// --- helpers ------------------------------------------------------------------------

// A website must not be able to drive this board through the owner's browser:
// this board serves no page, so a POST with an Origin is refused (403,
// answered here). The master and ota-flash.sh send no Origin and pass.
static bool followerRejectCsrf(AsyncWebServerRequest* request) {
  bool hasOrigin = request->hasHeader("Origin");
  String origin = hasOrigin ? request->header("Origin") : String();
  if (lanCsrfReject(request->method() == HTTP_POST, hasOrigin, origin,
                    request->host())) {
    request->send(403, "text/plain",
                  F("Cross-origin POST refused (CSRF guard)"));
    return true;
  }
  return false;
}

static String identityJsonNow() {
  const FollowerClusterView cv = clusterViewGet();
  FollowerIdentity id;
  id.name = effectiveDeviceName.c_str();
  id.rev = GIT_REV;
  id.width = displayWidth;
  id.rescue = rescueActive();
  id.masterId = cv.leaderName.c_str();
  id.masterHost = cv.leaderHost.c_str();
  id.linked = linkViewGet().connected;
  id.upSeconds = millis() / 1000;
  id.heapBytes = ESP.getFreeHeap();
  id.sketchBytes = ESP.getSketchSize();
  id.sketchFreeBytes = ESP.getFreeSketchSpace();
  id.flashMode = (int)ESP.getFlashChipMode();
  id.flashId = ESP.getFlashChipId();
  return followerIdentityJson(id);
}

// --- OTA (v1 registerMasterFirmwareEndpoint, trimmed) --------------------------------

static void registerMasterFirmwareEndpoint(AsyncWebServer& server) {
  server.on("/firmware/master", HTTP_POST,
    [](AsyncWebServerRequest* request) {
      if (request->_tempObject != nullptr ||
          (masterOtaOwnerRequest != nullptr &&
           masterOtaOwnerRequest != request)) {
        request->send(409, "text/plain",
                      "Another master OTA upload is already in progress — "
                      "retry when it finishes");
        return;
      }
      // #347: did onUpload establish a session for THIS request? A POST with
      // no multipart file part never enters onUpload, so Update.begin() and
      // the in-onUpload CSRF gate never run — Update.isFinished() would then
      // report success on a never-begun Update and trigger a spurious,
      // unauthenticated reboot.
      bool uploadRan = (masterOtaOwnerRequest == request);
      masterOtaOwnerRequest = nullptr;
      if (otaTxPowerReduced) {
        followerTxOtaCap(false);
        otaTxPowerReduced = false;
      }
      int rejStatus = 0;
      String rejReason;
      bool rejected = otaRejection.take(rejStatus, rejReason);
      OtaCompletion verdict = otaUploadCompletion(
          rejected, Update.hasError(), uploadRan, Update.isFinished());
      // Anything but a flashed image means no reboot follows: thaw the row.
      if (verdict != OtaCompletion::Flashed) masterOtaUploadActive = false;
      switch (verdict) {
        case OtaCompletion::Rejected:
          request->send(rejStatus, "text/plain", rejReason);
          break;
        case OtaCompletion::FlashError:
          request->send(500, "text/plain",
                        String(F("Master OTA failed: ")) +
                            Update.getErrorString());
          break;
        case OtaCompletion::NoFile:
          request->send(400, "text/plain",
                        F("No firmware in request (a multipart file part is "
                          "required)"));
          break;
        case OtaCompletion::Incomplete:
          request->send(500, "text/plain",
                        F("Master OTA incomplete: final chunk missing"));
          break;
        case OtaCompletion::Flashed:
          request->send(200, "text/plain",
                        F("Master firmware flashed; rebooting…"));
          isPendingReboot = true;
          break;
      }
    },
    [](AsyncWebServerRequest* request, String filename, size_t index,
       uint8_t* data, size_t len, bool final) {
      // Concurrent-upload guard (v1 #191): one live session owns the
      // Update singleton; overlaps are marked rejected per-request.
      if (index == 0 && masterOtaOwnerRequest != nullptr &&
          masterOtaOwnerRequest != request) {
        request->_tempObject = malloc(1);
        return;
      }
      if (request->_tempObject != nullptr) return;
      if (index == 0 || masterOtaOwnerRequest == request) {
        masterOtaLastChunkMs = millis();
      }
      if (index == 0) {
        // Before any flash write or freeze: no owner is taken on a refusal.
        // The unit-reflash gate: this upload ends in a reboot, which would
        // strand the unit being flashed in twiboot (the leader's rollout
        // reads the 409 as "busy, retry").
        // MD5 is MANDATORY (v1 #144: eboot's checksum does not catch a
        // truncated upload) and validated before Update.begin (#354), which
        // erases the flash region.
        bool hasOrigin = request->hasHeader("Origin");
        String md5 = request->hasParam("md5")
                         ? request->getParam("md5")->value()
                         : String();
        OtaGate gate = otaUploadGate(
            lanCsrfReject(true, hasOrigin,
                          hasOrigin ? request->header("Origin") : String(),
                          request->host()),
            unitUpdateQueuedOrRunning(), md5);
        otaRejection.clear();
        if (gate != OtaGate::Pass) {
          otaRejection.set(gate);
          return;
        }
        // The wall link is fetching an image, or has stored one and is about
        // to restart into it: one updater, one writer.
        if (updateDownloadActive()) {
          otaRejection.set(409, F("The row is installing an image from its "
                                  "master — retry when it has restarted"));
          return;
        }
        // #540: a gzip image is refused here unless it shows the flash
        // config this board runs (FollowerOtaImage.h) — before the freeze
        // and before Update.begin, so a refusal costs nothing.
        otaUploadIsGzip = otaIsGzip(data, len);
        otaGzipTail = OtaGzipTail();
        if (otaUploadIsGzip) {
          uint32_t running = 0;
          OtaImageCheck check =
              ESP.flashRead(0, &running, sizeof(running))
                  ? otaGzipCheck(data, len, (const uint8_t*)&running)
                  : OtaImageCheck::FlashMode;
          if (check != OtaImageCheck::Ok) {
            otaRejection.set(400, String(otaImageCheckReason(check)));
            return;
          }
        }
        // Freeze all display/unit work for the upload (v1 #116): WiFi RX +
        // flash writes + stepper current on one small supply is the storm
        // that endangers a flash.
        masterOtaUploadActive = true;

        followerTxOtaCap(true);  // v1 #60 sag guard
        otaTxPowerReduced = true;

        uint32_t freeSpace = ESP.getFreeSketchSpace();
        if (freeSpace < 0x1000) {
          // #354: (freeSpace - 0x1000) below would underflow to a huge
          // maxSketchSpace and defeat the contentLen pre-check.
          otaRejection.set(507, String(F("No sketch space free: ")) + freeSpace);
          return;
        }
        uint32_t maxSketchSpace = (freeSpace - 0x1000) & 0xFFFFF000;
        size_t contentLen = request->contentLength();
        if (contentLen > 0 && contentLen > maxSketchSpace) {
          otaRejection.set(413, String(F("Firmware too large: ")) + contentLen +
                                    F(" bytes > maxSketchSpace ") +
                                    maxSketchSpace);
          return;
        }
        if (ESP.getFlashChipRealSize() < ESP.getFlashChipSize()) {
          // v1 #92/#94: Update.begin() would refuse everything.
          otaRejection.set(
              412, F("Flash config mismatch — reflash once over USB"));
          return;
        }
        // A plain image reserves the whole free space; a gzip one only
        // what it needs, so it is stored clear of where it unpacks to
        // (FollowerOtaImage.h).
        otaReservedBytes = otaUploadIsGzip
                               ? otaGzipReserve(contentLen, maxSketchSpace)
                               : maxSketchSpace;
        Update.runAsync(true);
        if (!Update.begin(otaReservedBytes, U_FLASH)) {
          // Stale updater state from an aborted upload (v1 #162).
          Update.end(false);
          Update.clearError();
          if (!Update.begin(otaReservedBytes, U_FLASH)) {
            otaRejection.set(500, String(F("Update.begin failed: ")) +
                                      Update.getErrorString());
            return;
          }
        }
        if (!Update.setMD5(md5.c_str())) {
          otaRejection.set(400, String(F("Update.setMD5 rejected '")) + md5 + "'");
          Update.end(false);
          return;
        }
        masterOtaOwnerRequest = request;
        request->onDisconnect([request]() {
          if (masterOtaOwnerRequest == request) {
            masterOtaOwnerRequest = nullptr;
          }
        });
      }
      if (masterOtaOwnerRequest != request) return;
      if (otaRejection.rejected()) return;
      if (!Update.hasError() && len > 0) {
        Update.write(data, len);
      }
      if (otaUploadIsGzip) otaGzipTail.feed(data, len);
      if (final) {
        if (otaUploadIsGzip &&
            !otaGzipUnpackFits(otaGzipTail, appAreaBytes(),
                               otaReservedBytes)) {
          // Nothing is staged for eboot: end(false) drops the session.
          Update.end(false);
          otaRejection.set(413, F("gzip image: unpacked it would not fit "
                                  "below its stored copy"));
          return;
        }
        if (!Update.end(true)) {
          // md5-mismatch end() latches the error but skips _reset (v1
          // #162) — a second end(false) clears the size state.
          Update.end(false);
        }
      }
    });
}

// --- endpoint registration ------------------------------------------------------------

void webEndpointsInit(AsyncWebServer& server) {
  // Pre-auth body-size guard (#347) — before any route so it wins the
  // first-match-wins scan for an oversized body.
  attachBodyLimitGuard(server);
  registerMasterFirmwareEndpoint(server);

  // Who this board is and what it runs. ota-flash.sh reads `version` and
  // `plat` here before and after an upload.
  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", identityJsonNow());
  });

  // Pairing: a master names itself (form field `master`); its address is the
  // caller's. Who may pair is FollowerPairPolicy.h. From then on this row
  // dials that master (FollowerLink.cpp); loop() writes the record. The
  // answer is this board's identity, or 409 naming the master it obeys.
  server.on("/pair", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (followerRejectCsrf(request)) return;
    const AsyncWebParameter* p = request->getParam("master", true);
    const String master = p != nullptr ? p->value() : String();
    const String caller = request->client()->remoteIP().toString();
    const FollowerClusterView cv = clusterViewGet();
    FollowerPairing now;
    now.paired = cv.phase != ClusterFollowerPhase::Standalone;
    now.masterId = cv.leaderName.c_str();
    now.masterHost = cv.leaderHost.c_str();
    now.masterInContact = clusterLeaderContactFresh();
    now.masterLost = cv.phase == ClusterFollowerPhase::LeaderLost;
    switch (followerPairDecide(master.c_str(), caller.c_str(), now)) {
      case PairVerdict::BadId:
        request->send(400, "text/plain",
                      F("'master' must be the master's id: 1 to 32 printable "
                        "characters, no spaces"));
        return;
      case PairVerdict::Refuse: {
        String out = F("{\"error\":\"paired\",\"master\":");
        appendJsonString(out, cv.leaderName);
        out += '}';
        request->send(409, "application/json", out);
        return;
      }
      case PairVerdict::Store:
        clusterPair(master, caller);
        break;
      case PairVerdict::Same:
        break;
    }
    request->send(200, "application/json", identityJsonNow());
  });
}

// --- loop side ----------------------------------------------------------------------

bool webOtaUploadFrozen() {
  if (!masterOtaUploadActive) return false;
  if (otaUploadStalled(masterOtaLastChunkMs, millis())) {
    SerialPrintln(F("OTA upload stalled >30 s — resuming normal operation"));
    masterOtaUploadActive = false;
    if (otaTxPowerReduced) {  // an abandoned upload must not freeze the ladder
      followerTxOtaCap(false);
      otaTxPowerReduced = false;
    }
    // Free the session slot too (v1 #191): a late chunk of the abandoned
    // request is then ignored, and the updater's buffer goes back to the heap
    // now instead of at the next upload.
    masterOtaOwnerRequest = nullptr;
    Update.end(false);
    Update.end(false);  // an end() on a latched error skips the reset (v1 #162)
    Update.clearError();
    return false;
  }
  return true;
}
