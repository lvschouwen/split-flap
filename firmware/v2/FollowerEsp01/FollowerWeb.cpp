// FollowerWeb.cpp — endpoint glue (#298). Contract + context rules in
// FollowerWeb.h. The /cluster/* handlers mirror the v2 master's follower
// endpoints (WebEndpoints.cpp) minus digest/promote; /firmware/master is
// v1's ESP8266 Update flow trimmed (no RTC verdict cookie — ota-flash.sh's
// version comparison is the revert detector on this board).

#include "ClusterQuiet.h"  // the ping's quiet flag (#227)
#include "FollowerEscalation.h"
#include "FollowerWeb.h"

#include <ESP8266WiFi.h>
#include <memory>
#include <new>
#include <Updater.h>
#include <flash_hal.h>  // FS_start: the end of the app area

#include "BuildVersion.h"
#include "ApiIndex.h"
#include "ApiIndexAsset.h"  // #519: the GET /api reply, in flash
#include "BootDump.h"  // #522: boot-section dump slot + CRC + JSON
#include "BootInfo.h"  // #499: read-only boot report slot + JSON
#include "FollowerBus.h"
#include "ClusterHmac.h"  // #313 follow-on: rebuild canonical msgs for verify
#include "FollowerCluster.h"
#include "FollowerConfig.h"
#include "FollowerCors.h"
#include "ClusterForeign.h"
#include "FollowerJson.h"
#include "FollowerOtaImage.h"  // #540: gzip upload checks
#include "FollowerPrefs.h"     // #513: reflashOnBoot
#include "FollowerResetLog.h"  // #503: reset history in /cluster/health
#include "FollowerRescue.h"  // #343: beacon marker + op lockout
#include "FollowerUpdate.h"  // the wall link's download owns the updater meanwhile
#include "FollowerSettings.h"
#include "FollowerWifi.h"
#include "OtaUploadGate.h"  // shared gate / completion / stall rules
#include "UnitTimings.h"
#include "SelfTestPoll.h"  // the shared self-test wait (#529)
#include "WearPolicy.h"
#include "WebBodyLimitGuard.h"  // pre-auth body-size guard (#347)

volatile bool isPendingReboot = false;
static volatile bool masterOtaUploadActive = false;
static volatile unsigned long masterOtaLastChunkMs = 0;

// --- staged work (handlers set, webLoopTick drains) ---------------------------------

static volatile bool unitHealthRefreshPending = false;
static volatile bool reflashPending = false;

struct StagedOp {
  volatile bool pending = false;
  uint32_t seq = 0;
  FollowerOpKind kind = FollowerOpKind::None;
  uint8_t addr = 0;
  long arg = 0;
};
static StagedOp stagedOp;
static MaintResult opResult;
static SelfTestSlot selfTestSlot;
static BootInfoSlot bootInfoSlot;  // #499: last read-only boot report
static BootDumpSlot bootDumpSlot;  // #522: last boot-section dump result
// The raw 1 KB section lives on the heap and only around a dump: claimed by
// the request that stages one, given back by loop() once the result has had
// time to be fetched or a reflash job needs the room.
static uint8_t* bootDumpBytes = nullptr;
static uint32_t bootDumpBytesSeq = 0;  // seq that wrote bootDumpBytes
static uint32_t bootDumpBytesAtMs = 0;  // claimed or last written
#define BOOT_DUMP_KEEP_MS (5UL * 60UL * 1000UL)
static uint32_t maintSeqCounter = 0;
// A staged Probe op waiting for its scan (0 = none): stamped when it has run.
static uint32_t probeOpSeq = 0;

// Self-test poll state (the unit measures ~2 revolutions; we poll its
// GET_SELF_TEST until it stops reporting "running").
static bool selfTestPolling = false;
static SelfTestPoll selfTestPoll;
static uint32_t selfTestPollLastMs = 0;

// --- OTA session state (v1 #191 conventions) ----------------------------------------

static AsyncWebServerRequest* volatile masterOtaOwnerRequest = nullptr;

// #358: refused foreign-leader contacts, surfaced in /cluster/health.
// Handler-context only (the ESP-01's async handlers and loop() cooperate on
// one core), RAM-only, resets on reboot.
static ForeignContactStats foreignContacts;
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

// #294 rung 3 CORS: per-response reflection (the ESP8266 async fork has no
// middleware). Simple requests only — no preflight handler needed.
// operator new aborts on this core when the heap cannot serve it — the board
// resets instead of answering. A handler about to make a large allocation asks
// first and answers 503; the margin covers the web library's send buffer and
// response objects on top of the block itself.
static bool heapCanHold(size_t bytes) {
  return ESP.getMaxFreeBlockSize() >= bytes + 1536;
}

// loop() only: the result handler copies out of the block without yielding,
// so it can never see it go.
static void releaseBootDumpBytes() {
  if (bootDumpBytes == nullptr) return;
  delete[] bootDumpBytes;
  bootDumpBytes = nullptr;
  bootDumpBytesSeq = 0;
}

// For replies that are not built from a String (flash content, streamed or
// callback-filled bodies): the same CORS decision, then send.
static void sendResponseWithCors(AsyncWebServerRequest* request,
                                 AsyncWebServerResponse* response) {
  if (request->hasHeader("Origin") &&
      followerCorsPathAllowed(request->url())) {
    const String origin = request->header("Origin");
    if (lanOriginAllowed(origin)) {
      response->addHeader("Access-Control-Allow-Origin", origin);
      response->addHeader("Vary", "Origin");
    }
  }
  request->send(response);
}

static void sendWithCors(AsyncWebServerRequest* request, int status,
                         const String& contentType, const String& body) {
  sendResponseWithCors(request,
                       request->beginResponse(status, contentType, body));
}

// #313 CSRF gate for the middleware-less ESP8266 fork: call at the top of
// every mutating handler. Returns true (and answers 403) when the request is
// a forged cross-site POST — a browser POST whose Origin is not a LAN pane.
// The leader's server-to-server calls send no Origin and pass; the wall's
// own LAN UI sends a LAN origin and passes.
static bool followerRejectCsrf(AsyncWebServerRequest* request) {
  bool hasOrigin = request->hasHeader("Origin");
  String origin = hasOrigin ? request->header("Origin") : String();
  if (lanCsrfRejectPost(request->method() == HTTP_POST, hasOrigin,
                             origin)) {
    request->send(403, "text/plain",
                  F("Cross-origin POST refused (CSRF guard)"));
    return true;
  }
  return false;
}

static FollowerVitals vitalsNow() {
  FollowerVitals v;
  v.heapBytes = ESP.getFreeHeap();
  v.rssiDbm = WiFi.RSSI();
  v.upSeconds = millis() / 1000;
  return v;
}

// #540: how much of the app area the running image takes — what is left is
// where an OTA upload is stored.
static FollowerFlashInfo flashInfoNow() {
  FollowerFlashInfo f;
  f.sketchBytes = ESP.getSketchSize();
  f.sketchFreeBytes = ESP.getFreeSketchSpace();
  f.flashMode = (int)ESP.getFlashChipMode();
  f.flashId = ESP.getFlashChipId();
  return f;
}

// Health facts snapshot for the join/ping replies (#294 keys).
static ClusterRowHealth healthNow(char* maskBuf, size_t maskCap) {
  ClusterRowHealth h;
  h.width = displayWidth;
  int detected = 0;
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    if (unitFacts[i].state != 0) detected++;
  }
  h.detected = detected;
  h.faulty = computeFaultyUnitCount(unitFacts, UNITS_AMOUNT);
  unitFaultMaskHex(unitFacts, displayWidth, maskBuf, maskCap);
  h.faultMask = maskBuf;
  h.lost = computeLostUnitCount(unitFacts, UNITS_AMOUNT);
  h.busDead = followerBusRecovery().dead;
  WearAssessment wear;
  assessWear(unitFacts, displayWidth, wear);
  h.wear = wear.flaggedCount > 0;
  return h;
}

// Body form param (the cluster wire posts form-encoded bodies).
static bool paramString(AsyncWebServerRequest* request, const char* name,
                        String& out) {
  if (!request->hasParam(name, true)) return false;
  out = request->getParam(name, true)->value();
  return true;
}

// Required numeric QUERY param (v2 parity: the maintenance ops ride the
// query string — postCalibration() posts `path?address=..`; strtol base 0
// keeps v1's hex support). Sends the 400 itself.
static bool queryRequireLong(AsyncWebServerRequest* request, const char* name,
                             long& out) {
  if (!request->hasParam(name)) {
    String msg = "Missing '";
    msg += name;
    msg += "' query param";
    sendWithCors(request, 400, "text/plain", msg);
    return false;
  }
  String raw = request->getParam(name)->value();
  char* end = nullptr;
  out = strtol(raw.c_str(), &end, 0);
  if (end == raw.c_str()) {
    String msg = "'";
    msg += name;
    msg += "' must be a number";
    sendWithCors(request, 400, "text/plain", msg);
    return false;
  }
  return true;
}

// Busy gate for the {"seq":N} ops: one staged slot, and the reflash job /
// a waiting render own the bus first (mutual 409/503 discipline).
static bool opSlotBusy() {
  return stagedOp.pending || selfTestPolling || reflashPending ||
         probeOpSeq != 0 || reflashInProgress(reflashProgress);
}

UnitOpStaged unitOpStage(FollowerOpKind kind, uint8_t addr, long arg,
                         uint32_t& seq) {
  // #343: the beacon never touches the bus — flash new firmware first.
  if (rescueActive()) return UnitOpStaged::Rescue;
  if (opSlotBusy()) return UnitOpStaged::Busy;
  if (kind == FollowerOpKind::BootDump) {
    // Claimed only when the op will be staged: a refused request must not
    // leave 1 KB held through the reflash that made the slot busy.
    if (bootDumpBytes == nullptr) {
      if (!heapCanHold(BOOT_SECTION_LEN)) return UnitOpStaged::NoMemory;
      bootDumpBytes = new (std::nothrow) uint8_t[BOOT_SECTION_LEN];
      if (bootDumpBytes == nullptr) return UnitOpStaged::NoMemory;
    }
    bootDumpBytesAtMs = millis();
  }
  stagedOp.seq = ++maintSeqCounter;
  stagedOp.kind = kind;
  stagedOp.addr = addr;
  stagedOp.arg = arg;
  stagedOp.pending = true;  // set last (v1 flag-handoff rule)
  seq = stagedOp.seq;
  return UnitOpStaged::Yes;
}

bool unitOpsBusy() { return opSlotBusy(); }
const MaintResult& unitOpResult() { return opResult; }
const SelfTestSlot& unitOpSelfTest() { return selfTestSlot; }
const BootInfoSlot& unitOpBootInfo() { return bootInfoSlot; }
const uint8_t* unitOpBootDumpBytes(uint32_t seq) {
  return bootDumpBytesSeq == seq ? bootDumpBytes : nullptr;
}

static void stageOp(AsyncWebServerRequest* request, FollowerOpKind kind,
                    uint8_t addr, long arg) {
  uint32_t seq = 0;
  switch (unitOpStage(kind, addr, arg, seq)) {
    case UnitOpStaged::Rescue:
      sendWithCors(request, 409, "text/plain",
                   F("Rescue beacon active — unit ops disabled until a "
                     "firmware push"));
      return;
    case UnitOpStaged::Busy:
      sendWithCors(request, 503, "text/plain",
                   F("Another unit operation is in progress — try again"));
      return;
    case UnitOpStaged::NoMemory:
      sendWithCors(request, 503, "text/plain", F("out of memory — retry"));
      return;
    case UnitOpStaged::Yes:
      break;
  }
  char buf[24];
  snprintf(buf, sizeof(buf), "{\"seq\":%lu}", (unsigned long)seq);
  sendWithCors(request, 200, "application/json", buf);
}

size_t unitsHealthJson(char* buf, size_t cap) {
  int faulty = computeFaultyUnitCount(unitFacts, UNITS_AMOUNT);
  size_t n = buildUnitHealthJson(buf, cap, unitFacts, displayWidth, faulty,
                                 SFP_I2C_ADDRESS_BASE, millis());
  if (n == 0 || n >= cap) {
    n = (size_t)snprintf(buf, cap, "{\"width\":%d,\"faulty\":%d,\"units\":[]}",
                         displayWidth, faulty);
  }
  // Wear + reflash progress splices (v2 additive keys — same payload the
  // S3 member panel reads).
  WearAssessment wear;
  assessWear(unitFacts, UNITS_AMOUNT, wear);
  char wearJson[96];
  size_t wearLen = buildWearJson(wear, wearJson, sizeof(wearJson));
  if (n > 0 && wearLen < sizeof(wearJson) && n + wearLen + 2 < cap) {
    n += (size_t)snprintf(buf + n - 1, cap - n + 1, ",%s}", wearJson) - 1;
  }
  char reflashJson[REFLASH_JSON_CAP];
  buildReflashJson(reflashJson, sizeof(reflashJson), reflashProgress);
  if (n > 0 && n + strlen(reflashJson) + 13 < cap) {
    n += (size_t)snprintf(buf + n - 1, cap - n + 1, ",\"reflash\":%s}",
                          reflashJson) - 1;
  }
  return n;
}

// Query-string address (v2 parity — see queryRequireLong).
static bool checkAddressParam(AsyncWebServerRequest* request, int& outAddr) {
  const char* raw = nullptr;
  String value;
  if (request->hasParam("address")) {
    value = request->getParam("address")->value();
    raw = value.c_str();
  }
  MaintVerdict verdict =
      maintValidateAddress(raw, unitFacts, UNITS_AMOUNT, outAddr);
  if (verdict.httpStatus != 200) {
    sendWithCors(request, verdict.httpStatus, "text/plain", verdict.message);
    return false;
  }
  return true;
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
            lanCsrfRejectPost(true, hasOrigin,
                              hasOrigin ? request->header("Origin") : String()),
            reflashPending || reflashInProgress(reflashProgress), md5);
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

  // Self-documenting route + terse-key legend index for the headless
  // operator (#308). The reply is fixed text, rendered at build time into
  // flash (ApiIndexAsset.h) and sent from there: no buffer, no copy (#519).
  server.on("/api", HTTP_GET, [](AsyncWebServerRequest* request) {
    sendResponseWithCors(
        request, request->beginResponse(200, "application/json",
                                        API_INDEX_JSON, API_INDEX_JSON_LEN));
  });

  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest* request) {
    FollowerClusterView cv = clusterViewGet();
    sendWithCors(request, 200, "application/json",
                 followerSettingsJson(effectiveDeviceName, GIT_REV,
                                      displayWidth,
                                      followerPhaseName(cv.phase),
                                      cv.leaderName, cv.leaderHost, cv.row,
                                      vitalsNow(), flashInfoNow(),
                                      followerTxPowerDbm10(),
                                      prefsReflashOnBoot()));
  });

  // #513: the one operator setting. `reflashOnBoot=true|false` as a query or
  // form parameter (the S3 takes the same form field on POST /). Staged here,
  // persisted by loop(); GET /settings reports the persisted value.
  server.on("/settings", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (followerRejectCsrf(request)) return;
    const AsyncWebParameter* p = request->getParam("reflashOnBoot");
    if (p == nullptr) p = request->getParam("reflashOnBoot", true);
    if (p == nullptr) {
      sendWithCors(request, 400, "text/plain",
                   F("Missing 'reflashOnBoot' param"));
      return;
    }
    bool want = true;
    if (!followerParseBool(p->value().c_str(), want)) {
      sendWithCors(request, 400, "text/plain",
                   F("reflashOnBoot must be true or false"));
      return;
    }
    prefsStageReflashOnBoot(want);
    sendWithCors(request, 200, "text/plain",
                 want ? F("reflashOnBoot=true") : F("reflashOnBoot=false"));
  });

  // #318 E: the row's in-RAM log, cursor-paged so the leader pulls only new
  // lines (GET /log?after=<cursor>). Body = "<nextCursor>\n<delta>"; the
  // leader parses the first line and tees the rest into the fleet log. A
  // human hitting /log with no cursor gets the whole ring. Deliberately NOT
  // on the CORS surface — the pull is server-to-server; browsers read the
  // fleet log from the master's /log/flash.
  server.on("/log", HTTP_GET, [](AsyncWebServerRequest* request) {
    uint32_t after = 0;
    if (request->hasParam("after")) {
      after = (uint32_t)strtoul(request->getParam("after")->value().c_str(),
                                nullptr, 10);
    }
    // One copy, ring -> response buffer (#519). The cursor line comes first;
    // the next cursor is the ring's write cursor, known before reading.
    const FollowerLogRing& ring = followerLogRing();
    const size_t streamBytes = ring.countSince(after) + 16;
    if (!heapCanHold(streamBytes)) {
      sendWithCors(request, 503, "text/plain", F("out of memory — retry"));
      return;
    }
    AsyncResponseStream* response =
        request->beginResponseStream("text/plain", streamBytes);
    response->print(ring.written);
    response->print('\n');
    ring.readSinceInto(after, [response](const char* data, size_t len) {
      response->write((const uint8_t*)data, len);
    });
    sendResponseWithCors(request, response);
  });

  server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (followerRejectCsrf(request)) return;
    isPendingReboot = true;
    sendWithCors(request, 200, "text/plain", F("Rebooting…"));
  });

  // --- cluster wire (#272 contract, mirrored from the v2 follower) ----------

  server.on("/cluster/join", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (followerRejectCsrf(request)) return;
    String leaderHost, rowStr, epochStr;
    if (!paramString(request, "leaderHost", leaderHost) ||
        !paramString(request, "row", rowStr) ||
        !paramString(request, "epoch", epochStr)) {
      request->send(400, "text/plain", F("Missing leaderHost/row/epoch"));
      return;
    }
    String leaderName;
    if (!paramString(request, "leaderName", leaderName)) {
      leaderName = leaderHost;
    }
    long row = rowStr.toInt();
    uint32_t epoch = (uint32_t)strtoul(epochStr.c_str(), nullptr, 10);
    ClusterJoinCheck check = clusterJoinValidate(
        row, leaderHost, leaderName, FOLLOWER_NAME_MAX,
        request->client()->remoteIP().toString());
    if (check != ClusterJoinCheck::Ok) {
      request->send(clusterJoinCheckStatus(check), "text/plain",
                    clusterJoinCheckMessage(check));
      return;
    }
    // Sticky leadership (#295 semantics): while our leader is demonstrably
    // alive, a DIFFERENT leader's join is refused with its identity.
    String curName, curHost;
    if (clusterJoinWouldConflict(leaderHost, curName, curHost)) {
      foreignContactRecord(foreignContacts, ForeignContactKind::Join,
                           request->client()->remoteIP().toString(), millis());
      SerialPrintln(String(F("Foreign join refused from ")) +
                    request->client()->remoteIP().toString() +
                    F(" — leader is ") + curHost);
      String out = "{\"error\":\"other-leader\",\"leaderHost\":";
      appendJsonString(out, curHost);
      out += ",\"leaderName\":";
      appendJsonString(out, curName);
      out += '}';
      request->send(409, "application/json", out);
      return;
    }
    // #313 follow-on: the leader's per-member wire-auth key (absent from a
    // pre-HMAC leader → enforcement stays off).
    String key;
    paramString(request, "key", key);
    // #342 additive: the leader's POSIX zone for the clock fallback. A bad
    // value is dropped (not 400) — the join must survive a pre-#342 wire.
    String tz;
    paramString(request, "tz", tz);
    if (tz.length() > FOLLOWER_TZ_MAX || !clusterWirePrintable(tz, 0x21)) {
      tz = "";
    }
    clusterHandleJoin(leaderName, leaderHost, (int)row, epoch, key, tz);
    char mask[16];
    ClusterRowHealth h = healthNow(mask, sizeof(mask));
    request->send(200, "application/json",
                  followerJoinReplyJson(effectiveDeviceName, GIT_REV, h,
                                        vitalsNow(), rescueActive()));
  });

  server.on("/cluster/render", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (followerRejectCsrf(request)) return;
    // Source-IP binding (#313): only the joined leader drives this row. Never
    // called by a browser UI (leader wire only), so a strict bind is safe.
    FollowerClusterView rcv = clusterViewGet();
    if (clusterCallerIsForeign(rcv.leaderHost,
                               request->client()->remoteIP().toString())) {
      foreignContactRecord(foreignContacts, ForeignContactKind::Render,
                           request->client()->remoteIP().toString(), millis());
      SerialPrintln(String(F("Foreign render refused from ")) +
                    request->client()->remoteIP().toString() +
                    F(" — leader is ") + rcv.leaderHost);
      request->send(403, "text/plain", F("render must come from the leader"));
      return;
    }
    String epochStr, seqStr, text;
    if (!paramString(request, "epoch", epochStr) ||
        !paramString(request, "seq", seqStr) ||
        !paramString(request, "text", text)) {
      request->send(400, "text/plain", F("Missing epoch/seq/text"));
      return;
    }
    uint32_t epoch = (uint32_t)strtoul(epochStr.c_str(), nullptr, 10);
    uint32_t seq = (uint32_t)strtoul(seqStr.c_str(), nullptr, 10);
    int speed = 80;
    String speedStr;
    if (paramString(request, "speed", speedStr)) {
      speed = speedStr.toInt();
      if (speed < 1 || speed > 100) {
        request->send(400, "text/plain", F("Speed must be 1..100"));
        return;
      }
    }
    String commitStr;
    uint64_t commitAtMs = 0;
    if (paramString(request, "commitAtMs", commitStr)) {
      commitAtMs = strtoull(commitStr.c_str(), nullptr, 10);
    }
    // Wire-auth (#313 follow-on): verify BEFORE truncation — the leader signs
    // the untruncated segment, so the canonical message must use the raw text.
    if (clusterHmacEnforced()) {
      String tsStr, mac;
      if (!paramString(request, "ts", tsStr) ||
          !paramString(request, "mac", mac)) {
        request->send(403, "text/plain", F("cluster signature required"));
        return;
      }
      uint64_t ts = strtoull(tsStr.c_str(), nullptr, 10);
      String msg = clusterHmacRenderMsg(ts, epoch, seq, text, speed, commitAtMs);
      if (!clusterVerifySigned(msg, ts, mac)) {
        request->send(403, "text/plain", F("bad cluster signature"));
        return;
      }
    }
    // Segments render verbatim; bound the length like every text producer.
    if ((int)text.length() > UNITS_AMOUNT) {
      text = text.substring(0, UNITS_AMOUNT);
    }
    ClusterRenderVerdict v =
        clusterHandleRender(epoch, seq, text, speed, commitAtMs);
    if (v == ClusterRenderVerdict::NotClustered) {
      request->send(409, "application/json",
                    F("{\"error\":\"not clustered\"}"));
      return;
    }
    String out = "{\"applied\":";
    out += (v == ClusterRenderVerdict::Apply) ? "true" : "false";
    out += ",\"seq\":";
    out += String((unsigned long)seq);
    out += '}';
    request->send(200, "application/json", out);
  });

  server.on("/cluster/ping", HTTP_POST, [](AsyncWebServerRequest* request) {
#if CLUSTER_WIRE_DEBUG
    // #386 bench trace: proves the ping REACHED the handler. If the leader
    // logs a failure and no ENTER line appears in GET /log, the request died
    // earlier (body guard 413, CSRF, or it never arrived).
    SerialPrintln("dbg/wire: ping ENTER from " +
                  request->client()->remoteIP().toString() + " len=" +
                  String((unsigned long)request->contentLength()));
#endif
    if (followerRejectCsrf(request)) return;
    // Source-IP binding (#313): only the joined leader's ping keeps this row
    // alive — a foreign ping must not refresh the contact-fresh window.
    FollowerClusterView pcv = clusterViewGet();
    if (clusterCallerIsForeign(pcv.leaderHost,
                               request->client()->remoteIP().toString())) {
      foreignContactRecord(foreignContacts, ForeignContactKind::Ping,
                           request->client()->remoteIP().toString(), millis());
      SerialPrintln(String(F("Foreign ping refused from ")) +
                    request->client()->remoteIP().toString() +
                    F(" — leader is ") + pcv.leaderHost);
      request->send(403, "text/plain", F("ping must come from the leader"));
      return;
    }
    // digest=/you= piggyback params have no functional consumer here (#298:
    // never a takeover candidate) — but the leader's mac binds them (#313
    // follow-on HIGH#2), so read them back to reconstruct the exact signed
    // canonical below. Absent ⇒ "" / -1, matching the leader's empty case.
    String digest;
    paramString(request, "digest", digest);
    String youStr;
    int youIndex = paramString(request, "you", youStr) ? youStr.toInt() : -1;
    // Wire-auth (#313 follow-on): a keyed follower requires a valid ts+mac
    // before contact is refreshed.
    const bool keyed = clusterHmacEnforced();
    uint64_t pingTs = 0;
    if (keyed) {
      String tsStr, mac;
      if (!paramString(request, "ts", tsStr) ||
          !paramString(request, "mac", mac)) {
#if CLUSTER_WIRE_DEBUG
        SerialPrintln(F("dbg/wire: ping REJECT 403 — ts/mac absent"));
#endif
        request->send(403, "text/plain", F("cluster signature required"));
        return;
      }
      uint64_t ts = strtoull(tsStr.c_str(), nullptr, 10);
      pingTs = ts;
      if (!clusterVerifySigned(clusterHmacPingMsg(ts, digest, youIndex), ts,
                               mac)) {
#if CLUSTER_WIRE_DEBUG
        // #386: separates a key mismatch from a replay-window/mark reject —
        // both return the same 403 to the leader.
        SerialPrintln("dbg/wire: ping REJECT 403 — bad sig (digestLen=" +
                      String(digest.length()) + " you=" + String(youIndex) +
                      ")");
#endif
        request->send(403, "text/plain", F("bad cluster signature"));
        return;
      }
    }
    if (!clusterHandlePing()) {
#if CLUSTER_WIRE_DEBUG
      SerialPrintln(F("dbg/wire: ping REJECT 409 — handlePing declined"));
#endif
      request->send(409, "application/json",
                    F("{\"error\":\"not clustered\"}"));
      return;
    }
    // #227: only an accepted ping speaks for the leader, and a keyed row
    // takes the quiet flag only with its own mac over this ping's timestamp.
    String quietStr, quietMac;
    const bool quietPresent =
        paramString(request, CLUSTER_PING_QUIET_PARAM, quietStr);
    bool quietMacOk = false;
    if (keyed && quietPresent &&
        paramString(request, CLUSTER_PING_QUIET_MAC_PARAM, quietMac)) {
      quietMacOk = clusterMacMatches(
          clusterQuietMsg(pingTs, clusterQuietFromPing(quietStr.c_str())),
          quietMac);
    }
    bool leaderQuiet = false;
    if (clusterQuietAccept(quietPresent, quietStr.c_str(), keyed, quietMacOk,
                           leaderQuiet)) {
      clusterNoteLeaderQuiet(leaderQuiet);
    }
    FollowerClusterView cv = clusterViewGet();
    char mask[16];
    ClusterRowHealth h = healthNow(mask, sizeof(mask));
    request->send(200, "application/json",
                  followerPingReplyJson(followerPhaseName(cv.phase), cv.epoch,
                                        cv.lastSeq, h, vitalsNow(), GIT_REV,
                                        rescueActive()));
  });

  server.on("/cluster/leave", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (followerRejectCsrf(request)) return;
    // leave has two legitimate callers — the leader's reconfigure fan-out and
    // the wall's local "Leave" button (a LAN browser).
    FollowerClusterView lcv = clusterViewGet();
    bool fromLanBrowser = request->hasHeader("Origin") &&
                          lanOriginAllowed(request->header("Origin"));
    bool keyed = clusterHmacEnforced();
    bool signedOk = false;
    String tsStr, mac;
    if (keyed && paramString(request, "ts", tsStr) &&
        paramString(request, "mac", mac)) {
      uint64_t ts = strtoull(tsStr.c_str(), nullptr, 10);
      signedOk = clusterVerifySigned(clusterHmacLeaveMsg(ts), ts, mac);
    }
    if (!clusterLeaveAllowed(keyed, signedOk, lcv.leaderHost,
                             request->client()->remoteIP().toString(),
                             fromLanBrowser)) {
      request->send(403, "text/plain",
                    keyed ? F("leave requires a valid signature or this row's "
                              "web UI")
                          : F("leave must come from the leader or this row's "
                              "web UI"));
      return;
    }
    clusterHandleLeave();  // idempotent
    request->send(200, "text/plain", F("ok"));
  });

  server.on("/cluster/health", HTTP_GET, [](AsyncWebServerRequest* request) {
    FollowerClusterView cv = clusterViewGet();
    int detected = 0;
    for (int i = 0; i < UNITS_AMOUNT; i++) {
      if (unitFacts[i].state != 0) detected++;
    }
    FollowerClusterDiag diag;
    diag.msSinceRender = cv.msSinceRender;
    diag.secsUntilBlank = cv.secsUntilBlank;
    diag.i2cTx = followerBusTxCount();
    diag.i2cErr = followerBusErrCount();
    diag.minHeap = followerMinHeap();
    diag.stackFree = ESP.getFreeContStack();  // #435: painted low-water
    diag.sntpSynced = cv.sntpSynced;
    diag.hmac = clusterHmacEnforced();
    diag.foreign = foreignContacts;  // #358
    diag.nowMs = millis();
    diag.bus = followerBusRecovery();  // #488
    diag.resets = &resetLogGet();      // #503
    diag.escalation = &escalationRecordGet();
    request->send(200, "application/json",
                  followerClusterHealthJson(
                      followerPhaseName(cv.phase), cv.leaderName,
                      cv.leaderHost, cv.row, cv.epoch, cv.lastSeq,
                      cv.heldSegment, GIT_REV, displayWidth, detected,
                      computeFaultyUnitCount(unitFacts, UNITS_AMOUNT), diag));
  });

  // --- unit health (v1/v2 shared wire shape) --------------------------------

  server.on("/units/health", HTTP_GET, [](AsyncWebServerRequest* request) {
    // Built in a buffer sized for THIS row and freed with the response (#519):
    // the 16-unit worst case used to sit in RAM permanently, 8 KB for a reply
    // that is 1.6 KB on a 5-unit row. The callback response reads straight
    // from the buffer, so there is no second copy.
    const size_t cap = followerHealthBufCap(displayWidth, UNITS_AMOUNT);
    if (!heapCanHold(cap)) {
      sendWithCors(request, 503, "text/plain", F("out of memory — retry"));
      return;
    }
    std::shared_ptr<char> held(new (std::nothrow) char[cap],
                               std::default_delete<char[]>());
    char* buf = held.get();
    if (buf == nullptr) {
      sendWithCors(request, 503, "text/plain", F("out of memory — retry"));
      return;
    }
    size_t n = unitsHealthJson(buf, cap);
    sendResponseWithCors(
        request,
        request->beginResponse(
            "application/json", n,
            [held, n](uint8_t* out, size_t maxLen, size_t index) -> size_t {
              if (index >= n) return 0;
              size_t take = n - index;
              if (take > maxLen) take = maxLen;
              memcpy(out, held.get() + index, take);
              return take;
            }));
  });

  server.on("/units/health/refresh", HTTP_POST,
            [](AsyncWebServerRequest* request) {
              if (followerRejectCsrf(request)) return;
              if (rescueActive()) {
                sendWithCors(request, 409, "application/json",
                             F("{\"status\":\"rescue\"}"));
                return;
              }
              if (reflashPending || reflashInProgress(reflashProgress)) {
                sendWithCors(request, 503, "application/json",
                             F("{\"status\":\"busy\"}"));
                return;
              }
              unitHealthRefreshPending = true;
              sendWithCors(request, 202, "application/json",
                           F("{\"status\":\"pending\"}"));
            });

  // --- {"seq":N} maintenance ops (#204 contract subset) ----------------------

  server.on("/unit/offset", HTTP_GET, [](AsyncWebServerRequest* request) {
    // GET params ride the query string, not the body.
    const char* raw = nullptr;
    String value;
    if (request->hasParam("address")) {
      value = request->getParam("address")->value();
      raw = value.c_str();
    }
    int addr = 0;
    MaintVerdict verdict =
        maintValidateAddress(raw, unitFacts, UNITS_AMOUNT, addr);
    if (verdict.httpStatus != 200) {
      sendWithCors(request, verdict.httpStatus, "text/plain",
                   verdict.message);
      return;
    }
    const UnitFacts& unit = unitFacts[addr - SFP_I2C_ADDRESS_BASE];
    if (!unit.offsetValid) {
      sendWithCors(request, 502, "text/plain",
                   F("Unit did not return a valid offset (firmware may "
                     "predate #32)"));
      return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"offset\":%d}", (int)unit.offset);
    sendWithCors(request, 200, "application/json", buf);
  });

  server.on("/unit/offset", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (followerRejectCsrf(request)) return;
    int addr = 0;
    if (!checkAddressParam(request, addr)) return;
    long value = 0;
    if (!queryRequireLong(request, "value", value)) return;
    MaintVerdict verdict = maintValidateOffset(value);
    if (verdict.httpStatus != 200) {
      sendWithCors(request, verdict.httpStatus, "text/plain",
                   verdict.message);
      return;
    }
    stageOp(request, FollowerOpKind::WriteOffset, (uint8_t)addr, value);
  });

  server.on("/unit/jog", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (followerRejectCsrf(request)) return;
    int addr = 0;
    if (!checkAddressParam(request, addr)) return;
    long steps = 0;
    if (!queryRequireLong(request, "steps", steps)) return;
    MaintVerdict verdict = maintValidateJog(steps);
    if (verdict.httpStatus != 200) {
      sendWithCors(request, verdict.httpStatus, "text/plain",
                   verdict.message);
      return;
    }
    stageOp(request, FollowerOpKind::Jog, (uint8_t)addr, steps);
  });

  server.on("/unit/home", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (followerRejectCsrf(request)) return;
    int addr = 0;
    if (!checkAddressParam(request, addr)) return;
    stageOp(request, FollowerOpKind::Home, (uint8_t)addr, 0);
  });

  server.on("/unit/identify", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (followerRejectCsrf(request)) return;
    int addr = 0;
    if (!checkAddressParam(request, addr)) return;
    stageOp(request, FollowerOpKind::Identify, (uint8_t)addr, 0);
  });

  server.on("/unit/reset-odometer", HTTP_POST,
            [](AsyncWebServerRequest* request) {
              if (followerRejectCsrf(request)) return;
              int addr = 0;
              if (!checkAddressParam(request, addr)) return;
              stageOp(request, FollowerOpKind::ResetOdometer, (uint8_t)addr,
                      0);
            });

  // Feature gates (#409): the same flip the S3 rows get, so this row's units
  // are not the ones that need a reflash to enable a motion change.
  server.on("/unit/gates", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (followerRejectCsrf(request)) return;
    int addr = 0;
    if (!checkAddressParam(request, addr)) return;
    long gates = 0;
    if (!queryRequireLong(request, "gates", gates)) return;
    MaintVerdict verdict = maintValidateGates(gates);
    if (verdict.httpStatus != 200) {
      sendWithCors(request, verdict.httpStatus, "text/plain",
                   verdict.message);
      return;
    }
    stageOp(request, FollowerOpKind::SetGates, (uint8_t)addr, gates);
  });

  server.on("/unit/self-test", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (followerRejectCsrf(request)) return;
    int addr = 0;
    if (!checkAddressParam(request, addr)) return;
    stageOp(request, FollowerOpKind::SelfTest, (uint8_t)addr, 0);
  });

  server.on("/unit/self-test-result", HTTP_GET,
            [](AsyncWebServerRequest* request) {
              if (!request->hasParam("seq")) {
                sendWithCors(request, 400, "text/plain",
                             F("Missing 'seq' query param"));
                return;
              }
              long seq = request->getParam("seq")->value().toInt();
              if (seq < 1) {
                sendWithCors(request, 400, "text/plain",
                             F("seq must be >= 1"));
                return;
              }
              char buf[128];
              buildSelfTestJson(buf, sizeof(buf), selfTestSlot,
                                (uint32_t)seq);
              sendWithCors(request, 200, "application/json", buf);
            });

  // v1 debug semantics: range check only, no sketch-state gate. displayTask
  // equivalent rule: never reprobe right after — the loop's probe-inhibit
  // deadline (armed at execution) keeps runtime probes out of the twiboot
  // window (v1 #88).
  server.on("/unit/boot-update", HTTP_POST,
            [](AsyncWebServerRequest* request) {
    if (followerRejectCsrf(request)) return;
    int addr = 0;
    if (!checkAddressParam(request, addr)) return;
    stageOp(request, FollowerOpKind::BootUpdate, (uint8_t)addr, 0);
  });

  // Boot-section dump (#522): reads the unit's twiboot image over I2C. The
  // unit passes through its bootloader and restarts; nothing is written.
  server.on("/unit/boot-dump", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (followerRejectCsrf(request)) return;
    int addr = 0;
    if (!checkAddressParam(request, addr)) return;
    stageOp(request, FollowerOpKind::BootDump, (uint8_t)addr, 0);
  });

  server.on("/unit/boot-dump-result", HTTP_GET,
            [](AsyncWebServerRequest* request) {
    if (!request->hasParam("seq")) {
      sendWithCors(request, 400, "text/plain", F("Missing 'seq' query param"));
      return;
    }
    long seq = request->getParam("seq")->value().toInt();
    if (seq < 1) {
      sendWithCors(request, 400, "text/plain", F("seq must be >= 1"));
      return;
    }
    const BootDumpSlot& slot = bootDumpSlot;
    if (slot.seq != (uint32_t)seq || slot.outcome != BootDumpOutcome::Ok) {
      char small[96];
      buildBootDumpJson(small, sizeof(small), slot, (uint32_t)seq, nullptr);
      sendWithCors(request, 200, "application/json", small);
      return;
    }
    // ~2.2 KB JSON with the hex dump — heap, not stack. The callback
    // response pattern (same as /units/health) avoids a second copy.
    size_t cap = BOOT_DUMP_JSON_CAP;
    if (!heapCanHold(cap + BOOT_SECTION_LEN)) {
      sendWithCors(request, 503, "text/plain", F("out of memory — retry"));
      return;
    }
    std::shared_ptr<char> held(new (std::nothrow) char[cap],
                               std::default_delete<char[]>());
    if (held.get() == nullptr) {
      sendWithCors(request, 503, "text/plain", F("out of memory — retry"));
      return;
    }
    const uint8_t* bytes = nullptr;
    if (bootDumpBytesSeq == (uint32_t)seq) bytes = bootDumpBytes;
    size_t n = buildBootDumpJson(held.get(), cap, slot, (uint32_t)seq, bytes);
    sendResponseWithCors(
        request,
        request->beginResponse(
            "application/json", n,
            [held, n](uint8_t* out, size_t maxLen, size_t index) -> size_t {
              if (index >= n) return 0;
              size_t take = n - index;
              if (take > maxLen) take = maxLen;
              memcpy(out, held.get() + index, take);
              return take;
            }));
  });

  // Read-only boot report (#499): what the unit says about its own boot
  // section. No restart, nothing written.
  server.on("/unit/boot-info", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (followerRejectCsrf(request)) return;
    int addr = 0;
    if (!checkAddressParam(request, addr)) return;
    stageOp(request, FollowerOpKind::BootInfo, (uint8_t)addr, 0);
  });

  server.on("/unit/boot-info-result", HTTP_GET,
            [](AsyncWebServerRequest* request) {
    if (!request->hasParam("seq")) {
      sendWithCors(request, 400, "text/plain", F("Missing 'seq' query param"));
      return;
    }
    long seq = request->getParam("seq")->value().toInt();
    if (seq < 1) {
      sendWithCors(request, 400, "text/plain", F("seq must be >= 1"));
      return;
    }
    char buf[BOOT_INFO_JSON_CAP];
    buildBootInfoJson(buf, sizeof(buf), bootInfoSlot, (uint32_t)seq);
    sendWithCors(request, 200, "application/json", buf);
  });

  server.on("/unit/reboot", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (followerRejectCsrf(request)) return;
    long addr = 0;
    if (!queryRequireLong(request, "address", addr)) return;
    if (addr < 1 || addr > 126) {
      sendWithCors(request, 400, "text/plain", F("Address must be 1..126"));
      return;
    }
    stageOp(request, FollowerOpKind::RebootToBootloader, (uint8_t)addr, 0);
  });

  server.on("/unit/op-result", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("seq")) {
      sendWithCors(request, 400, "text/plain",
                   F("Missing 'seq' query param"));
      return;
    }
    long seq = request->getParam("seq")->value().toInt();
    if (seq < 1) {
      sendWithCors(request, 400, "text/plain", F("seq must be >= 1"));
      return;
    }
    char buf[96];
    buildOpResultJson(buf, sizeof(buf), opResult, (uint32_t)seq);
    sendWithCors(request, 200, "application/json", buf);
  });

  // --- bulk unit reflash (v1 #138 flow: arm, loop() does the work) -----------

  server.on("/reflash-units", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (followerRejectCsrf(request)) return;
    // Optional ?address=N narrows the job to one unit (#513, the S3's #412
    // contract): a {"seq":N} op whose outcome lands in /unit/op-result, with
    // the same progress object in /units/health. Absent = the whole row.
    // An address that does not parse, or that arrives in the form body, is
    // refused rather than ignored: falling through would turn a one-unit
    // request into a whole-row reflash.
    if (request->hasParam("address", true)) {
      sendWithCors(request, 400, "text/plain",
                   F("'address' must be a query parameter"));
      return;
    }
    if (request->hasParam("address")) {
      long addr = 0;
      if (!reflashParseAddress(request->getParam("address")->value().c_str(),
                               addr) ||
          !reflashAddressInRange(addr, SFP_I2C_ADDRESS_BASE, UNITS_AMOUNT)) {
        sendWithCors(request, 400, "text/plain",
                     F("Address must be a decimal unit address within the "
                       "managed range"));
        return;
      }
      // Optional &force=1: reflash the unit even on the bundled rev.
      bool force = false;
      if (request->hasParam("force") &&
          !reflashParseForce(request->getParam("force")->value().c_str(),
                             force)) {
        sendWithCors(request, 400, "text/plain", F("'force' must be 1 or 0"));
        return;
      }
      stageOp(request, FollowerOpKind::ReflashUnit, (uint8_t)addr,
              force ? 1 : 0);
      return;
    }
    if (request->hasParam("force")) {
      // Never a whole-row erase of healthy units.
      sendWithCors(request, 400, "text/plain",
                   F("'force' needs 'address': one unit at a time"));
      return;
    }
    if (rescueActive()) {
      sendWithCors(request, 409, "text/plain",
                   F("Rescue beacon active — reflash disabled until a "
                     "firmware push"));
      return;
    }
    if (opSlotBusy()) {
      sendWithCors(request, 503, "text/plain",
                   F("Unit firmware flash already in progress — try again "
                     "in a moment"));
      return;
    }
    reflashPending = true;
    sendWithCors(request, 200, "text/plain",
                 F("Reflash queued. Units are re-flashed 2 at a time — "
                   "progress in /units/health's reflash object."));
  });
}

// --- loop drain ---------------------------------------------------------------------

bool webOtaUploadFrozen() {
  if (!masterOtaUploadActive) return false;
  if (otaUploadStalled(masterOtaLastChunkMs, millis())) {
    SerialPrintln(F("OTA upload stalled >30 s — resuming normal operation"));
    masterOtaUploadActive = false;
    if (otaTxPowerReduced) {  // an abandoned upload must not freeze the ladder
      followerTxOtaCap(false);
      otaTxPowerReduced = false;
    }
    // Free the session slot too (v1 #191) — the next upload's begin()
    // retry recovers the abandoned Update session instead of a 409 wedge.
    masterOtaOwnerRequest = nullptr;
    return false;
  }
  return true;
}

// Stamps the single result slot the {"seq":N} → GET /unit/op-result contract
// reads, graded by the shared MaintenancePolicy.h rules.
static void stampOpResult(uint32_t seq, MaintGrade grade) {
  opResult.seq = seq;
  opResult.outcome = grade.outcome;
  opResult.reason = grade.reason;
}

static void executeStagedOp() {
  StagedOp op = stagedOp;  // copy, then release the slot at the end
  MaintGrade grade = maintGradeWire(-1);
  switch (op.kind) {
    case FollowerOpKind::WriteOffset:
      grade = maintGradeWire(busWriteOffset(op.addr, (int16_t)op.arg));
      // Patch the probe-time fact in place so GET /unit/offset reflects the
      // write without a reprobe.
      if (grade.outcome == MaintOutcome::Ok) {
        unitFactsApplyOffsetWrite(unitFacts[op.addr - SFP_I2C_ADDRESS_BASE],
                                  (int16_t)op.arg);
      }
      break;
    case FollowerOpKind::Jog:
      grade = maintGradeWire(busJog(op.addr, (int)op.arg));
      break;
    case FollowerOpKind::Home:
      grade = maintGradeWire(busHome(op.addr));
      break;
    case FollowerOpKind::Identify:
      grade = maintGradeWire(busIdentify(op.addr));
      break;
    case FollowerOpKind::ResetOdometer:
      grade = maintGradeWire(busResetOdometer(op.addr));
      if (grade.outcome == MaintOutcome::Ok) {
        unitFactsApplyOdometerReset(unitFacts[op.addr - SFP_I2C_ADDRESS_BASE]);
      }
      break;
    case FollowerOpKind::SetGates:
      // busSetGates verifies with a read-back, so a unit that refused the
      // bits grades as a failure here rather than a phantom success.
      grade = maintGradeGates(busSetGates(op.addr, (uint8_t)op.arg));
      if (grade.outcome == MaintOutcome::Ok) {
        unitFactsApplyGatesWrite(unitFacts[op.addr - SFP_I2C_ADDRESS_BASE],
                                 (uint8_t)op.arg);
      }
      break;
    case FollowerOpKind::ReflashUnit:
      releaseBootDumpBytes();  // the flash is this board's heap low-water mark
      // Blocks loop() for the length of one unit's flash, like the bulk job;
      // the op slot stays claimed, so every other unit op answers 503.
      busRunReflashJob(op.addr, op.arg != 0);
      grade = busLastReflashGrade();
      break;
    case FollowerOpKind::RebootToBootloader:
      grade = maintGradeWire(busRebootToBootloader(op.addr));
      if (grade.outcome == MaintOutcome::Ok) busInvalidateUnitReads(op.addr);
      // The unit sits in twiboot for ~1 s — keep every runtime probe out
      // of that window (v1 #88). Armed on a NACK too: it does not prove the
      // unit stayed in its sketch.
      busArmProbeInhibit(millis() + UNIT_PROBE_INHIBIT_MS);
      // Only a probe re-reads the offset; queue one for once the inhibit
      // has run out, or the unit's reads stay invalid until someone asks.
      unitHealthRefreshPending = true;
      break;
    case FollowerOpKind::SelfTest:
      selfTestSlot = SelfTestSlot{};
      selfTestSlot.seq = op.seq;
      selfTestSlot.addr = op.addr;
      if (busStartSelfTest(op.addr) == 0) {
        // The op result follows the test: it is stamped by pollSelfTest once
        // the unit reports, and reads pending until then.
        selfTestPolling = true;
        selfTestPollLastMs = millis();
        selfTestPollBegin(selfTestPoll, selfTestPollLastMs);
        stagedOp.pending = false;
        return;
      }
      selfTestSlot.outcome = SelfTestOutcome::WireFail;
      grade = maintGradeObserved(false);
      break;
    case FollowerOpKind::BootUpdate:
      busRunBootUpdate(op.seq, op.addr, opResult);
      unitHealthRefreshPending = true;  // its reads were invalidated
      stagedOp.pending = false;
      return;
    case FollowerOpKind::BootDump:
      busRunBootDump(op.seq, op.addr, bootDumpSlot, bootDumpBytes);
      bootDumpBytesSeq = (bootDumpSlot.outcome == BootDumpOutcome::Ok)
                             ? op.seq : 0;
      bootDumpBytesAtMs = millis();
      grade = maintGradeObserved(bootDumpSlot.outcome == BootDumpOutcome::Ok);
      unitHealthRefreshPending = true;  // its reads were invalidated
      break;
    case FollowerOpKind::Probe:
      // Runs with the health refresh below, once any twiboot window is over.
      unitHealthRefreshPending = true;
      probeOpSeq = op.seq;
      stagedOp.pending = false;
      return;
    case FollowerOpKind::BootInfo: {
      BootInfoSlot slot;
      slot.seq = op.seq;
      slot.addr = op.addr;
      slot.ok = busReadBootInfo(op.addr, slot.report);
      slot.done = true;
      bootInfoSlot = slot;
      grade = maintGradeObserved(slot.ok, MaintReason::BootInfoReadFail);
      break;
    }
    default:
      break;
  }
  stampOpResult(op.seq, grade);
  stagedOp.pending = false;
}

// One poll per SELF_TEST_POLL_MS; SelfTestPoll.h decides what the replies
// mean (stale terminal, unsupported firmware, timeout).
static void pollSelfTest() {
  if (!selfTestPolling) return;
  if (millis() - selfTestPollLastMs < SELF_TEST_POLL_MS) return;
  selfTestPollLastMs = millis();
  UnitSelfTestReading reading;
  bool readOk = busReadSelfTest(selfTestSlot.addr, reading);
  SelfTestOutcome outcome = selfTestPollObserve(
      selfTestPoll, readOk, reading, selfTestPollLastMs, selfTestSlot);
  if (outcome == SelfTestOutcome::Pending) return;
  selfTestSlot.outcome = outcome;
  selfTestPolling = false;
  stampOpResult(selfTestSlot.seq,
                maintGradeObserved(outcome == SelfTestOutcome::Ok));
}

void webLoopTick() {
  if (reflashPending) {
    reflashPending = false;
    releaseBootDumpBytes();  // the flash is this board's heap low-water mark
    busRunReflashJob();
  }
  if (stagedOp.pending) executeStagedOp();
  if (bootDumpBytes != nullptr && !stagedOp.pending &&
      (uint32_t)(millis() - bootDumpBytesAtMs) >= BOOT_DUMP_KEEP_MS) {
    releaseBootDumpBytes();
  }
  pollSelfTest();
  if (unitHealthRefreshPending) {
    // Probe-inhibit (v1 #88): wait out any twiboot window before scanning.
    if ((int32_t)(millis() - busProbeInhibitedUntilMs()) >= 0) {
      unitHealthRefreshPending = false;
      busProbe();
      busPollHealth();
      if (probeOpSeq != 0) {
        stampOpResult(probeOpSeq, maintGradeWire(0));
        probeOpSeq = 0;
      }
    }
  }
}
