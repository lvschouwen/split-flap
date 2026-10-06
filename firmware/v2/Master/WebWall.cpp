// WebWall.cpp — the first routes of the operator API that covers the whole
// Split-Flap (#559/#566): POST /api/v2/action starts a job and answers with
// its id, GET /api/v2/op/{id} says what became of it, GET /api/v2/wall lists
// the boards. Async context: handlers read WallState snapshots and stage
// requests, they never change anything themselves (WebEndpoints.cpp rules).
//
// Actions so far, all about which boards make up the wall:
//   {"name":"pair","target":{"host":"192.168.1.50"},"args":{"row":1,"col":0,"width":5}}
//       args are optional: below the last row, left edge, as wide as the row says;
//       target.port is the row's web port when it is not 80 (a bench stand-in)
//   {"name":"release","target":{"row":"<row id>"}}
//   {"name":"arrange","args":{"rows":[{"id":"","row":0,"col":0,"width":16}, ...]}}
//       every board of the table once, "" = the master's own row
// Answers: 202 {"op":N}; 400 with the reason; 409 while another such request
// runs; 503 when no job can be started.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncJson.h>

#include "BuildVersion.h"
#include "JsonCopied.h"
#include "Tasks.h"
#include "WallShow.h"
#include "WallState.h"
#include "WebEndpointsInternal.h"

namespace {

constexpr size_t ACTION_BODY_MAX = 1024;

void sendError(AsyncWebServerRequest* request, int status, const char* message) {
  AsyncJsonResponse* response = new AsyncJsonResponse();
  response->getRoot()["error"] = message;
  response->setCode(status);
  response->setLength();
  request->send(response);
}

// A whole number in [0, 255] under `key`, -1 when the key is absent; false
// when it is there and is not one.
bool readPlace(JsonVariantConst args, const char* key, int16_t& out) {
  out = -1;
  JsonVariantConst v = args[key];
  if (v.isNull()) return true;
  if (!v.is<int>() || v.as<int>() < 0 || v.as<int>() > 255) return false;
  out = (int16_t)v.as<int>();
  return true;
}

const char* buildPair(JsonVariantConst body, WallRequest& request) {
  const char* host = body["target"]["host"].as<const char*>();
  uint8_t address[4];
  if (host == nullptr || strlen(host) > CLUSTER_HOST_MAX_LEN || !wallRowHostParse(host, address)) {
    return "target.host must be the row's address on the local network, like 192.168.1.50";
  }
  strcpy(request.host, host);
  JsonVariantConst port = body["target"]["port"];
  if (!port.isNull()) {
    if (!port.is<int>() || port.as<int>() < 1 || port.as<int>() > 65535) {
      return "target.port is the row's web port, 1 to 65535";
    }
    request.port = (uint16_t)port.as<int>();
  }
  JsonVariantConst args = body["args"];
  if (!readPlace(args, "row", request.place.row) || !readPlace(args, "col", request.place.col) ||
      !readPlace(args, "width", request.place.width)) {
    return "args.row, args.col and args.width are whole numbers from 0 to 255";
  }
  request.kind = WallRequestKind::Pair;
  return nullptr;
}

const char* buildRelease(JsonVariantConst body, const WallRowsTable& table, WallRequest& request) {
  const char* id = body["target"]["row"].as<const char*>();
  if (id == nullptr || wallRowsFind(table, id) < 0) return "target.row is not a row of this wall";
  strcpy(request.id, table.rows[wallRowsFind(table, id)].id);
  request.kind = WallRequestKind::Release;
  return nullptr;
}

// The same boards at new places. Membership changes only by pair and release.
const char* buildArrange(JsonVariantConst body, const WallRowsTable& table, WallRequest& request) {
  JsonArrayConst rows = body["args"]["rows"].as<JsonArrayConst>();
  if (rows.isNull() || (int)rows.size() != table.count) {
    return "args.rows must list every board of the wall once";
  }
  request.table = table;
  bool placed[CLUSTER_MAX_MEMBERS] = {false};
  for (JsonVariantConst entry : rows) {
    const char* id = entry["id"].as<const char*>();
    if (id == nullptr) return "every entry of args.rows needs an id (\"\" = the master's own row)";
    const int at = id[0] == 0 ? wallRowsOwn(table) : wallRowsFind(table, id);
    if (at < 0 || placed[at]) return "args.rows must list every board of the wall once";
    placed[at] = true;
    int16_t row, col, width;
    if (!readPlace(entry, "row", row) || !readPlace(entry, "col", col) ||
        !readPlace(entry, "width", width) || row < 0 || col < 0 || width < 0) {
      return "every entry of args.rows needs row, col and width from 0 to 255";
    }
    request.table.rows[at].row = (uint8_t)row;
    request.table.rows[at].col = (uint8_t)col;
    request.table.rows[at].width = (uint8_t)width;
  }
  ClusterGrid grid;
  const ClusterVerdict verdict = wallRowsValidate(request.table, grid);
  if (!verdict.ok) return verdict.message;
  request.kind = WallRequestKind::Arrange;
  return nullptr;
}

void handleAction(AsyncWebServerRequest* request, JsonVariant& json) {
  JsonVariantConst body = json;
  const char* name = body["name"].as<const char*>();
  if (name == nullptr) return sendError(request, 400, "the body is {\"name\",\"target\",\"args\"}");
  uint32_t generation;
  const WallRowsTable table = wallStateRows(generation);
  // Static: a rows table does not belong on the web server task's stack. The
  // web state lock makes it one request at a time.
  static WallRequest staged;
  WebStateLock lock;
  staged = WallRequest{};
  const char* refusal = "no such action";
  if (strcmp(name, "pair") == 0) refusal = buildPair(body, staged);
  else if (strcmp(name, "release") == 0) refusal = buildRelease(body, table, staged);
  else if (strcmp(name, "arrange") == 0) refusal = buildArrange(body, table, staged);
  if (refusal != nullptr) return sendError(request, 400, refusal);
  staged.opId = wallOpBegin(name, -1);
  if (staged.opId == 0) return sendError(request, 503, "too many jobs are running");
  if (!wallStateStage(staged)) {
    wallOpFinish(staged.opId, false, "another change to the wall was still running");
    return sendError(request, 409, "another change to the wall is still running");
  }
  AsyncJsonResponse* response = new AsyncJsonResponse();
  response->getRoot()["op"] = staged.opId;
  response->setCode(202);
  response->setLength();
  request->send(response);
}

void handleOp(AsyncWebServerRequest* request) {
  // /api/v2/op/<id>
  const String tail = request->url().substring(strlen("/api/v2/op/"));
  char* end = nullptr;
  const unsigned long id = strtoul(tail.c_str(), &end, 10);
  WallOp op;
  if (tail.length() == 0 || *end != 0 || !wallOpGet((uint32_t)id, op)) {
    return sendError(request, 404, "no such job (finished jobs are kept until their place is needed)");
  }
  AsyncJsonResponse* response = new AsyncJsonResponse();
  JsonVariant root = response->getRoot();
  root["op"] = op.id;
  root["name"] = jsonCopied(op.name);
  root["state"] = op.phase == WallOpPhase::Running ? "running"
                  : op.phase == WallOpPhase::Done  ? "done"
                                                   : "failed";
  if (op.phase == WallOpPhase::Done) root["result"] = jsonCopied(op.detail);
  if (op.phase == WallOpPhase::Failed) root["reason"] = jsonCopied(op.detail);
  response->setCode(op.phase == WallOpPhase::Running ? 202 : 200);
  response->setLength();
  request->send(response);
}

const char* reachName(WallRowReach reach) {
  switch (reach) {
    case WallRowReach::Never: return "never";
    case WallRowReach::Up: return "up";
    case WallRowReach::Busy: return "busy";
    case WallRowReach::Away: return "away";
    case WallRowReach::Lost: return "lost";
  }
  return "?";
}

void handleWall(AsyncWebServerRequest* request) {
  // Heap, not this task's stack: the snapshot and one row's unit facts.
  std::unique_ptr<WallSnapshot> wall(new WallSnapshot(wallStateGet()));
  std::unique_ptr<UnitFactsDoc> units(new UnitFactsDoc);
  const DisplaySnapshot own = displaySnapshotGet();
  const uint32_t nowMs = millis();
  AsyncJsonResponse* response = new AsyncJsonResponse();
  JsonVariant root = response->getRoot();
  root["master"]["id"] = effectiveName;
  root["master"]["rev"] = GIT_REV;
  root["master"]["units"] = own.displayWidth;
  JsonArray rows = root["rows"].to<JsonArray>();
  for (int i = 0; i < wall->rows.count; i++) {
    const WallRowDef& def = wall->rows.rows[i];
    JsonObject row = rows.add<JsonObject>();
    row["id"] = jsonCopied(def.id);
    row["own"] = wallRowIsOwn(def);
    row["row"] = def.row;
    row["col"] = def.col;
    row["width"] = def.width;
    char text[WALL_ROW_TEXT_MAX + 1];
    if (wallShowRowText(i, text, sizeof(text))) row["text"] = jsonCopied(text);
    if (wallRowIsOwn(def)) {
      row["showing"] = jsonCopied(own.currentText);
      continue;
    }
    const WallRowLink& link = wall->link[i];
    row["pairedAt"] = jsonCopied(def.host);
    row["reach"] = reachName(wallRowReach(link.contact, nowMs));
    row["connects"] = link.connects;
    row["restarts"] = link.restarts;
    if (link.contact.everHeard) row["heardMsAgo"] = (uint32_t)(nowMs - link.contact.lastHeardMs);
    if (!link.everWelcomed) continue;
    row["address"] = jsonCopied(link.address);
    row["rev"] = jsonCopied(link.rev);
    row["rescue"] = link.rescue;
    row["units"] = link.reportedWidth;
    row["textShown"] = link.textApplied;
    row["shownCount"] = link.shownCount;
    row["lastLateMs"] = link.lastLateMs;
    row["worstLateMs"] = link.worstLateMs;
    if (link.haveStatus) {
      JsonObject s = row["status"].to<JsonObject>();
      s["uptimeS"] = link.status.up_s;
      s["heap"] = link.status.heap;
      s["heapMin"] = link.status.min_heap;
      s["heapLargestBlock"] = link.status.max_block;
      s["heap2"] = link.status.heap2;
      s["rssi"] = link.status.rssi;
      s["txPowerDbm"] = link.status.tx_power / 4.0f;
      s["busTx"] = link.status.bus_tx;
      s["busErrors"] = link.status.bus_err;
      s["busDead"] = link.status.bus_dead;
      s["busEpisodes"] = link.status.bus_episodes;
      s["escalations"] = link.status.escalations;
      s["busy"] = link.status.busy;
      s["imageSize"] = link.status.image_size;
      s["timeSynced"] = link.status.time_synced;
    }
    uint32_t atMs = 0;
    if (wallStateRowUnits(i, *units, atMs)) {
      JsonObject u = row["unitFacts"].to<JsonObject>();
      u["width"] = units->width;
      u["faulty"] = units->faulty;
      u["msAgo"] = (uint32_t)(nowMs - atMs);
    }
  }
  response->setLength();
  request->send(response);
}

}  // namespace

void webWallRegister(AsyncWebServer& server) {
  AsyncCallbackJsonWebHandler* action =
      new AsyncCallbackJsonWebHandler("/api/v2/action", handleAction);
  action->setMethod(HTTP_POST);
  action->setMaxContentLength(ACTION_BODY_MAX);
  server.addHandler(action);
  server.on("/api/v2/op", HTTP_GET, handleOp);
  server.on("/api/v2/wall", HTTP_GET, handleWall);
}
