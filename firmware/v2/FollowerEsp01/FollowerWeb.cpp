// FollowerWeb.cpp — endpoint glue (#298). Contract + context rules in
// FollowerWeb.h. The /cluster/* handlers mirror the v2 master's follower
// endpoints (WebEndpoints.cpp) minus digest/promote; /firmware/master is
// v1's ESP8266 Update flow trimmed (no RTC verdict cookie — ota-flash.sh's
// version comparison is the revert detector on this board).

#include "FollowerWeb.h"

#include <ESP8266WiFi.h>
#include <Updater.h>

#include "BuildVersion.h"
#include "ApiIndex.h"
#include "FollowerBus.h"
#include "ClusterHmac.h"  // #313 follow-on: rebuild canonical msgs for verify
#include "FollowerCluster.h"
#include "FollowerConfig.h"
#include "FollowerCors.h"
#include "ClusterForeign.h"
#include "FollowerJson.h"
#include "FollowerRescue.h"  // #343: beacon marker + op lockout
#include "FollowerSettings.h"
#include "FollowerWifi.h"
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
static uint32_t maintSeqCounter = 0;

// Self-test poll state (the unit measures ~2 revolutions; we poll its
// GET_SELF_TEST until it stops reporting "running").
static bool selfTestPolling = false;
static uint8_t selfTestAddr = 0;
static uint32_t selfTestPollDeadlineMs = 0;
static uint32_t selfTestPollLastMs = 0;
#define SELF_TEST_TIMEOUT_MS 20000UL

// --- OTA session state (v1 #191 conventions) ----------------------------------------

static AsyncWebServerRequest* volatile masterOtaOwnerRequest = nullptr;

// #358: refused foreign-leader contacts, surfaced in /cluster/health.
// Handler-context only (the ESP-01's async handlers and loop() cooperate on
// one core), RAM-only, resets on reboot.
static ForeignContactStats foreignContacts;
static bool otaRejected = false;
static int otaRejectionStatus = 0;
static String otaRejectionReason;
static bool otaTxPowerReduced = false;
static constexpr float OTA_TX_POWER_DBM = 10.0f;
static constexpr float DEFAULT_TX_POWER_DBM = 20.5f;

// --- helpers ------------------------------------------------------------------------

// #294 rung 3 CORS: per-response reflection (the ESP8266 async fork has no
// middleware). Simple requests only — no preflight handler needed.
static void sendWithCors(AsyncWebServerRequest* request, int status,
                         const String& contentType, const String& body) {
  AsyncWebServerResponse* response =
      request->beginResponse(status, contentType, body);
  if (request->hasHeader("Origin") &&
      followerCorsPathAllowed(request->url())) {
    const String origin = request->header("Origin");
    if (followerCorsOriginAllowed(origin)) {
      response->addHeader("Access-Control-Allow-Origin", origin);
      response->addHeader("Vary", "Origin");
    }
  }
  request->send(response);
}

// #313 CSRF gate for the middleware-less ESP8266 fork: call at the top of
// every mutating handler. Returns true (and answers 403) when the request is
// a forged cross-site POST — a browser POST whose Origin is not a LAN pane.
// The leader's server-to-server calls send no Origin and pass; the wall's
// own LAN UI sends a LAN origin and passes.
static bool followerRejectCsrf(AsyncWebServerRequest* request) {
  bool hasOrigin = request->hasHeader("Origin");
  String origin = hasOrigin ? request->header("Origin") : String();
  if (followerCsrfRejectPost(request->method() == HTTP_POST, hasOrigin,
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

// Health facts snapshot for the join/ping replies (#294 keys).
static FollowerHealthFacts healthNow(char* maskBuf, size_t maskCap) {
  FollowerHealthFacts h;
  h.width = displayWidth;
  int detected = 0;
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    if (unitFacts[i].state != 0) detected++;
  }
  h.detected = detected;
  h.faulty = computeFaultyUnitCount(unitFacts, UNITS_AMOUNT);
  followerFaultMaskHex(unitFacts, displayWidth, maskBuf, maskCap);
  h.faultMask = maskBuf;
  WearAssessment wear;
  assessWear(unitFacts, UNITS_AMOUNT, wear);
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

// v1's printable-ASCII gate for wire strings that get re-served.
static bool isPrintableAscii(const String& s, char minChar) {
  for (unsigned int i = 0; i < s.length(); i++) {
    if ((unsigned char)s[i] < (unsigned char)minChar ||
        (unsigned char)s[i] > 0x7E) {
      return false;
    }
  }
  return true;
}

// Busy gate for the {"seq":N} ops: one staged slot, and the reflash job /
// a waiting render own the bus first (mutual 409/503 discipline).
static bool opSlotBusy() {
  return stagedOp.pending || selfTestPolling || reflashPending ||
         reflashInProgress(reflashProgress);
}

static void stageOp(AsyncWebServerRequest* request, FollowerOpKind kind,
                    uint8_t addr, long arg) {
  if (rescueActive()) {
    // #343: the beacon never touches the bus — flash new firmware first.
    sendWithCors(request, 409, "text/plain",
                 F("Rescue beacon active — unit ops disabled until a "
                   "firmware push"));
    return;
  }
  if (opSlotBusy()) {
    sendWithCors(request, 503, "text/plain",
                 F("Another unit operation is in progress — try again"));
    return;
  }
  stagedOp.seq = ++maintSeqCounter;
  stagedOp.kind = kind;
  stagedOp.addr = addr;
  stagedOp.arg = arg;
  stagedOp.pending = true;  // set last (v1 flag-handoff rule)
  char buf[24];
  snprintf(buf, sizeof(buf), "{\"seq\":%lu}", (unsigned long)stagedOp.seq);
  sendWithCors(request, 200, "application/json", buf);
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
        WiFi.setOutputPower(DEFAULT_TX_POWER_DBM);
        otaTxPowerReduced = false;
      }
      if (otaRejected) {
        int status = otaRejectionStatus;
        String reason = otaRejectionReason;
        otaRejected = false;
        otaRejectionStatus = 0;
        otaRejectionReason = String();
        masterOtaUploadActive = false;
        request->send(status, "text/plain", reason);
        return;
      }
      if (Update.hasError()) {
        String msg = String("Master OTA failed: ") + Update.getErrorString();
        masterOtaUploadActive = false;
        request->send(500, "text/plain", msg);
      } else if (!uploadRan) {
        // #347: no file part streamed — nothing was flashed; never report
        // success (which reboots).
        masterOtaUploadActive = false;
        request->send(400, "text/plain",
                      "No firmware in request (a multipart file part is "
                      "required)");
      } else if (!Update.isFinished()) {
        masterOtaUploadActive = false;
        request->send(500, "text/plain",
                      "Master OTA incomplete: final chunk missing");
      } else {
        request->send(200, "text/plain",
                      "Master firmware flashed; rebooting…");
        isPendingReboot = true;
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
        // CSRF gate (#313): reject a forged cross-site upload BEFORE any
        // flash write or freeze — an attacker can match ?md5= to their own
        // bytes (MD5 is integrity, not authenticity), so a browser CSRF could
        // flash hostile firmware. Marked like every other reject so the
        // completion handler answers 403; no owner/freeze is taken.
        if (followerCsrfRejectPost(true, request->hasHeader("Origin"),
                                   request->hasHeader("Origin")
                                       ? request->header("Origin")
                                       : String())) {
          otaRejected = true;
          otaRejectionStatus = 403;
          otaRejectionReason = F("Cross-origin OTA refused (CSRF guard)");
          return;
        }
        // Freeze all display/unit work for the upload (v1 #116): WiFi RX +
        // flash writes + stepper current on one small supply is the storm
        // that endangers a flash.
        masterOtaUploadActive = true;
        otaRejected = false;
        otaRejectionStatus = 0;
        otaRejectionReason = String();

        WiFi.setOutputPower(OTA_TX_POWER_DBM);  // v1 #60 sag guard
        otaTxPowerReduced = true;

        uint32_t freeSpace = ESP.getFreeSketchSpace();
        if (freeSpace < 0x1000) {
          // #354: (freeSpace - 0x1000) below would underflow to a huge
          // maxSketchSpace and defeat the contentLen pre-check.
          otaRejected = true;
          otaRejectionStatus = 507;
          otaRejectionReason = String("No sketch space free: ") + freeSpace;
          return;
        }
        uint32_t maxSketchSpace = (freeSpace - 0x1000) & 0xFFFFF000;
        size_t contentLen = request->contentLength();
        if (contentLen > 0 && contentLen > maxSketchSpace) {
          otaRejected = true;
          otaRejectionStatus = 413;
          otaRejectionReason = String("Firmware too large: ") + contentLen +
                               " bytes > maxSketchSpace " + maxSketchSpace;
          return;
        }
        if (ESP.getFlashChipRealSize() < ESP.getFlashChipSize()) {
          // v1 #92/#94: Update.begin() would refuse everything.
          otaRejected = true;
          otaRejectionStatus = 412;
          otaRejectionReason =
              "Flash config mismatch — reflash once over USB";
          return;
        }
        // MD5 is MANDATORY (v1 #144): eboot's checksum does not catch a
        // truncated upload. Validated BEFORE Update.begin (#354): begin
        // erases the flash region, so a malformed no-md5 POST used to cost
        // a full erase cycle (display frozen, flash wear) per request.
        if (!request->hasParam("md5")) {
          otaRejected = true;
          otaRejectionStatus = 400;
          otaRejectionReason = "md5 query param is required";
          return;
        }
        String md5 = request->getParam("md5")->value();
        md5.toLowerCase();
        if (md5.length() != 32) {
          otaRejected = true;
          otaRejectionStatus = 400;
          otaRejectionReason = "md5 query param must be a 32-char hex digest";
          return;
        }
        Update.runAsync(true);
        if (!Update.begin(maxSketchSpace, U_FLASH)) {
          // Stale updater state from an aborted upload (v1 #162).
          Update.end(false);
          Update.clearError();
          if (!Update.begin(maxSketchSpace, U_FLASH)) {
            otaRejected = true;
            otaRejectionStatus = 500;
            otaRejectionReason =
                String("Update.begin failed: ") + Update.getErrorString();
            return;
          }
        }
        if (!Update.setMD5(md5.c_str())) {
          otaRejected = true;
          otaRejectionStatus = 400;
          otaRejectionReason = "Update.setMD5 rejected '" + md5 + "'";
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
      if (otaRejected) return;
      if (!Update.hasError() && len > 0) {
        Update.write(data, len);
      }
      if (final) {
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
  // operator (#308). Static buffer (BSS) — no per-request heap churn on the
  // ESP-01, like /units/health.
  server.on("/api", HTTP_GET, [](AsyncWebServerRequest* request) {
    static char buf[API_JSON_CAP];
    size_t n = buildApiJson(buf, API_JSON_CAP);
    if (n == 0 || n >= API_JSON_CAP) {
      sendWithCors(request, 500, "text/plain", F("api index unavailable"));
      return;
    }
    sendWithCors(request, 200, "application/json", buf);
  });

  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest* request) {
    FollowerClusterView cv = clusterViewGet();
    sendWithCors(request, 200, "application/json",
                 followerSettingsJson(effectiveDeviceName, GIT_REV,
                                      displayWidth,
                                      followerPhaseName(cv.phase),
                                      cv.leaderName, cv.leaderHost, cv.row,
                                      vitalsNow()));
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
    String body;
    body.reserve(FOLLOWER_LOG_SIZE + 16);
    uint32_t next = followerLogReadSince(after, body);
    String out;
    out.reserve(body.length() + 12);
    out += next;
    out += '\n';
    out += body;
    sendWithCors(request, 200, "text/plain", out);
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
    if (row < 0 || row > 255) {
      request->send(400, "text/plain", F("Row out of range"));
      return;
    }
    if (leaderHost.length() == 0 || leaderHost.length() > FOLLOWER_HOST_MAX ||
        !isPrintableAscii(leaderHost, 0x21)) {
      request->send(400, "text/plain", F("Invalid leaderHost"));
      return;
    }
    if (leaderName.length() > FOLLOWER_NAME_MAX ||
        !isPrintableAscii(leaderName, 0x20)) {
      request->send(400, "text/plain", F("Invalid leaderName"));
      return;
    }
    // Source-IP binding (#313): the caller must actually BE leaderHost — the
    // real leader dials from WiFi.localIP(), the exact value it sends here.
    // Blocks any-LAN-host / CSRF membership hijack.
    if (leaderHost != request->client()->remoteIP().toString()) {
      request->send(403, "text/plain",
                    F("leaderHost must match the caller's address"));
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
      followerAppendJsonString(out, curHost);
      out += ",\"leaderName\":";
      followerAppendJsonString(out, curName);
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
    if (tz.length() > FOLLOWER_TZ_MAX || !isPrintableAscii(tz, 0x21)) {
      tz = "";
    }
    clusterHandleJoin(leaderName, leaderHost, (int)row, epoch, key, tz);
    char mask[16];
    FollowerHealthFacts h = healthNow(mask, sizeof(mask));
    request->send(200, "application/json",
                  followerJoinReplyJson(effectiveDeviceName, GIT_REV, h,
                                        vitalsNow(), rescueActive()));
  });

  server.on("/cluster/render", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (followerRejectCsrf(request)) return;
    // Source-IP binding (#313): only the joined leader drives this row. Never
    // called by a browser UI (leader wire only), so a strict bind is safe.
    FollowerClusterView rcv = clusterViewGet();
    if (rcv.leaderHost.length() > 0 &&
        rcv.leaderHost != request->client()->remoteIP().toString()) {
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
    FollowerRenderVerdict v =
        clusterHandleRender(epoch, seq, text, speed, commitAtMs);
    if (v == FollowerRenderVerdict::NotClustered) {
      request->send(409, "application/json",
                    F("{\"error\":\"not clustered\"}"));
      return;
    }
    String out = "{\"applied\":";
    out += (v == FollowerRenderVerdict::Apply) ? "true" : "false";
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
    if (pcv.leaderHost.length() > 0 &&
        pcv.leaderHost != request->client()->remoteIP().toString()) {
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
    if (clusterHmacEnforced()) {
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
    FollowerClusterView cv = clusterViewGet();
    char mask[16];
    FollowerHealthFacts h = healthNow(mask, sizeof(mask));
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
                          followerCorsOriginAllowed(request->header("Origin"));
    if (clusterHmacEnforced()) {
      // Keyed (#313 follow-on): the leader arm becomes a valid SIGNATURE
      // (beats a spoofed IP); the local Leave button rides the browser arm.
      bool signedOk = false;
      String tsStr, mac;
      if (paramString(request, "ts", tsStr) && paramString(request, "mac", mac)) {
        uint64_t ts = strtoull(tsStr.c_str(), nullptr, 10);
        signedOk = clusterVerifySigned(clusterHmacLeaveMsg(ts), ts, mac);
      }
      if (!signedOk && !fromLanBrowser) {
        request->send(403, "text/plain",
                      F("leave requires a valid signature or this row's web UI"));
        return;
      }
    } else {
      // Pre-HMAC combined guard (#313): leader IP OR LAN browser; a bare
      // non-leader LAN host is refused (closes the any-host force-leave DoS).
      bool fromLeader =
          lcv.leaderHost.length() > 0 &&
          lcv.leaderHost == request->client()->remoteIP().toString();
      if (lcv.leaderHost.length() > 0 && !fromLeader && !fromLanBrowser) {
        request->send(403, "text/plain",
                      F("leave must come from the leader or this row's web UI"));
        return;
      }
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
    diag.sntpSynced = cv.sntpSynced;
    diag.hmac = clusterHmacEnforced();
    diag.foreign = foreignContacts;  // #358
    diag.nowMs = millis();
    request->send(200, "application/json",
                  followerClusterHealthJson(
                      followerPhaseName(cv.phase), cv.leaderName,
                      cv.leaderHost, cv.row, cv.epoch, cv.lastSeq,
                      cv.heldSegment, GIT_REV, displayWidth, detected,
                      computeFaultyUnitCount(unitFacts, UNITS_AMOUNT), diag));
  });

  // --- unit health (v1/v2 shared wire shape) --------------------------------

  server.on("/units/health", HTTP_GET, [](AsyncWebServerRequest* request) {
    // Static, not per-request heap (v1's ESP-01 RAM tactic — the S3 version
    // heap-allocates, but ~3.5 KB of new/delete churn per poll fragments
    // this board's ~40 KB heap). Safe unlocked: handlers run one at a time
    // in the single LWIP context.
    // Sized off a follower-LOCAL cap, deliberately NOT the shared
    // UNIT_HEALTH_JSON_CAP: the master raised that to 6144 for the #367
    // err/errAge keys, which stay inert here (per-unit I2C attribution is
    // master-only — i2cErrors/lastErrorMs are never set by FollowerBus.cpp),
    // so those keys never widen a follower payload. #365 ext-diag is the
    // opposite case: FollowerBus.cpp DOES populate extDiagValid on this bus,
    // so the se/sx/sag/he/dw/sb keys are live here too — raised from 5120 to
    // 6144 (measured worst case incl. the wear + reflash splices: ~5662 B;
    // see test_health_json_follower_worst_case_fits_local_buf).
    static constexpr size_t FOLLOWER_HEALTH_BUF = 6144;
    static char buf[FOLLOWER_HEALTH_BUF];
    int faulty = computeFaultyUnitCount(unitFacts, UNITS_AMOUNT);
    size_t n = buildUnitHealthJson(buf, FOLLOWER_HEALTH_BUF, unitFacts,
                                   displayWidth, faulty,
                                   SFP_I2C_ADDRESS_BASE, millis());
    if (n == 0 || n >= FOLLOWER_HEALTH_BUF) {
      n = (size_t)snprintf(buf, FOLLOWER_HEALTH_BUF,
                           "{\"width\":%d,\"faulty\":%d,\"units\":[]}",
                           displayWidth, faulty);
    }
    // Wear + reflash progress splices (v2 additive keys — same payload the
    // S3 member panel reads).
    WearAssessment wear;
    assessWear(unitFacts, UNITS_AMOUNT, wear);
    char wearJson[96];
    size_t wearLen = buildWearJson(wear, wearJson, sizeof(wearJson));
    if (n > 0 && wearLen < sizeof(wearJson) &&
        n + wearLen + 2 < FOLLOWER_HEALTH_BUF) {
      n += (size_t)snprintf(buf + n - 1, FOLLOWER_HEALTH_BUF - n + 1,
                            ",%s}", wearJson) - 1;
    }
    char reflashJson[80];
    buildReflashJson(reflashJson, sizeof(reflashJson), reflashProgress);
    if (n > 0 && n + strlen(reflashJson) + 13 < FOLLOWER_HEALTH_BUF) {
      snprintf(buf + n - 1, FOLLOWER_HEALTH_BUF - n + 1,
               ",\"reflash\":%s}", reflashJson);
    }
    sendWithCors(request, 200, "application/json", buf);
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
  if (millis() - masterOtaLastChunkMs > 30000UL) {
    SerialPrintln(F("OTA upload stalled >30 s — resuming normal operation"));
    masterOtaUploadActive = false;
    // Free the session slot too (v1 #191) — the next upload's begin()
    // retry recovers the abandoned Update session instead of a 409 wedge.
    masterOtaOwnerRequest = nullptr;
    return false;
  }
  return true;
}

static void executeStagedOp() {
  StagedOp op = stagedOp;  // copy, then release the slot at the end
  int wireStatus = -1;
  switch (op.kind) {
    case FollowerOpKind::WriteOffset:
      wireStatus = busWriteOffset(op.addr, (int16_t)op.arg);
      if (wireStatus == 0) {
        // Patch the probe-time fact in place (v2 rule) so GET /unit/offset
        // reflects the write without a reprobe.
        UnitFacts& u = unitFacts[op.addr - SFP_I2C_ADDRESS_BASE];
        u.offset = (int16_t)op.arg;
        u.offsetValid = true;
      }
      break;
    case FollowerOpKind::Jog:
      wireStatus = busJog(op.addr, (int)op.arg);
      break;
    case FollowerOpKind::Home:
      wireStatus = busHome(op.addr);
      break;
    case FollowerOpKind::Identify:
      wireStatus = busIdentify(op.addr);
      break;
    case FollowerOpKind::ResetOdometer:
      wireStatus = busResetOdometer(op.addr);
      if (wireStatus == 0) {
        UnitFacts& u = unitFacts[op.addr - SFP_I2C_ADDRESS_BASE];
        u.odometer = 0;
      }
      break;
    case FollowerOpKind::RebootToBootloader:
      wireStatus = busRebootToBootloader(op.addr);
      // The unit sits in twiboot for ~1 s — keep every runtime probe out
      // of that window (v1 #88).
      busArmProbeInhibit(millis() + 3000);
      break;
    case FollowerOpKind::SelfTest:
      wireStatus = busStartSelfTest(op.addr);
      selfTestSlot = SelfTestSlot{};
      selfTestSlot.seq = op.seq;
      if (wireStatus == 0) {
        selfTestPolling = true;
        selfTestAddr = op.addr;
        selfTestPollDeadlineMs = millis() + SELF_TEST_TIMEOUT_MS;
        selfTestPollLastMs = millis();
      } else {
        selfTestSlot.outcome = SelfTestOutcome::WireFail;
      }
      break;
    default:
      break;
  }
  // The op-result contract answers for every kind; for a self-test, "ok"
  // means "started" — the measurements land in /unit/self-test-result.
  opResult.seq = op.seq;
  opResult.outcome =
      wireStatus == 0 ? MaintOutcome::Ok : MaintOutcome::WireFail;
  opResult.reason = MaintReason::None;
  stagedOp.pending = false;
}

static void pollSelfTest() {
  if (!selfTestPolling) return;
  if (millis() - selfTestPollLastMs < 500) return;
  selfTestPollLastMs = millis();
  UnitSelfTestReading reading;
  if (busReadSelfTest(selfTestAddr, reading)) {
    if (reading.state == 2 /* ok */) {
      selfTestSlot.outcome = SelfTestOutcome::Ok;
      selfTestSlot.stepsPerRev = reading.stepsPerRev;
      selfTestSlot.hallWindowSteps = reading.hallWindowSteps;
      selfTestSlot.revTimeMs = reading.revTimeMs;
      selfTestPolling = false;
      return;
    }
    if (reading.state == 3 /* failed */) {
      selfTestSlot.outcome = SelfTestOutcome::UnitFailed;
      selfTestPolling = false;
      return;
    }
    // state 0 (never) right after a start means the unit dropped the
    // command — old firmware; keep polling until the deadline settles it.
  }
  if ((int32_t)(millis() - selfTestPollDeadlineMs) >= 0) {
    selfTestSlot.outcome = SelfTestOutcome::Timeout;
    selfTestPolling = false;
  }
}

void webLoopTick() {
  if (reflashPending) {
    reflashPending = false;
    busRunReflashJob();
  }
  if (stagedOp.pending) executeStagedOp();
  pollSelfTest();
  if (unitHealthRefreshPending) {
    // Probe-inhibit (v1 #88): wait out any twiboot window before scanning.
    if ((int32_t)(millis() - busProbeInhibitedUntilMs()) >= 0) {
      unitHealthRefreshPending = false;
      busProbe();
      busPollHealth();
    }
  }
}
