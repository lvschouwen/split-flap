// WebBoard.cpp — what the operator API reads about one board, one unit and
// the wall's firmware (#559/#572). Async context: every handler renders from
// snapshot copies and changes nothing (WebEndpoints.cpp rules).
//
// GET /api/v2/board/<id>          <id> = a row board's id, or the master's
//     own name for the master and its own row. The board's place and verdict,
//     how it is reached and what it runs; for the master its vitals with
//     their history ("stats", as GET /system/stats), its last starts, its
//     network checks and its rescue image; "settings"; and "units": the
//     table {"fields":[...],"rows":[[...],...]} (UnitApiJson.h), one row a
//     unit in bus order.
// GET /api/v2/unit/<id>/<address>  everything about one unit, grouped
//     (UnitApiJson.h); "position" counts from 0 on its board.
// GET /api/v2/firmware             should be / is: the boards against the
//     master's rev and the stored row image, the units against the bundled
//     unit firmware, their bootloaders, the rescue image.
// GET / PUT /api/v2/settings/wall, /api/v2/settings/board/<master's name>
//     the settings (WallSettingsJson.h). A PUT carries any part of what the
//     GET gives and answers {"done":true,"restart":<needed for it to take
//     effect>}; 400 names the key that was refused. A row board has no
//     settings of its own: its GET is {}, its PUT 409.
// GET /api/v2/log[?row=<id>][&kind=ram|flash][&prev=1]
//     a board's raw log as text. The headers say what it is: X-Log-Board (the
//     board's id) and X-Log-Kind ("ram": what the board holds in memory since
//     it started; "flash": the master's log file that outlives a restart,
//     prev=1 for the file before the present one). A row board sends its log
//     only while someone reads it: the first read asks for it and answers
//     with what has arrived so far, so read again after a second or two, and
//     keep reading to follow it. A row board has no flash log.
// 404 for a board or unit this wall does not have; 503 while a row board's
// units have not arrived yet.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <LittleFS.h>
#include <WiFi.h>

#include <memory>

#include "BootSectionClassify.h"
#include "BootTrace.h"
#include "BuildVersion.h"
#include "CrashContext.h"
#include "FactorySlot.h"
#include "FlashLog.h"
#include "FollowerImageStore.h"
#include "JsonCopied.h"
#include "MqttService.h"
#include "NetLiveness.h"
#include "ReflashPlan.h"
#include "RowLog.h"
#include "SystemStats.h"
#include "SystemStatsPolicy.h"
#include "Tasks.h"
#include "UnitApiJson.h"
#include "WallShow.h"
#include "WallSettingsJson.h"
#include "WallState.h"
#include "WallUpdatePolicy.h"
#include "WallWatch.h"
#include "WearPolicy.h"
#include "WebEndpoints.h"
#include "WebEndpointsInternal.h"
#include "WebLog.h"
#include "WebWallJson.h"

namespace {

void sendError(AsyncWebServerRequest* request, int status, const char* message) {
  AsyncJsonResponse* response = new AsyncJsonResponse();
  response->getRoot()["error"] = message;
  response->setCode(status);
  response->setLength();
  request->send(response);
}

// Everything a handler reads about the wall, taken once. Heap, not the web
// server task's stack.
struct Gathered {
  uint32_t generation = 0;
  WallSnapshot wall;
  WallVerdicts verdicts;
  bool judged = false;
  DisplaySnapshot own;
  UnitFactsDoc rowUnits;  // one row board's, filled by unitsOf()
  uint32_t nowMs = 0;
};

std::unique_ptr<Gathered> gather() {
  std::unique_ptr<Gathered> g(new Gathered);
  g->generation = wallStateRowsGeneration();
  g->wall = wallStateGet();
  // Verdicts of the same rows table, or none: a row's number must mean the
  // same board in both.
  g->judged = wallVerdictsGet(g->verdicts, g->generation) &&
              g->generation == wallStateRowsGeneration();
  g->own = displaySnapshotGet();
  g->nowMs = millis();
  return g;
}

// Which board a path names: its index in the rows table, WALL_OP_OWN_ROW for
// the master (which need not be in the table), -1 for none.
int boardOf(const Gathered& g, const String& id) {
  if (id.length() == 0) return -1;
  if (id == effectiveName) return WALL_OP_OWN_ROW;
  return wallRowsFind(g.wall.rows, id.c_str());
}

// A board's units and their verdicts. False for a row board whose units have
// not arrived since it was last welcomed.
struct BoardUnits {
  const UnitFacts* facts = nullptr;
  int count = 0;
  const WallVerdictBoard* verdicts = nullptr;
  uint32_t ageMs = 0;  // a row board's: since they arrived
};

bool unitsOf(Gathered& g, int board, BoardUnits& out) {
  if (board == WALL_OP_OWN_ROW) {
    out.facts = g.own.units;
    out.count = g.own.displayWidth;
    out.verdicts = g.judged ? g.verdicts.own() : nullptr;
    return true;
  }
  uint32_t atMs = 0;
  if (!wallStateRowUnits(board, g.rowUnits, atMs)) return false;
  out.facts = g.rowUnits.units;
  out.count = g.rowUnits.width;
  out.verdicts = g.judged ? g.verdicts.ofRow(board) : nullptr;
  out.ageMs = (uint32_t)(g.nowMs - atMs);
  return true;
}

const UnitVerdict* verdictOf(const BoardUnits& units, int index) {
  if (units.verdicts == nullptr || index >= units.verdicts->units) return nullptr;
  return &units.verdicts->unit[index];
}

void writeUnitsTable(JsonObject into, const BoardUnits& units) {
  JsonArray fields = into["fields"].to<JsonArray>();
  for (const char* field : UNIT_TABLE_FIELDS) fields.add(field);
  JsonArray rows = into["rows"].to<JsonArray>();
  for (int i = 0; i < units.count && i < UNITS_AMOUNT; i++) {
    unitApiTableRow(rows.add<JsonArray>(), units.facts[i], SFP_I2C_ADDRESS_BASE + i,
                    verdictOf(units, i));
  }
  const uint16_t supplyMin = unitFleetVccMin(units.facts, units.count);
  if (supplyMin != 0) into["supplyMinMv"] = supplyMin;
  WearAssessment wear;
  assessWear(units.facts, UNITS_AMOUNT, wear);
  if (wear.validCount > 0) into["turnsMedian"] = wear.median;
}

// The master itself: what only this board knows about its own running.
void writeMaster(JsonObject root, const Gathered& g) {
  root["kind"] = "master";
  root["address"] = WiFi.localIP().toString();
  root["rev"] = GIT_REV;
  root["showing"] = jsonCopied(g.own.currentText);
  root["mqttConnected"] = mqttIsConnected();
  std::unique_ptr<char[]> stats(new char[SYSTEM_STATS_JSON_CAP]);
  const size_t n = systemStatsJson(stats.get(), SYSTEM_STATS_JSON_CAP);
  if (n > 0 && n < SYSTEM_STATS_JSON_CAP) root["stats"] = serialized(String(stats.get()));
  root["starts"] = serialized(bootTraceJson());
  root["lastStart"]["reset"] = webResetReasonString();
  root["lastStart"]["cause"] = webBootRebootCause();
  root["network"] = serialized(netLivenessJson());
  root["crash"] = serialized(crashCtxReportJson());
  const RescueSlotFacts rescue = rescueSlotCurrent();
  JsonObject r = root["rescue"].to<JsonObject>();
  r["rev"] = jsonCopied(rescue.rev);
  r["state"] = rescueSlotStateLabel(rescue.state);
  r["warn"] = rescue.warn;
  char reflash[REFLASH_JSON_CAP];
  buildReflashJson(reflash, sizeof(reflash), g.own.reflash);
  root["unitUpdate"] = serialized(String(reflash));
  JsonObject settings = root["settings"].to<JsonObject>();
  WebStateLock lock;
  settings["name"] = liveSettings->deviceName;
  settings["unitCount"] = liveSettings->unitCountOverride;
  settings["updateUnitsAtStart"] = liveSettings->reflashOnBoot;
}

// "/api/v2/board/<a>/<b>" -> a and b; b is empty when there is none.
void pathTail(AsyncWebServerRequest* request, const char* prefix, String& first, String& second) {
  const String tail = request->url().substring(strlen(prefix));
  const int slash = tail.indexOf('/');
  first = slash < 0 ? tail : tail.substring(0, slash);
  second = slash < 0 ? String() : tail.substring(slash + 1);
}

void handleBoard(AsyncWebServerRequest* request) {
  String id, rest;
  pathTail(request, "/api/v2/board/", id, rest);
  std::unique_ptr<Gathered> g = gather();
  const int board = boardOf(*g, id);
  if (board == -1 || rest.length() > 0) return sendError(request, 404, "no such board on this wall");
  const bool own = board == WALL_OP_OWN_ROW;
  const int place = own ? wallRowsOwn(g->wall.rows) : board;
  AsyncJsonResponse* response = new AsyncJsonResponse();
  JsonObject root = response->getRoot().to<JsonObject>();
  root["id"] = id;
  root["own"] = own;
  if (place >= 0) {
    const WallRowDef& def = g->wall.rows.rows[place];
    root["row"] = def.row;
    root["col"] = def.col;
    root["width"] = def.width;
    char text[WALL_ROW_TEXT_MAX + 1];
    if (wallShowRowText(place, text, sizeof(text))) root["text"] = jsonCopied(text);
  }
  if (g->judged) writeVerdict(root, own ? g->verdicts.own() : g->verdicts.ofRow(board));
  if (own) {
    writeMaster(root, *g);
  } else {
    root["kind"] = "row";
    writeRowLink(root, g->wall.rows.rows[board], g->wall.link[board], g->nowMs);
    // The row boards share the master's choice.
    root["settings"]["updateUnitsAtStart"] = webUpdateUnitsAtStart();
  }
  root["jobRunning"] = wallJobRunningOn(board);
  BoardUnits units;
  if (unitsOf(*g, board, units)) {
    JsonObject table = root["units"].to<JsonObject>();
    writeUnitsTable(table, units);
    if (!own) table["msAgo"] = units.ageMs;
  }
  response->setLength();
  request->send(response);
}

void handleUnit(AsyncWebServerRequest* request) {
  String id, addressText;
  pathTail(request, "/api/v2/unit/", id, addressText);
  std::unique_ptr<Gathered> g = gather();
  const int board = boardOf(*g, id);
  if (board == -1) return sendError(request, 404, "no such board on this wall");
  char* end = nullptr;
  const long address = strtol(addressText.c_str(), &end, 10);
  BoardUnits units;
  if (!unitsOf(*g, board, units)) {
    return sendError(request, 503, "that row board has not sent its units yet");
  }
  const int index = (int)address - SFP_I2C_ADDRESS_BASE;
  if (addressText.length() == 0 || *end != 0 || index < 0 || index >= units.count ||
      index >= UNITS_AMOUNT) {
    return sendError(request, 404, "no such unit on that board");
  }
  AsyncJsonResponse* response = new AsyncJsonResponse();
  JsonObject root = response->getRoot().to<JsonObject>();
  root["board"] = id;
  root["position"] = index;
  unitApiDetail(root, units.facts[index], (int)address, g->nowMs, verdictOf(units, index));
  if (board != WALL_OP_OWN_ROW) root["msAgo"] = units.ageMs;
  response->setLength();
  request->send(response);
}

struct Tally {
  int total = 0, current = 0, outdated = 0, unknown = 0;
  int bootOk = 0, bootOutdated = 0, bootDamaged = 0, bootUnread = 0;

  void add(const UnitFacts& u) {
    if (u.state == 0) return;  // nothing at this place
    total++;
    if (!u.statusValid || u.fwStatus == 2) unknown++;
    else if (u.fwStatus == 0) current++;
    else outdated++;
    switch (u.bootVerdict) {
      case BOOT_INTEGRITY_OK: bootOk++; break;
      case BOOT_INTEGRITY_OUTDATED: bootOutdated++; break;
      case BOOT_INTEGRITY_CORRUPT: bootDamaged++; break;
      default: bootUnread++; break;
    }
  }
  void add(const Tally& t) {
    total += t.total; current += t.current; outdated += t.outdated; unknown += t.unknown;
    bootOk += t.bootOk; bootOutdated += t.bootOutdated; bootDamaged += t.bootDamaged;
    bootUnread += t.bootUnread;
  }
  void writeUnits(JsonObject into) const {
    into["total"] = total;
    into["current"] = current;
    into["outdated"] = outdated;
    into["unknown"] = unknown;
  }
  void writeBoot(JsonObject into) const {
    into["total"] = total;
    into["ok"] = bootOk;
    into["outdated"] = bootOutdated;
    into["damaged"] = bootDamaged;
    into["unread"] = bootUnread;
  }
};

void handleFirmware(AsyncWebServerRequest* request) {
  std::unique_ptr<Gathered> g = gather();
  AsyncJsonResponse* response = new AsyncJsonResponse();
  JsonObject root = response->getRoot().to<JsonObject>();
  root["master"]["id"] = effectiveName;
  root["master"]["rev"] = GIT_REV;
  const RescueSlotFacts rescue = rescueSlotCurrent();
  JsonObject r = root["rescue"].to<JsonObject>();
  r["rev"] = jsonCopied(rescue.rev);
  r["state"] = rescueSlotStateLabel(rescue.state);
  r["warn"] = rescue.warn;
  FollowerImageFacts image;
  const bool stored = followerImageFacts(image);
  if (stored) {
    JsonObject i = root["rowImage"].to<JsonObject>();
    i["rev"] = jsonCopied(image.rev);
    i["size"] = image.size;
    i["packed"] = image.packed;
  }
  root["update"]["phase"] = wallUpdatePhaseName((WallUpdatePhase)g->wall.updatePhase);
  if (g->wall.updateRow >= 0 && g->wall.updateRow < g->wall.rows.count) {
    root["update"]["row"] = jsonCopied(g->wall.rows.rows[g->wall.updateRow].id);
  }
  JsonArray boards = root["boards"].to<JsonArray>();
  Tally all;
  const auto addBoard = [&](const char* id, const char* kind, int board) {
    JsonObject b = boards.add<JsonObject>();
    b["id"] = jsonCopied(id);
    b["kind"] = kind;
    BoardUnits units;
    if (unitsOf(*g, board, units)) {
      Tally t;
      for (int i = 0; i < units.count && i < UNITS_AMOUNT; i++) t.add(units.facts[i]);
      t.writeUnits(b["units"].to<JsonObject>());
      t.writeBoot(b["bootloaders"].to<JsonObject>());
      all.add(t);
    }
    return b;
  };
  JsonObject master = addBoard(effectiveName.c_str(), "master", WALL_OP_OWN_ROW);
  master["rev"] = GIT_REV;
  master["current"] = true;
  for (int i = 0; i < g->wall.rows.count; i++) {
    const WallRowDef& def = g->wall.rows.rows[i];
    if (wallRowIsOwn(def)) continue;
    const WallRowLink& link = g->wall.link[i];
    JsonObject b = addBoard(def.id, "row", i);
    if (link.everWelcomed) {
      b["rev"] = jsonCopied(link.rev);
      b["rescue"] = link.rescue;
      // A row is current when it runs the image the master holds for it.
      if (stored) b["current"] = strcmp(link.rev, image.rev) == 0 && !link.rescue;
    }
    b["updateAttempts"] = link.updateAttempts;
    b["updateBlocked"] = link.updateBlocked;
  }
  JsonObject units = root["units"].to<JsonObject>();
  // What the master's own image carries; a row board judges its units
  // against the bundle in its own image.
  units["shouldBe"] = BUNDLED_UNIT_REV;
  all.writeUnits(units);
  JsonObject boot = root["bootloaders"].to<JsonObject>();
  char crc[9];
  snprintf(crc, sizeof(crc), "%08lx", (unsigned long)BOOT_CURRENT_CRC32);
  boot["shouldBe"] = jsonCopied(crc);
  all.writeBoot(boot);
  response->setLength();
  request->send(response);
}

void handleWallSettingsGet(AsyncWebServerRequest* request) {
  AsyncJsonResponse* response = new AsyncJsonResponse();
  {
    WebStateLock lock;
    wallSettingsWrite(response->getRoot().to<JsonObject>(), *liveSettings);
  }
  response->setLength();
  request->send(response);
}

void handleBoardSettingsGet(AsyncWebServerRequest* request) {
  String id, rest;
  pathTail(request, "/api/v2/settings/board/", id, rest);
  uint32_t generation = 0;
  const WallRowsTable table = wallStateRows(generation);
  const bool master = id == effectiveName;
  if (rest.length() > 0 || id.length() == 0 || (!master && wallRowsFind(table, id.c_str()) < 0)) {
    return sendError(request, 404, "no such board on this wall");
  }
  AsyncJsonResponse* response = new AsyncJsonResponse();
  JsonObject root = response->getRoot().to<JsonObject>();
  if (master) {
    WebStateLock lock;
    boardSettingsWrite(root, *liveSettings);
  }
  response->setLength();
  request->send(response);
}

// Stages a settings post that carries no text: no display gate applies.
void stageSettings(AsyncWebServerRequest* request, const PendingSettingsPost& post) {
  bool needsReboot = false;
  bool deviceNameChanged = false;
  if (webStagePost(post, needsReboot, deviceNameChanged) != WebStage::Staged) {
    return sendError(request, 503, "the settings could not be staged, try again in a moment");
  }
  AsyncJsonResponse* response = new AsyncJsonResponse();
  response->getRoot()["done"] = true;
  response->getRoot()["restart"] = needsReboot;
  response->setLength();
  request->send(response);
}

void sendRefusedKey(AsyncWebServerRequest* request, const WallSettingsRefused& refused) {
  if (refused.key[0] == 0) return sendError(request, 400, "the body is a JSON object of settings");
  char message[80];
  snprintf(message, sizeof(message), "\"%s\" is no setting here, or its value is not allowed",
           refused.key);
  AsyncJsonResponse* response = new AsyncJsonResponse();
  response->getRoot()["error"] = String(message);
  response->getRoot()["key"] = String(refused.key);
  response->setCode(400);
  response->setLength();
  request->send(response);
}

void handleWallSettingsPut(AsyncWebServerRequest* request, JsonVariant& json) {
  PendingSettingsPost post;
  WallSettingsRefused refused;
  if (!wallSettingsBuild(json, post, refused)) return sendRefusedKey(request, refused);
  stageSettings(request, post);
}

void handleBoardSettingsPut(AsyncWebServerRequest* request, JsonVariant& json) {
  String id, rest;
  pathTail(request, "/api/v2/settings/board/", id, rest);
  if (rest.length() > 0 || id != effectiveName) {
    uint32_t generation = 0;
    const WallRowsTable table = wallStateRows(generation);
    if (rest.length() == 0 && id.length() > 0 && wallRowsFind(table, id.c_str()) >= 0) {
      return sendError(request, 409, "a row board has no settings of its own");
    }
    return sendError(request, 404, "no such board on this wall");
  }
  PendingSettingsPost post;
  WallSettingsRefused refused;
  if (!boardSettingsBuild(json, post, refused)) return sendRefusedKey(request, refused);
  stageSettings(request, post);
}

void sendLog(AsyncWebServerRequest* request, AsyncWebServerResponse* response, const String& board,
             const char* kind) {
  response->addHeader("X-Log-Board", board);
  response->addHeader("X-Log-Kind", kind);
  request->send(response);
}

void handleLog(AsyncWebServerRequest* request) {
  const String id = request->hasParam("row") ? request->getParam("row")->value() : String();
  const String kind = request->hasParam("kind") ? request->getParam("kind")->value() : "ram";
  if (kind != "ram" && kind != "flash") {
    return sendError(request, 400, "kind is \"ram\" or \"flash\"");
  }
  if (id.length() == 0 || id == effectiveName) {
    if (kind == "ram") {
      return sendLog(request, request->beginResponse(200, "text/plain", webLogRead()),
                     effectiveName, "ram");
    }
    if (!flashLogAvailable()) return sendError(request, 503, "the storage is not mounted");
    const char* path =
        request->hasParam("prev") ? flashLogPreviousPath() : flashLogCurrentPath();
    // A rotation between this check and the response's own open ends in a
    // 404 from the file response: read again.
    if (!LittleFS.exists(path)) return sendError(request, 404, "no such log file yet");
    return sendLog(request, request->beginResponse(LittleFS, path, "text/plain"), effectiveName,
                   "flash");
  }
  uint32_t generation = 0;
  const WallRowsTable table = wallStateRows(generation);
  const int row = wallRowsFind(table, id.c_str());
  if (row < 0) return sendError(request, 404, "no such board on this wall");
  if (kind != "ram") return sendError(request, 400, "a row board keeps no flash log");
  String text;
  if (!rowLogRead(row, generation, text)) {
    return sendError(request, 503, "that row's log cannot be held right now, ask again");
  }
  sendLog(request, request->beginResponse(200, "text/plain", text), id, "ram");
}

constexpr size_t SETTINGS_BODY_MAX = 1024;

}  // namespace

void webBoardRegister(AsyncWebServer& server) {
  AsyncCallbackJsonWebHandler* wallPut =
      new AsyncCallbackJsonWebHandler("/api/v2/settings/wall", handleWallSettingsPut);
  wallPut->setMethod(HTTP_PUT);
  wallPut->setMaxContentLength(SETTINGS_BODY_MAX);
  server.addHandler(wallPut);
  AsyncCallbackJsonWebHandler* boardPut =
      new AsyncCallbackJsonWebHandler("/api/v2/settings/board", handleBoardSettingsPut);
  boardPut->setMethod(HTTP_PUT);
  boardPut->setMaxContentLength(SETTINGS_BODY_MAX);
  server.addHandler(boardPut);
  server.on("/api/v2/log", HTTP_GET, handleLog);
  server.on("/api/v2/settings/wall", HTTP_GET, handleWallSettingsGet);
  server.on("/api/v2/settings/board", HTTP_GET, handleBoardSettingsGet);
  server.on("/api/v2/board", HTTP_GET, handleBoard);
  server.on("/api/v2/unit", HTTP_GET, handleUnit);
  server.on("/api/v2/firmware", HTTP_GET, handleFirmware);
}
