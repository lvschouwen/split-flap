// Master OTA + rescue-slot install/boot endpoints — split from
// WebEndpoints.cpp (#338); async-context rules in WebEndpoints.cpp's header.
// Deliberate async-context exception (v1 precedent): the firmware streams are
// written right here in the async_tcp task — an upload cannot be staged
// through a queue. Reboots and the ?v= write are still staged for the drain.

#include "WebEndpoints.h"
#include "WebEndpointsInternal.h"

#include <ESPAsyncWebServer.h>
#include <Update.h>

#include "FactorySlot.h"
#include "HelpersSerialHandling.h"
#include "MqttService.h"
#include "OtaService.h"
#include "ReflashPlan.h"
#include "Tasks.h"

// OTA upload rejection state (v1 ServiceBootModes pattern): the upload
// callback can't respond, so it records the rejection and the completion
// callback sends it. Both callbacks run in the async_tcp task — same task,
// no cross-task race, no mutex (uploads are serialized by the TCP stream).
static int otaRejectionStatus = 0;  // 0 = not rejected
static String otaRejectionReason;

// Concurrent-upload guard (#191): the request that owns the live Update
// session. A second overlapping POST must not abort/hijack it — the
// overlapping request is marked rejected via its _tempObject (freed by the
// request destructor) and answered 409, and it never touches the shared
// rejection state above, which belongs to the owner. Same async_tcp task
// for all callbacks — no lock needed. Compared only, never dereferenced.
static AsyncWebServerRequest* otaOwnerRequest = nullptr;

// Upload throughput measurement (#248): decides whether flash erase or the
// network dominates OTA wall time before any speed work is designed. Owned
// by the async_tcp task like the session state above.
static uint32_t otaUploadStartMs = 0;

// Stall watchdog (#313): last time a chunk of the live OTA arrived, stamped
// per-chunk. A slow-loris that opens /firmware/master and then stops (no
// preflight, MQTT frozen for the flash) would otherwise wedge OTA + MQTT
// indefinitely. webFirmwareLoop() (netTask) aborts a session idle past the
// timeout — the port of the ESP-01 follower's webOtaUploadFrozen(). Written
// by the async_tcp task, read (and cleared via the session) by netTask; a
// stalled session has no concurrent writer, so the plain uint32 is safe.
static uint32_t otaLastChunkMs = 0;
static const uint32_t OTA_STALL_TIMEOUT_MS = 30000UL;

// Same pattern for POST /firmware/rescue (#195). Separate state on purpose:
// a rescue install and a master OTA are different flows and must not read
// each other's leftovers. (Concurrent uploads remain #191 territory.)
static int rescueRejectionStatus = 0;
static String rescueRejectionReason;
// #347: which request actually began a factory-slot install, so onRequest
// never reports "installed" for a POST that carried no file part.
static AsyncWebServerRequest* rescueOwnerRequest = nullptr;

void webFirmwareRegister(AsyncWebServer& server) {
  // --- master OTA (#190) -----------------------------------------------------
  // v1 wire contract: POST multipart field "firmware" + mandatory ?md5=
  // (v1 #144) + optional ?v= intended-version diagnostic. Update targets
  // the inactive A/B slot; the image boots PENDING_VERIFY and OtaService
  // confirms it pre-inrush near the end of setup() (#305), netif-up as
  // fallback (bootloader reverts otherwise).
  server.on(
      "/firmware/master", HTTP_POST,
      [](AsyncWebServerRequest* request) {
        // Overlap-rejected upload (#191), or a bodyless POST racing a live
        // session: answer 409 without touching the owner's shared state,
        // MQTT freeze or Update session.
        if (request->_tempObject != nullptr ||
            (otaOwnerRequest != nullptr && otaOwnerRequest != request)) {
          request->send(409, "text/plain",
                        F("Another master OTA upload is already in progress "
                          "— retry when it finishes"));
          return;
        }
        // #347: did onUpload actually establish a session for THIS request?
        // A POST with no multipart file part never enters onUpload, so
        // Update.begin() and the in-onUpload CSRF/md5 gates never run — and
        // Update.isFinished() then reports success on a never-begun Update,
        // triggering a spurious, unauthenticated reboot. Capture before the
        // clear below.
        bool uploadRan = (otaOwnerRequest == request);
        otaOwnerRequest = nullptr;  // session concluded, whatever the verdict
        if (otaRejectionStatus != 0) {
          request->send(otaRejectionStatus, "text/plain", otaRejectionReason);
          return;
        }
        if (Update.hasError()) {
          mqttResumeAfterOta();  // no reboot coming — thaw the session (#116)
          request->send(500, "text/plain", String("Master OTA failed: ") +
                                               Update.errorString());
          return;
        }
        if (!uploadRan) {
          // #347: no file part streamed — nothing was flashed; never report
          // success (which would reboot). MQTT was never frozen (that happens
          // in onUpload), so no thaw needed.
          request->send(400, "text/plain",
                        F("No firmware in request (a multipart file part is "
                          "required)"));
          return;
        }
        if (!Update.isFinished()) {
          mqttResumeAfterOta();  // no reboot coming — thaw the session (#116)
          request->send(500, "text/plain",
                        F("Master OTA incomplete: upload ended before the "
                          "image was complete."));
          return;
        }
        request->send(200, "text/plain",
                      F("Master firmware flashed; rebooting…"));
        WebStateLock lock;
        pendingReboot = true;
        rebootRequestedAtMs = millis();
      },
      [](AsyncWebServerRequest* request, String filename, size_t index,
         uint8_t* data, size_t len, bool final) {
        if (index == 0) {
          // Concurrent-upload guard (#191): a live session owns the Update
          // singleton and the shared rejection state — mark this request
          // rejected (per-request _tempObject; malloc pairs with the free
          // in the request destructor) and leave both alone.
          if (otaOwnerRequest != nullptr && otaOwnerRequest != request) {
            request->_tempObject = malloc(1);
            return;
          }
          otaRejectionStatus = 0;
          otaRejectionReason = "";

          // CSRF gate (#313), INLINE before Update.begin: the middleware runs
          // too late for upload routes (post-body), so a forged cross-site
          // POST would otherwise flash + arm its image before the 403.
          if (webUploadCsrfRejected(request)) {
            otaRejectionStatus = 403;
            otaRejectionReason = "Cross-origin OTA refused (CSRF guard)";
            return;
          }

          // Reflash gate (#205): a master OTA reboots the S3 mid-unit-flash
          // and strands the in-flight unit in twiboot for no reason.
          if (reflashInProgress(displaySnapshotGet().reflash)) {
            otaRejectionStatus = 409;
            otaRejectionReason =
                "Unit reflash in progress — retry when it finishes";
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

          SerialPrintln("Master OTA upload started (md5 " + md5 + ")");
          if (Update.isRunning()) {
            Update.abort();  // stale aborted upload must not wedge this one
                             // (v1 #162 re-entry class)
          }
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            otaRejectionStatus = 500;
            otaRejectionReason =
                String("Master OTA could not start: ") + Update.errorString();
            return;
          }
          Update.setMD5(md5.c_str());
          // Freeze MQTT for the flash writes (v1 #116): staged — mqttTask
          // force-closes the session, the broker fires the "offline" LWT.
          mqttStopForOta();

          // ?v= staged unconditionally (empty if absent) so a stale value
          // from an earlier flash can't outlive this one (v1 #52 rationale),
          // through the same validator as every other settings write (#191).
          String intended = sanitizeIntendedVersion(
              request->hasParam("v") ? request->getParam("v")->value()
                                     : String());
          {
            WebStateLock lock;
            pendingIntendedVersion = intended;
            pendingIntendedVersionProvided = true;
          }

          // Session is live from here (#191). onDisconnect is the backstop
          // for a client that dies mid-upload: free the slot; the stale
          // Update session is aborted by the next upload's begin path above.
          otaOwnerRequest = request;
          otaUploadStartMs = millis();
          otaLastChunkMs = otaUploadStartMs;  // #313 stall watchdog baseline
          request->onDisconnect([request]() {
            if (otaOwnerRequest == request) otaOwnerRequest = nullptr;
          });
        }

        // Not (or no longer) the live owner (#191): covers overlap-rejected
        // requests AND stragglers rejected via the shared flag above whose
        // client keeps streaming — a later legitimate owner resets that
        // flag, and without this gate their leftover chunks would write
        // into the new owner's session.
        if (otaOwnerRequest != request) return;
        if (otaRejectionStatus != 0) return;
        otaLastChunkMs = millis();  // #313: progress resets the stall deadline

        if (len > 0 && Update.write(data, len) != len) {
          // Error is latched inside Update; the completion callback
          // reports it. Stop consuming flash time on further chunks.
          return;
        }
        if (final) {
          if (Update.end(true)) {
            uint32_t elapsedMs = millis() - otaUploadStartMs;
            uint32_t totalBytes = (uint32_t)(index + len);
            if (elapsedMs == 0) elapsedMs = 1;
            SerialPrintf(
                "Master OTA received %u bytes in %u ms (%u KB/s)\n",
                (unsigned)totalBytes, (unsigned)elapsedMs,
                (unsigned)(((uint64_t)totalBytes * 1000ULL / 1024ULL) /
                           elapsedMs));
            SerialPrintln(F("Master OTA image verified and armed — reboot "
                            "boots it PENDING_VERIFY"));
          } else {
            SerialPrintln(String("Master OTA failed at end: ") +
                          Update.errorString());
          }
        }
      });

  server.on("/debug/ota", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", otaDebugJson());
  });

  // --- factory rescue slot (#195) --------------------------------------------
  // Install/refresh the rescue image over WiFi: same wire contract as
  // /firmware/master (multipart field "firmware" + mandatory ?md5=), target
  // = the factory partition via FactorySlot's raw esp_partition writes.
  // Never touches otadata — installing a rescue image does not change what
  // boots next. Same async-context exception as the master OTA above: the
  // firmware stream is written right here in the async_tcp task.
  server.on(
      "/firmware/rescue", HTTP_POST,
      [](AsyncWebServerRequest* request) {
        if (rescueRejectionStatus != 0) {
          request->send(rescueRejectionStatus, "text/plain",
                        rescueRejectionReason);
          return;
        }
        // #347: capture/clear the per-request install marker (see master OTA).
        bool installRan = (rescueOwnerRequest == request);
        if (installRan) rescueOwnerRequest = nullptr;
        String err = factoryWriteError();
        if (err.length() > 0) {
          request->send(500, "text/plain", "Rescue install failed: " + err);
          return;
        }
        if (!installRan) {
          // No file part streamed — factoryWriteBegin never ran, the factory
          // slot is untouched; never report a false "installed".
          request->send(400, "text/plain",
                        F("No rescue image in request (a multipart file part "
                          "is required)"));
          return;
        }
        request->send(200, "text/plain",
                      F("Rescue image installed into the factory slot. No "
                        "reboot — POST /firmware/rescue-boot to test it."));
      },
      [](AsyncWebServerRequest* request, String filename, size_t index,
         uint8_t* data, size_t len, bool final) {
        if (index == 0) {
          // An install is already streaming: 409 without touching its state.
          // (The shared rejection flag stalls the in-flight upload too — the
          // erased header keeps that safe; clean per-request verdicts are
          // #191 territory.)
          if (factoryInstallInProgress()) {
            rescueRejectionStatus = 409;
            rescueRejectionReason =
                "another rescue install is in flight — let it finish (a "
                "dropped one expires after ~30 s) and retry";
            return;
          }
          rescueRejectionStatus = 0;
          rescueRejectionReason = "";

          // CSRF gate (#313), INLINE before factoryWriteBegin — the
          // middleware fires post-body, too late for an upload route.
          if (webUploadCsrfRejected(request)) {
            rescueRejectionStatus = 403;
            rescueRejectionReason = "Cross-origin rescue upload refused (CSRF "
                                    "guard)";
            return;
          }

          String md5 = request->hasParam("md5")
                           ? request->getParam("md5")->value()
                           : String();
          if (md5.length() == 0) {
            rescueRejectionStatus = 400;
            rescueRejectionReason =
                "md5 query parameter is required (compute it over the .bin "
                "and pass ?md5=...)";
            return;
          }
          if (!normalizeOtaMd5(md5)) {
            rescueRejectionStatus = 400;
            rescueRejectionReason = "md5 must be exactly 32 hex characters";
            return;
          }
          if (!factoryWriteBegin(md5)) {
            rescueRejectionStatus = 500;
            rescueRejectionReason =
                "Rescue install could not start: " + factoryWriteError();
            return;
          }
          rescueOwnerRequest = request;  // #347: a real install began here
        }

        if (rescueRejectionStatus != 0) return;

        if (len > 0 && !factoryWriteChunk(data, len, index)) {
          return;  // error latched in FactorySlot; completion reports it
        }
        if (final) {
          factoryWriteEnd();  // completion callback reads factoryWriteError()
        }
      });

  // Software entry into the rescue image — and the periodic "prove the
  // rescue image still boots" test (#195 spec). Guarded so a wall-mounted
  // device can't be pointed at an empty slot (the bootloader would fall
  // back to an OTA slot anyway, but the endpoint must stay honest as a
  // boot test). otadata erase + staged reboot; NVS untouched, so WiFi
  // credentials survive into rescue (#193 invariant).
  server.on("/firmware/rescue-boot", HTTP_POST,
            [](AsyncWebServerRequest* request) {
              if (factoryInstallInProgress()) {
                request->send(409, "text/plain",
                              F("A rescue image install is in flight — let "
                                "it finish, then retry."));
                return;
              }
              if (!factorySlotImageValid()) {
                request->send(409, "text/plain",
                              F("Factory slot holds no valid rescue image — "
                                "POST it to /firmware/rescue first."));
                return;
              }
              if (!rescueBootArm()) {
                request->send(500, "text/plain",
                              F("otadata erase failed — rescue boot not "
                                "armed."));
                return;
              }
              request->send(200, "text/plain",
                            F("Rebooting into the rescue image… it joins "
                              "WiFi (or opens <name>-rescue) and serves the "
                              "recovery page."));
              WebStateLock lock;
              pendingReboot = true;
              rebootRequestedAtMs = millis();
            });
}

// OTA stall watchdog (#313), drained first in webEndpointsLoop(): a session
// that opened /firmware/master and went silent has frozen MQTT
// (mqttStopForOta) and holds the Update slot. If no chunk has landed for the
// timeout, thaw everything — the next upload's Update.begin() recovers the
// abandoned session (v1 #191). Mirror of the ESP-01 follower's
// webOtaUploadFrozen(); the stalled peer is parked in socket-read, not
// writing flash, so there is no concurrent Update use.
void webFirmwareLoop() {
  if (otaOwnerRequest != nullptr &&
      millis() - otaLastChunkMs > OTA_STALL_TIMEOUT_MS) {
    SerialPrintln(F("Master OTA upload stalled >30 s — aborting and resuming "
                    "normal operation"));
    if (Update.isRunning()) Update.abort();
    otaOwnerRequest = nullptr;
    mqttResumeAfterOta();
  }
}
