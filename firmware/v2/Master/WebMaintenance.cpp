// Unit health + calibration/provisioning endpoints (#203/#204/#205) — split
// from WebEndpoints.cpp (#338); async-context rules in WebEndpoints.cpp's
// header. The async rule is structural here: only displayTask holds Wire, so
// every handler validates against a snapshot COPY, enqueues a DisplayCommand
// with a fresh seq and answers {"seq":N}; execution truth arrives via
// GET /unit/op-result plus the refreshed health facts.

#include "WebEndpoints.h"
#include "WebEndpointsInternal.h"

#include <ESPAsyncWebServer.h>

#include <memory>

#include "HelpersSerialHandling.h"
#include "MaintenancePolicy.h"
#include "ReflashPlan.h"
#include "SplitFlapProtocol.h"
#include "Tasks.h"
#include "UnitBus.h"
#include "WearPolicy.h"
#include "ClusterLeader.h"  // clusterLeaderBlankWall (/stop propagation #317)

// v1 parseCalibrationAddress as a seam: policy verdicts come from the pure
// MaintenancePolicy.h, this only translates request → verdict → response.
// Validates against the caller's snapshot COPY — a fast, possibly stale
// view; displayTask re-runs whatever check the queue delay can invalidate.
static bool maintCheckAddress(AsyncWebServerRequest* request,
                              const DisplaySnapshot& snap, int& outAddr) {
  String raw;
  bool provided = request->hasParam("address");
  if (provided) raw = request->getParam("address")->value();
  MaintVerdict verdict = maintValidateAddress(provided ? raw.c_str() : nullptr,
                                              snap.units, UNITS_AMOUNT,
                                              outAddr);
  if (verdict.httpStatus != 200) {
    request->send(verdict.httpStatus, "text/plain", verdict.message);
    return false;
  }
  return true;
}

// Required numeric query param (strtol base 0, v1 hex support). Sends the
// 400 itself so callers just early-return.
static bool maintRequireLongParam(AsyncWebServerRequest* request,
                                  const char* name, long& out) {
  if (!request->hasParam(name)) {
    String msg = "Missing '";
    msg += name;
    msg += "' query param";
    request->send(400, "text/plain", msg);
    return false;
  }
  String raw = request->getParam(name)->value();
  char* end = nullptr;
  out = strtol(raw.c_str(), &end, 0);
  if (end == raw.c_str()) {
    String msg = "'";
    msg += name;
    msg += "' must be a number";
    request->send(400, "text/plain", msg);
    return false;
  }
  return true;
}

// Producer gate (#205): while a reflash job runs, display-mutating POSTs
// answer 409 instead of enqueueing — nothing piles up behind the job to
// burst-drain afterwards. /stop is the ONE exception (it is the cancel)
// and deliberately does not call this.
static bool rejectWhileReflashing(AsyncWebServerRequest* request) {
  if (!reflashInProgress(displaySnapshotGet().reflash)) return false;
  request->send(409, "text/plain",
                F("Unit reflash in progress — retry when it finishes"));
  return true;
}

// Enqueue-or-503 tail shared by every maintenance POST: the client either
// gets its correlation seq or an honest busy — never a silently dropped op.
// Every caller except /stop funnels through here, so the reflash gate
// lives here too.
static void maintEnqueue(AsyncWebServerRequest* request,
                         const DisplayCommand& cmd) {
  if (rejectWhileReflashing(request)) return;
  if (!displayEnqueue(cmd)) {
    request->send(503, "text/plain",
                  F("Display queue full — try again in a moment"));
    return;
  }
  char buf[24];
  snprintf(buf, sizeof(buf), "{\"seq\":%lu}", (unsigned long)cmd.seq);
  request->send(200, "application/json", buf);
}

void webMaintenanceRegister(AsyncWebServer& server) {
  // --- unit health (#203, v1 #45 wire contract) -----------------------------
  // GET renders JSON from the snapshot copy — never touches the bus from
  // async context.
  server.on("/units/health", HTTP_GET, [](AsyncWebServerRequest* request) {
    DisplaySnapshot snap = displaySnapshotGet();
    // Heap, not stack: ~2 KB doesn't belong on the async_tcp task stack,
    // and a static buffer would race concurrent requests.
    std::unique_ptr<char[]> buf(new char[UNIT_HEALTH_JSON_CAP]);
    size_t n =
        buildUnitHealthJson(buf.get(), UNIT_HEALTH_JSON_CAP, snap.units,
                            snap.displayWidth, snap.faultyUnitCount,
                            SFP_I2C_ADDRESS_BASE, millis());
    if (n == 0 || n >= UNIT_HEALTH_JSON_CAP) {
      // Would-be-truncated payload: fall back to a valid headline-only JSON
      // rather than shipping a cut object (v1 truncation discipline).
      n = (size_t)snprintf(buf.get(), UNIT_HEALTH_JSON_CAP,
                           "{\"width\":%d,\"faulty\":%d,\"units\":[]}",
                           snap.displayWidth, snap.faultyUnitCount);
    }
    // Wear assessment rides the same payload (#231, additive key), spliced
    // before the closing brace like the reflash object below.
    WearAssessment wear;
    assessWear(snap.units, snap.displayWidth, wear);
    char wearJson[96];
    size_t wearLen = buildWearJson(wear, wearJson, sizeof(wearJson));
    if (n > 0 && wearLen < sizeof(wearJson) &&
        n + wearLen + 2 < UNIT_HEALTH_JSON_CAP) {
      n += (size_t)snprintf(buf.get() + n - 1, UNIT_HEALTH_JSON_CAP - n + 1,
                            ",%s}", wearJson) - 1;
    }
    // Reflash progress rides the same payload (#205, additive key — the
    // Maintenance tab already polls this endpoint). Spliced before the
    // closing brace; the ~70 B worst case fits the cap's slack by design.
    char reflashJson[80];
    buildReflashJson(reflashJson, sizeof(reflashJson), snap.reflash);
    if (n > 0 && n + strlen(reflashJson) + 13 < UNIT_HEALTH_JSON_CAP) {
      snprintf(buf.get() + n - 1, UNIT_HEALTH_JSON_CAP - n + 1,
               ",\"reflash\":%s}", reflashJson);
    }
    request->send(200, "application/json", buf.get());
  });
  // POST enqueues a Probe — displayTask re-scans the bus AND re-polls
  // health in one pass (a probe subsumes v1's plain re-poll; ?probe=1 is
  // accepted for wire compat but changes nothing). 202 mirrors v1's
  // "pending"; a full queue reports busy like v1's flash-in-progress 503.
  server.on("/units/health/refresh", HTTP_POST,
            [](AsyncWebServerRequest* request) {
              if (rejectWhileReflashing(request)) return;
              if (!displayEnqueue(makeProbeCommand())) {
                request->send(503, "application/json",
                              F("{\"status\":\"busy\"}"));
                return;
              }
              request->send(202, "application/json",
                            F("{\"status\":\"pending\"}"));
            });

  // --- calibration + provisioning (#204, queue-native) ----------------------

  server.on("/unit/offset", HTTP_GET, [](AsyncWebServerRequest* request) {
    DisplaySnapshot snap = displaySnapshotGet();
    int addr = 0;
    if (!maintCheckAddress(request, snap, addr)) return;
    const UnitFacts& unit = snap.units[addr - SFP_I2C_ADDRESS_BASE];
    if (!unit.offsetValid) {
      // v1 wording (pre-#32 unit firmware has no GET_OFFSET); also covers
      // "reads invalidated until the next probe".
      request->send(502, "text/plain",
                    F("Unit did not return a valid offset (firmware may "
                      "predate #32)"));
      return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"offset\":%d}", (int)unit.offset);
    request->send(200, "application/json", buf);
  });

  server.on("/unit/offset", HTTP_POST, [](AsyncWebServerRequest* request) {
    DisplaySnapshot snap = displaySnapshotGet();
    int addr = 0;
    if (!maintCheckAddress(request, snap, addr)) return;
    long value = 0;
    if (!maintRequireLongParam(request, "value", value)) return;
    MaintVerdict verdict = maintValidateOffset(value);
    if (verdict.httpStatus != 200) {
      request->send(verdict.httpStatus, "text/plain", verdict.message);
      return;
    }
    maintEnqueue(request, makeWriteOffsetCommand(displayNextMaintSeq(),
                                                 (uint8_t)addr,
                                                 (int16_t)value));
  });

  server.on("/unit/jog", HTTP_POST, [](AsyncWebServerRequest* request) {
    DisplaySnapshot snap = displaySnapshotGet();
    int addr = 0;
    if (!maintCheckAddress(request, snap, addr)) return;
    long steps = 0;
    if (!maintRequireLongParam(request, "steps", steps)) return;
    MaintVerdict verdict = maintValidateJog(steps);
    if (verdict.httpStatus != 200) {
      request->send(verdict.httpStatus, "text/plain", verdict.message);
      return;
    }
    maintEnqueue(request, makeJogCommand(displayNextMaintSeq(), (uint8_t)addr,
                                         (int)steps));
  });

  server.on("/unit/home", HTTP_POST, [](AsyncWebServerRequest* request) {
    DisplaySnapshot snap = displaySnapshotGet();
    int addr = 0;
    if (!maintCheckAddress(request, snap, addr)) return;
    maintEnqueue(request,
                 makeHomeCommand(displayNextMaintSeq(), (uint8_t)addr));
  });

  server.on("/unit/identify", HTTP_POST, [](AsyncWebServerRequest* request) {
    DisplaySnapshot snap = displaySnapshotGet();
    int addr = 0;
    if (!maintCheckAddress(request, snap, addr)) return;
    maintEnqueue(request,
                 makeIdentifyCommand(displayNextMaintSeq(), (uint8_t)addr));
  });

  // Physical-rebuild bookkeeping (#231): zero the unit's wear odometer after
  // a flap swap or motor replacement. Same op contract as identify/home.
  server.on("/unit/reset-odometer", HTTP_POST,
            [](AsyncWebServerRequest* request) {
    DisplaySnapshot snap = displaySnapshotGet();
    int addr = 0;
    if (!maintCheckAddress(request, snap, addr)) return;
    maintEnqueue(request, makeResetOdometerCommand(displayNextMaintSeq(),
                                                   (uint8_t)addr));
  });

  // On-demand unit self-test (#265): the unit measures its own mechanics
  // (steps/rev, hall window, rev time) over ~2 revolutions. Same op
  // contract as identify/home; the measurements come back via
  // GET /unit/self-test-result.
  server.on("/unit/self-test", HTTP_POST, [](AsyncWebServerRequest* request) {
    DisplaySnapshot snap = displaySnapshotGet();
    int addr = 0;
    if (!maintCheckAddress(request, snap, addr)) return;
    maintEnqueue(request,
                 makeSelfTestCommand(displayNextMaintSeq(), (uint8_t)addr));
  });

  // Self-test execution feedback: pending / ok(+measurements) /
  // failed(+reason) / expired, rendered from the snapshot's single
  // SelfTestSlot — the self-test twin of /unit/op-result.
  server.on("/unit/self-test-result", HTTP_GET,
            [](AsyncWebServerRequest* request) {
    long seq = 0;
    if (!maintRequireLongParam(request, "seq", seq)) return;
    if (seq < 1) {
      request->send(400, "text/plain", F("seq must be >= 1"));
      return;
    }
    DisplaySnapshot snap = displaySnapshotGet();
    char buf[128];
    buildSelfTestJson(buf, sizeof(buf), snap.lastSelfTest, (uint32_t)seq);
    request->send(200, "application/json", buf);
  });

  // Debug endpoint, v1 semantics preserved: pushes the unit into twiboot
  // (~1 s on its DIP-derived address, then back to the sketch). v1 parity:
  // range check only, no sketch-state gate — it exists precisely for poking
  // at units the probe view might mislabel. displayTask deliberately does
  // NOT reprobe afterwards (v1 #88: probing the twiboot window pins the
  // bootloader alive).
  server.on("/unit/reboot", HTTP_POST, [](AsyncWebServerRequest* request) {
    long addr = 0;
    if (!maintRequireLongParam(request, "address", addr)) return;
    if (addr < 1 || addr > 126) {
      request->send(400, "text/plain", F("Address must be 1..126"));
      return;
    }
    maintEnqueue(request, makeRebootToBootloaderCommand(displayNextMaintSeq(),
                                                        (uint8_t)addr));
  });

  server.on("/unit/set-address", HTTP_POST,
            [](AsyncWebServerRequest* request) {
              DisplaySnapshot snap = displaySnapshotGet();
              int addr = 0;
              if (!maintCheckAddress(request, snap, addr)) return;
              long target = 0;
              if (!maintRequireLongParam(request, "value", target)) return;
              // Fast 409 from the copy; displayTask re-runs the same policy
              // against live facts right before the burn (authoritative).
              MaintVerdict verdict = maintValidateSetAddressTarget(
                  target, addr, snap.units, UNITS_AMOUNT);
              if (verdict.httpStatus != 200) {
                request->send(verdict.httpStatus, "text/plain",
                              verdict.message);
                return;
              }
              maintEnqueue(request,
                           makeSetAddressCommand(displayNextMaintSeq(),
                                                 (uint8_t)addr,
                                                 (uint8_t)target));
            });

  server.on("/unit/clear-address", HTTP_POST,
            [](AsyncWebServerRequest* request) {
              DisplaySnapshot snap = displaySnapshotGet();
              int addr = 0;
              if (!maintCheckAddress(request, snap, addr)) return;
              maintEnqueue(request, makeClearAddressCommand(
                                        displayNextMaintSeq(), (uint8_t)addr));
            });

  // Blank-out recalibration (v1 semantics: two full-row frames force the
  // wrap-around re-home, then the enqueue-time text returns). Text comes
  // from the display snapshot (what is actually showing), alignment/speed
  // from the settings of this moment — all baked into the command.
  server.on("/reset-units", HTTP_POST, [](AsyncWebServerRequest* request) {
    SerialPrintln(F("Units reset requested from web UI"));
    DisplaySnapshot snap = displaySnapshotGet();
    WebContentSnapshot content = webDisplayContentSnapshot();
    maintEnqueue(request, makeResetUnitsCommand(
                              displayNextMaintSeq(), String(snap.currentText),
                              content.alignment, content.flapSpeed));
  });

  // Kill switch (v1 #35). Order is load-bearing (review 2026-07-11): the
  // abort flag is set BEFORE the enqueue so the queue's happens-before
  // guarantees displayTask's Stop always finds it set — set-after-enqueue
  // races an idle displayTask clearing it first, stranding the flag ON for
  // every future wait. A 503 rolls the flag back (nothing queued to abort).
  server.on("/stop", HTTP_POST, [](AsyncWebServerRequest* request) {
    SerialPrintln(F("Stop requested from web UI"));
    DisplayCommand cmd = makeStopCommand(displayNextMaintSeq());
    unitBusRequestAbort();
    if (!displayEnqueue(cmd)) {
      unitBusClearAbort();
      request->send(503, "text/plain",
                    F("Display queue full — try again in a moment"));
      return;
    }
    // Cluster propagation (#317): when leading, Stop blanks the WHOLE wall —
    // the local command above handles this board's own row; blank the
    // followers in sync (no-op when standalone).
    clusterLeaderBlankWall();
    char buf[24];
    snprintf(buf, sizeof(buf), "{\"seq\":%lu}", (unsigned long)cmd.seq);
    request->send(200, "application/json", buf);
  });

  // Execution feedback for the queued ops: pending / ok / failed(+reason) /
  // expired, rendered from the snapshot's single MaintResult slot.
  server.on("/unit/op-result", HTTP_GET, [](AsyncWebServerRequest* request) {
    long seq = 0;
    if (!maintRequireLongParam(request, "seq", seq)) return;
    if (seq < 1) {
      request->send(400, "text/plain", F("seq must be >= 1"));
      return;
    }
    DisplaySnapshot snap = displaySnapshotGet();
    char buf[96];
    buildOpResultJson(buf, sizeof(buf), snap.lastMaint, (uint32_t)seq);
    request->send(200, "application/json", buf);
  });

  // Bulk unit reflash (#205): pushes every unit not on the bundled rev
  // through twiboot with the PROGMEM-embedded image. Queue-native like all
  // maintenance ops — {"seq":N} now, job outcome via /unit/op-result, live
  // progress in /units/health's reflash object. Text/alignment/speed baked
  // at enqueue (the job re-shows them; reflashed units homed to blank).
  server.on("/reflash-units", HTTP_POST, [](AsyncWebServerRequest* request) {
    SerialPrintln(F("Unit reflash requested from web UI"));
    DisplaySnapshot snap = displaySnapshotGet();
    WebContentSnapshot content = webDisplayContentSnapshot();
    maintEnqueue(request, makeReflashUnitsCommand(
                              displayNextMaintSeq(), String(snap.currentText),
                              content.alignment, content.flapSpeed));
  });
}
