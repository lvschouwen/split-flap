// Cluster wire + management endpoints (epic #270) and the #294/#313
// CORS+CSRF middleware — split from WebEndpoints.cpp (#338); async-context
// rules in WebEndpoints.cpp's header. Follower-wire handlers stage into
// ClusterFollower under its own mutex; leader ops stage into ClusterLeader;
// the follower-image upload accumulates in PSRAM here and netTask commits it
// to flash (single-writer rule).

#include "WebEndpoints.h"
#include "WebEndpointsInternal.h"

#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>  // localIP in the leading-board join refusal (#295)

#include "BuildVersion.h"
#include "ClusterDigest.h"
#include "ClusterDiscovery.h"
#include "ClusterFollower.h"
#include "ClusterHmac.h"  // #313 follow-on: rebuild canonical msgs for verify
#include "ClusterLayout.h"  // CLUSTER_MAX_MEMBERS / CLUSTER_HOST_MAX_LEN
#include "ClusterLeader.h"
#include "FollowerImagePolicy.h"  // #304 follower-image upload guard
#include "FollowerImageStore.h"
#include "HelpersSerialHandling.h"
#include "MdnsDiscovery.h"  // normalizeMdnsHostname
#include "OtaService.h"  // normalizeOtaMd5
#include "SettingsJson.h"  // appendJsonString, settingsIsPrintableAscii
#include "Tasks.h"
#include "WearPolicy.h"

// Cluster board discovery staging (#274): same POST-arm / netTask-drain /
// GET-poll contract as the /mqtt/discover pair (WebSettings.cpp), browsing
// _splitflap._tcp.
static bool clusterDiscoverPending = false;
static String clusterDiscoverResultJson;

// Same rejection pattern as the master OTA/rescue uploads (WebFirmware.cpp)
// for POST /cluster/follower-firmware (#304) — the stored ESP-01 image
// upload. Independent of those flows.
static int followerFwRejectStatus = 0;
static String followerFwRejectReason;

// #294 rung 3 + #313 CSRF gate. Two jobs, one attach point (both gates are
// pure ClusterDigest.h logic; simple requests only — form posts / GETs, no
// preflight handler needed):
//   1. ENFORCE (#313): a mutating POST carrying a browser Origin that is not
//      a LAN pane is cross-site forgery — 403 BEFORE the handler runs, so
//      the whole class of mutating routes (/firmware/master, /reset-wifi,
//      /cluster/*, …) is covered without a per-route allowlist to drift.
//      The leader's esp_http_client sends no Origin and passes; the board's
//      own LAN UI sends a LAN origin and passes.
//   2. REFLECT (#294): on the per-member management surface, echo the LAN
//      origin back so another pane's browser can read the response.
static AsyncMiddlewareFunction clusterCorsMiddleware(
    [](AsyncWebServerRequest* request, ArMiddlewareNext next) {
      bool hasOrigin = request->hasHeader("Origin");
      String origin = hasOrigin ? request->header("Origin") : String();
      if (clusterCsrfRejectPost(request->method() == HTTP_POST, hasOrigin,
                                origin)) {
        request->send(403, "text/plain",
                      F("Cross-origin POST refused (CSRF guard)"));
        return;  // handler chain stops — next() is never called
      }
      next();
      if (!hasOrigin) return;
      if (!clusterCorsPathAllowed(request->url())) return;
      if (!clusterCorsOriginAllowed(origin)) return;
      AsyncWebServerResponse* response = request->getResponse();
      if (response == nullptr) return;
      response->addHeader("Access-Control-Allow-Origin", origin);
      response->addHeader("Vary", "Origin");
    });

AsyncMiddlewareFunction& webClusterCorsMiddleware() {
  return clusterCorsMiddleware;
}

void webClusterRegister(AsyncWebServer& server) {
  // --- Cluster follower endpoints (#272, epic #270) --------------------------
  // The LAN wire protocol the leader drives (form-encoded requests, JSON
  // replies — same conventions as the rest of this API). Handlers stage
  // into ClusterFollower under its own mutex; NVS writes and the display
  // enqueue run in netTask's clusterFollowerServiceTick().
  server.on("/cluster/join", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("leaderHost", true) ||
        !request->hasParam("row", true) || !request->hasParam("epoch", true)) {
      request->send(400, "text/plain", F("Missing leaderHost/row/epoch"));
      return;
    }
    ClusterJoinRequest req;
    req.leaderHost = request->getParam("leaderHost", true)->value();
    req.leaderName = request->hasParam("leaderName", true)
                         ? request->getParam("leaderName", true)->value()
                         : req.leaderHost;
    // #313 follow-on: the leader mints a per-member wire-auth key and sends it
    // here (absent from a pre-HMAC leader → enforcement stays off).
    req.key = request->hasParam("key", true)
                  ? request->getParam("key", true)->value()
                  : String();
    long row = request->getParam("row", true)->value().toInt();
    req.epoch = (uint32_t)strtoul(
        request->getParam("epoch", true)->value().c_str(), nullptr, 10);
    if (row < 0 || row >= CLUSTER_MAX_MEMBERS) {
      request->send(400, "text/plain", F("Row out of range"));
      return;
    }
    req.row = (int)row;
    if (req.leaderHost.length() == 0 ||
        req.leaderHost.length() > CLUSTER_HOST_MAX_LEN ||
        !settingsIsPrintableAscii(req.leaderHost, 0x21)) {
      request->send(400, "text/plain", F("Invalid leaderHost"));
      return;
    }
    if (req.leaderName.length() > CLUSTER_HOST_MAX_LEN ||
        !settingsIsPrintableAscii(req.leaderName, 0x20)) {
      request->send(400, "text/plain", F("Invalid leaderName"));
      return;
    }
    // Source-IP binding (#313): a join mints a membership pointing display
    // and firmware traffic at leaderHost, so the caller must actually BE
    // leaderHost — the real leader dials from WiFi.localIP(), which is the
    // exact value it puts in this field. A CSRF'd browser or any other LAN
    // host cannot satisfy this, so it cannot hijack the row/membership.
    if (req.leaderHost != request->client()->remoteIP().toString()) {
      request->send(403, "text/plain",
                    F("leaderHost must match the caller's address"));
      return;
    }
    // #295 sticky leadership: a board that LEADS a wall never becomes a
    // row of someone else's — after a promote, the returning old leader's
    // joins collect this marker (from us and from every claimed member)
    // and it demotes instead of building a second cluster.
    if (clusterLeaderEnabled()) {
      String out = "{\"error\":\"other-leader\",\"leaderHost\":";
      appendJsonString(out, WiFi.localIP().toString());
      out += ",\"leaderName\":";
      appendJsonString(out, effectiveName);
      out += '}';
      request->send(409, "application/json", out);
      return;
    }
    // While our current leader is demonstrably alive, a DIFFERENT
    // leader's join is refused with its identity — the rejected board
    // demotes itself on this marker.
    if (clusterFollowerJoinWouldConflict(req.leaderHost)) {
      ClusterFollowerView cv = clusterFollowerViewGet();
      String out = "{\"error\":\"other-leader\",\"leaderHost\":";
      appendJsonString(out, cv.leaderHost);
      out += ",\"leaderName\":";
      appendJsonString(out, cv.leaderName);
      out += '}';
      request->send(409, "application/json", out);
      return;
    }
    clusterFollowerHandleJoin(req);
    // Handshake reply: identity, firmware rev, width. Width is the boot
    // probe's result today; #234 refines it — no protocol change. The
    // #294 health keys ride along (minus width/rev, already present) so
    // the leader's strip is live from the handshake, not the first ping.
    DisplaySnapshot snap = displaySnapshotGet();
    char mask[16];
    clusterFaultMaskHex(snap.units, snap.displayWidth, mask, sizeof(mask));
    WearAssessment wear;
    assessWear(snap.units, snap.displayWidth, wear);
    // #332 additive: our deviceRole feeds the leader's succession tiers
    // (backup > rendering > spare > monitor). Absent = pre-#332 peer.
    String selfRole;
    {
      WebStateLock lock;
      if (liveSettings) selfRole = liveSettings->deviceRole;
    }
    String out = "{\"name\":";
    appendJsonString(out, effectiveName);
    out += ",\"role\":";
    appendJsonString(out, selfRole);
    out += ",\"rev\":\"" GIT_REV "\",\"width\":";
    out += (int)snap.displayWidth;
    out += ",\"detected\":";
    out += (int)snap.detectedUnitCount;
    out += ",\"faulty\":";
    out += (int)snap.faultyUnitCount;
    out += ",\"faultMask\":\"";
    out += mask;
    out += "\",\"wear\":";
    out += wear.flaggedCount > 0 ? "true" : "false";
    out += ",\"protocol\":1}";
    request->send(200, "application/json", out);
  });

  server.on("/cluster/render", HTTP_POST, [](AsyncWebServerRequest* request) {
    // Source-IP binding (#313): only the joined leader drives this row's
    // screen. This endpoint is never called by any browser UI (leader wire
    // only), so a strict bind is safe. Standalone (leaderHost "") falls
    // through to the handler's NotClustered 409.
    ClusterFollowerView cv = clusterFollowerViewGet();
    if (cv.leaderHost.length() > 0 &&
        cv.leaderHost != request->client()->remoteIP().toString()) {
      request->send(403, "text/plain", F("render must come from the leader"));
      return;
    }
    if (!request->hasParam("epoch", true) || !request->hasParam("seq", true) ||
        !request->hasParam("text", true)) {
      request->send(400, "text/plain", F("Missing epoch/seq/text"));
      return;
    }
    uint32_t epoch = (uint32_t)strtoul(
        request->getParam("epoch", true)->value().c_str(), nullptr, 10);
    uint32_t seq = (uint32_t)strtoul(
        request->getParam("seq", true)->value().c_str(), nullptr, 10);
    String text = request->getParam("text", true)->value();
    int speed;
    if (request->hasParam("speed", true)) {
      speed = request->getParam("speed", true)->value().toInt();
      if (speed < 1 || speed > 100) {
        request->send(400, "text/plain", F("Speed must be 1..100"));
        return;
      }
    } else {
      WebStateLock lock;
      speed = liveSettings->flapSpeed;
    }
    uint64_t commitAtMs =
        request->hasParam("commitAtMs", true)
            ? strtoull(request->getParam("commitAtMs", true)->value().c_str(),
                       nullptr, 10)
            : 0ULL;
    // Wire-auth (#313 follow-on): once a key is negotiated, the render must
    // carry a valid ts+mac over its exact content — a spoofed-IP host without
    // the key cannot forge it, and the mac binds the text so it can't be
    // swapped. Rebuild the canonical message from the wire params.
    if (clusterFollowerHmacEnforced()) {
      if (!request->hasParam("ts", true) || !request->hasParam("mac", true)) {
        request->send(403, "text/plain", F("cluster signature required"));
        return;
      }
      uint64_t ts = strtoull(request->getParam("ts", true)->value().c_str(),
                             nullptr, 10);
      String mac = request->getParam("mac", true)->value();
      String msg = clusterHmacRenderMsg(ts, epoch, seq, text, speed, commitAtMs);
      if (!clusterFollowerVerifySigned(msg, ts, mac)) {
        request->send(403, "text/plain", F("bad cluster signature"));
        return;
      }
    }
    ClusterRenderVerdict v =
        clusterFollowerHandleRender(epoch, seq, text, speed, commitAtMs);
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
    // #294: the leader piggybacks the cluster digest (+ this member's
    // table index) on the ping body, and the reply carries this row's
    // unit health — both additive; either side may predate the other.
    String digest = request->hasParam("digest", true)
                        ? request->getParam("digest", true)->value()
                        : String();
    int youIndex = request->hasParam("you", true)
                       ? (int)request->getParam("you", true)->value().toInt()
                       : -1;
    // Wire-auth (#313 follow-on): a keyed follower requires a valid ts+mac
    // before the digest/you piggyback is trusted or contact is refreshed.
    if (clusterFollowerHmacEnforced()) {
      if (!request->hasParam("ts", true) || !request->hasParam("mac", true)) {
        request->send(403, "text/plain", F("cluster signature required"));
        return;
      }
      uint64_t ts = strtoull(request->getParam("ts", true)->value().c_str(),
                             nullptr, 10);
      String mac = request->getParam("mac", true)->value();
      if (!clusterFollowerVerifySigned(clusterHmacPingMsg(ts, digest, youIndex),
                                       ts, mac)) {
        request->send(403, "text/plain", F("bad cluster signature"));
        return;
      }
    }
    if (!clusterFollowerHandlePing(digest, youIndex,
                                   request->client()->remoteIP().toString())) {
      request->send(409, "application/json",
                    F("{\"error\":\"not clustered\"}"));
      return;
    }
    ClusterFollowerView cv = clusterFollowerViewGet();
    DisplaySnapshot snap = displaySnapshotGet();
    WearAssessment wear;
    assessWear(snap.units, snap.displayWidth, wear);
    String out = "{\"state\":";
    appendJsonString(out, clusterFollowerPhaseName(cv.phase));
    out += ",\"epoch\":";
    out += String((unsigned long)cv.epoch);
    out += ",\"seq\":";
    out += String((unsigned long)cv.lastSeq);
    out += clusterPingHealthJson(snap.units, snap.displayWidth,
                                 snap.detectedUnitCount, snap.faultyUnitCount,
                                 wear.flaggedCount > 0, GIT_REV);
    // #332 additive: refresh our role every ping so a live role change
    // reorders the leader's succession tiers without a re-join.
    String selfRole;
    {
      WebStateLock lock;
      if (liveSettings) selfRole = liveSettings->deviceRole;
    }
    out += ",\"role\":";
    appendJsonString(out, selfRole);
    out += '}';
    request->send(200, "application/json", out);
  });

  // #294 rung 2: the stored ping-piggybacked digest — any pane renders the
  // whole wall from it. 404 = not clustered / nothing held yet (the page
  // falls back to the standalone view).
  server.on("/cluster/digest", HTTP_GET, [](AsyncWebServerRequest* request) {
    uint32_t ageMs = 0;
    String digest = clusterFollowerDigestGet(ageMs);
    if (digest.length() == 0) {
      request->send(404, "application/json", F("{\"error\":\"no digest\"}"));
      return;
    }
    String out = "{\"ageMs\":";
    out += String((unsigned long)ageMs);
    out += ",\"digest\":";
    out += digest;  // raw JSON from the leader — embedded, not escaped
    out += '}';
    request->send(200, "application/json", out);
  });

  // #295: one-click takeover from a follower that has written the leader
  // off. The staged config swap runs in clusterTask; sticky-leadership
  // join 409s resolve any promote race.
  server.on("/cluster/promote", HTTP_POST,
            [](AsyncWebServerRequest* request) {
              ClusterPromoteVerdict v = clusterFollowerPromote();
              String out = "{\"message\":";
              appendJsonString(out, v.message);
              out += '}';
              request->send(v.httpStatus, "application/json", out);
            });

  server.on("/cluster/leave", HTTP_POST, [](AsyncWebServerRequest* request) {
    // Combined guard (#313): /cluster/leave has TWO legitimate callers — the
    // leader's reconfigure fan-out (server-to-server, no Origin, dials from
    // leaderHost) and the local "Leave" button in the Cluster card (a LAN
    // browser). So honor a leave from the leader's IP OR from a LAN-origin
    // browser; a bare non-leader LAN host (no Origin, wrong IP) is refused,
    // closing the any-host force-leave DoS without breaking the UI button.
    // (The CSRF middleware has already 403'd any non-LAN browser origin.)
    ClusterFollowerView cv = clusterFollowerViewGet();
    bool fromLanBrowser = request->hasHeader("Origin") &&
                          clusterCorsOriginAllowed(request->header("Origin"));
    if (clusterFollowerHmacEnforced()) {
      // Keyed (#313 follow-on): the leader arm becomes a valid SIGNATURE
      // (beats a spoofed IP); the local Leave button rides the LAN-browser
      // arm. A bare unsigned non-browser leave is refused.
      bool signedOk = false;
      if (request->hasParam("ts", true) && request->hasParam("mac", true)) {
        uint64_t ts = strtoull(request->getParam("ts", true)->value().c_str(),
                               nullptr, 10);
        signedOk = clusterFollowerVerifySigned(
            clusterHmacLeaveMsg(ts), ts,
            request->getParam("mac", true)->value());
      }
      if (!signedOk && !fromLanBrowser) {
        request->send(403, "text/plain",
                      F("leave requires a valid signature or this display's "
                        "own web UI"));
        return;
      }
    } else {
      // Pre-HMAC combined guard (#313): the leader's reconfigure fan-out
      // (no Origin, dials from leaderHost) OR the local browser; a bare
      // non-leader LAN host is refused (closes the any-host force-leave DoS).
      bool fromLeader =
          cv.leaderHost.length() > 0 &&
          cv.leaderHost == request->client()->remoteIP().toString();
      if (cv.leaderHost.length() > 0 && !fromLeader && !fromLanBrowser) {
        request->send(403, "text/plain", F("leave must come from the leader "
                                           "or this display's own web UI"));
        return;
      }
    }
    clusterFollowerHandleLeave();  // idempotent
    request->send(200, "text/plain", F("ok"));
  });

  server.on("/cluster/health", HTTP_GET, [](AsyncWebServerRequest* request) {
    ClusterFollowerView cv = clusterFollowerViewGet();
    DisplaySnapshot snap = displaySnapshotGet();
    String out = "{\"state\":";
    appendJsonString(out, clusterFollowerPhaseName(cv.phase));
    out += ",\"leaderName\":";
    appendJsonString(out, cv.leaderName);
    out += ",\"leaderHost\":";
    appendJsonString(out, cv.leaderHost);
    out += ",\"row\":";
    out += cv.row;
    out += ",\"epoch\":";
    out += String((unsigned long)cv.epoch);
    out += ",\"seq\":";
    out += String((unsigned long)cv.lastSeq);
    out += ",\"segment\":";
    appendJsonString(out, cv.heldSegment);
    out += ",\"rev\":\"" GIT_REV "\",\"width\":";
    out += (int)snap.displayWidth;
    out += ",\"detected\":";
    out += (int)snap.detectedUnitCount;
    out += ",\"faulty\":";
    out += (int)snap.faultyUnitCount;
    out += ",\"hmac\":";  // #313 follow-on: enforcing signed leader-wire requests
    out += clusterFollowerHmacEnforced() ? "true" : "false";
    out += '}';
    request->send(200, "application/json", out);
  });

  // --- Cluster leader endpoints (#273) ----------------------------------------
  // `members` uses the ClusterLeaderPolicy wire format
  // (`host|row|col|width;…`, empty host = this master's own row; "" =
  // disable). Validation runs here for the 400; the swap itself (leave
  // fan-out, NVS persist, runtime reset) runs in clusterTask.
  server.on("/cluster/config", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("members", true)) {
      request->send(400, "text/plain", F("Missing members"));
      return;
    }
    ClusterConfigVerdict v = clusterLeaderStageConfig(
        request->getParam("members", true)->value());
    request->send(v.httpStatus, "text/plain", v.message);
  });

  // Board discovery for the Cluster card (#274): POST arms the staged
  // flag (re-POST while pending is a no-op — the flag is the re-entry
  // guard); the blocking MDNS.queryService pass runs from netTask's
  // drain, never here (/mqtt/discover contract).
  server.on("/cluster/discover", HTTP_POST,
            [](AsyncWebServerRequest* request) {
    {
      WebStateLock lock;
      if (!clusterDiscoverPending) {
        clusterDiscoverResultJson = "";
        clusterDiscoverPending = true;
      }
    }
    request->send(200, "text/plain", F("Board discovery started"));
  });
  server.on("/cluster/discover", HTTP_GET,
            [](AsyncWebServerRequest* request) {
    bool pending;
    String json;
    {
      WebStateLock lock;
      pending = clusterDiscoverPending;
      json = clusterDiscoverResultJson;
    }
    if (pending) {
      request->send(202, "text/plain", F("Discovery running"));
      return;
    }
    if (json.length() == 0) {
      request->send(404, "text/plain", F("No discovery has run yet"));
      return;
    }
    request->send(200, "application/json", json);
  });

  server.on("/cluster/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    // One wire shape for the leader's status (#273 + #294 health keys),
    // shared with the ping digest — serializer in ClusterDigest.h.
    request->send(200, "application/json",
                  clusterStatusJson(clusterLeaderStatusGet()));
  });

  // --- ESP-01 follower firmware: store + relay (#304 Part B) ------------------
  // Upload a follower-<rev>.bin ONCE to the S3 (same-origin — no CORS); it's
  // held on `storage` LittleFS and streamed on demand to esp01 rows by
  // clusterTask, so the wall never browser-uploads to a follower (keeps #294's
  // /firmware/* closed). Same async-context exception as the master OTA: the
  // stream is accumulated in PSRAM here, MD5-verified, then handed to netTask.
  // The follower-*.bin prefix guard mirrors ota-flash.sh #299 (an S3 image
  // bricks the ESP-01).
  server.on(
      "/cluster/follower-firmware", HTTP_POST,
      [](AsyncWebServerRequest* request) {
        if (followerFwRejectStatus != 0) {
          int s = followerFwRejectStatus;
          String r = followerFwRejectReason;
          followerFwRejectStatus = 0;
          followerFwRejectReason = "";
          request->send(s, "text/plain", r);
          return;
        }
        if (!followerImageWriteEnd()) {
          request->send(500, "text/plain",
                        "Follower image store failed: " +
                            followerImageWriteError());
          return;
        }
        request->send(200, "text/plain",
                      F("Follower image stored — use ‘Update firmware’ "
                        "on an ESP-01 member to push it."));
      },
      [](AsyncWebServerRequest* request, String filename, size_t index,
         uint8_t* data, size_t len, bool final) {
        if (index == 0) {
          followerFwRejectStatus = 0;
          followerFwRejectReason = "";
          // CSRF gate (#313), INLINE before followerImageWriteBegin — the
          // middleware fires post-body, too late for an upload route. This
          // upload is same-origin from the board's own UI, so its LAN Origin
          // passes; a forged cross-site push is refused.
          if (webUploadCsrfRejected(request)) {
            followerFwRejectStatus = 403;
            followerFwRejectReason =
                "Cross-origin follower-image upload refused (CSRF guard)";
            return;
          }
          String rev;
          if (!followerImageUploadAccepts(filename, rev)) {
            followerFwRejectStatus = 400;
            followerFwRejectReason =
                "expected a follower-<rev>.bin (an S3 image would brick the "
                "ESP-01)";
            return;
          }
          String md5 = request->hasParam("md5")
                           ? request->getParam("md5")->value()
                           : String();
          if (md5.length() == 0) {
            followerFwRejectStatus = 400;
            followerFwRejectReason = "md5 query parameter is required";
            return;
          }
          if (!normalizeOtaMd5(md5)) {
            followerFwRejectStatus = 400;
            followerFwRejectReason = "md5 must be exactly 32 hex characters";
            return;
          }
          if (!followerImageWriteBegin(md5, rev)) {
            followerFwRejectStatus = 409;
            followerFwRejectReason =
                "cannot start: " + followerImageWriteError();
            return;
          }
        }
        if (followerFwRejectStatus != 0) return;
        if (len > 0 && !followerImageWriteChunk(data, len, index)) {
          followerFwRejectStatus = 400;
          followerFwRejectReason =
              "upload failed: " + followerImageWriteError();
          return;
        }
        // final: the completion handler calls followerImageWriteEnd().
      });

  // On-demand: stream the stored image to one esp01 member. Validation +
  // eligibility live in clusterLeaderStageFollowerPush; the file stream runs
  // on clusterTask (never here). Progress rides GET /cluster/status.
  server.on("/cluster/member/update", HTTP_POST,
            [](AsyncWebServerRequest* request) {
    String host = request->hasParam("host", true)
                      ? request->getParam("host", true)->value()
                  : request->hasParam("host")
                      ? request->getParam("host")->value()
                      : String();
    ClusterConfigVerdict v = clusterLeaderStageFollowerPush(host);
    request->send(v.httpStatus, "text/plain", v.message);
  });
}

// Cluster board discovery drain (#274): same lock-domain rationale as the
// MQTT pass (WebSettings.cpp) — mDNS takes LWIP locks, so the blocking query
// runs here in netTask, never in a handler.
void webClusterDiscoverLoop() {
  bool clusterDiscoverDue;
  {
    WebStateLock lock;
    clusterDiscoverDue = clusterDiscoverPending;
  }
  if (!clusterDiscoverDue) return;
  ClusterDiscoveredBoard boards[CLUSTER_DISCOVER_MAX_BOARDS];
  size_t count = 0;
  int n = MDNS.queryService("splitflap", "tcp");
  for (int i = 0; i < n && count < CLUSTER_DISCOVER_MAX_BOARDS; i++) {
    // TXT name (what the board calls itself) over the answer hostname —
    // identical today, but the TXT survives mDNS conflict renaming.
    String txtName = MDNS.txt(i, "name");
    String name = txtName.length() > 0
                      ? txtName
                      : normalizeMdnsHostname(MDNS.hostname(i));
    // Self and nameless answers must not consume result slots — with a
    // full wall the leader's own advertisement would otherwise crowd out
    // the last real board (the JSON builder re-filters as backstop).
    if (name.length() == 0 || name.equalsIgnoreCase(effectiveName)) {
      continue;
    }
    ClusterDiscoveredBoard& b = boards[count++];
    b.name = name;
    IPAddress a = MDNS.address(i);
    b.ip = a == IPAddress() ? String() : a.toString();
    b.rev = MDNS.txt(i, "rev");
    b.width = clusterParseTxtWidth(MDNS.txt(i, "width"));
    b.plat = MDNS.txt(i, "plat");  // "" = S3 master (#297)
  }
  String json = buildClusterDiscoverJson(boards, count, effectiveName);
  SerialPrintf("Cluster discover: %u board(s)\n", (unsigned)count);
  WebStateLock lock;
  clusterDiscoverResultJson = json;
  clusterDiscoverPending = false;
}
