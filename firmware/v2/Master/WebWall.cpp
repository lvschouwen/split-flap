// WebWall.cpp — the first routes of the operator API that covers the whole
// Split-Flap (#559/#566): POST /api/v2/action starts a job and answers with
// its id, GET /api/v2/op/{id} says what became of it, GET /api/v2/wall lists
// the boards with the master's verdict on each (WallWatch.h). Async context: handlers read WallState snapshots and stage
// requests, they never change anything themselves (WebEndpoints.cpp rules).
//
// Which boards make up the wall:
//   {"name":"pair","target":{"host":"192.168.1.50"},"args":{"row":1,"col":0,"width":5}}
//       args are optional: below the last row, left edge, as wide as the row says;
//       target.port is the row's web port when it is not 80 (a bench stand-in)
//   {"name":"release","target":{"row":"<row id>"}}
//   {"name":"arrange","args":{"rows":[{"id":"","row":0,"col":0,"width":16}, ...]}}
//       every board of the table once, "" = the master's own row
//   {"name":"update","target":{"row":"<row id>"}}
//       offer the row the stored image again, whatever was held against it
//   {"name":"find-rows"}
//       search the network for row boards that can be paired (WallFind.h);
//       the list is the job's "data". It changes nothing and leaves no entry
//       in the event record
// Unit jobs, the same call for the master's own units and a row board's
// (names and values: WallJobs.h):
//   {"name":"home","target":{"row":"<row id>","unit":3}}
//   {"name":"jog","target":{"unit":3},"args":{"steps":-4}}
//       target.row "" or absent = the master's own row; one job at a time per row
// What the wall shows (WallActions.h), done when accepted, 200 {"done":true}:
//   {"name":"show","args":{"text":"HELLO\nWORLD"}}      and "forS":300 for a time
//   {"name":"mode","args":{"mode":"clock"}}
//   {"name":"quiet","args":{"on":true}}
//   {"name":"stop"}                                      abort and blank every row
//   {"name":"restart","target":{"row":"<row id>"}}       "" or absent = this master
//   {"name":"forget-wifi"}
//       this master forgets its WiFi and restarts into its setup network
// GET /api/v2/history[?before=<seq>][&limit=<n>] reads the event record newest
// first: {"events":[{seq,time,kind,detail,board,unit,a,b}...],"next":<seq>}.
// The reason, job or restart cause an entry is about is also given by name.
// "next" is the `before` of the following page and is absent on the last one;
// "time" is Unix seconds, absent when the clock was not set; "board" is the
// row's id, "" for the master, absent for a board no longer on the wall.
// Answers: 202 {"op":N} for a job, 200 {"done":true} for what is done when
// accepted; 400 with the reason; 409 while another such request runs, the row
// cannot take it, the wall is quiet, or a unit update is running; 503 when
// no job can be started or the display queue is full.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPmDNS.h>

#include <memory>

#include "BootDump.h"
#include "BuildVersion.h"
#include "EventRecord.h"
#include "FollowerImageStore.h"
#include "HelpersSerialHandling.h"
#include "JsonCopied.h"
#include "ReflashPlan.h"
#include "Tasks.h"
#include "WallActions.h"
#include "WallFind.h"
#include "WallJobs.h"
#include "WallRowEvents.h"
#include "WallShow.h"
#include "WallState.h"
#include "WallUpdatePolicy.h"
#include "WallWatch.h"
#include "WebEndpoints.h"
#include "WebEndpointsInternal.h"
#include "WebWallJson.h"
#include "WifiService.h"

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

void sendOp(AsyncWebServerRequest* request, uint32_t opId) {
  AsyncJsonResponse* response = new AsyncJsonResponse();
  response->getRoot()["op"] = opId;
  response->setCode(202);
  response->setLength();
  request->send(response);
}

// The DisplayCommand of a job on the master's own units. Probe has no seq:
// the display reports nothing back for it.
DisplayCommand ownCommand(const wl_Op& op, uint32_t seq) {
  const uint8_t unit = (uint8_t)op.address;
  switch (op.opcode) {
    case wl_OpCode_OPC_HOME: return makeHomeCommand(seq, unit);
    case wl_OpCode_OPC_IDENTIFY: return makeIdentifyCommand(seq, unit);
    case wl_OpCode_OPC_JOG: return makeJogCommand(seq, unit, (int)op.arg);
    case wl_OpCode_OPC_SET_OFFSET: return makeWriteOffsetCommand(seq, unit, (int16_t)op.arg);
    case wl_OpCode_OPC_SELF_TEST: return makeSelfTestCommand(seq, unit);
    case wl_OpCode_OPC_RESTART_UNIT: return makeRebootToBootloaderCommand(seq, unit);
    case wl_OpCode_OPC_RESET_ODOMETER: return makeResetOdometerCommand(seq, unit);
    case wl_OpCode_OPC_SET_GATES: return makeSetGatesCommand(seq, unit, (uint8_t)op.arg);
    case wl_OpCode_OPC_BOOT_INFO: return makeBootInfoCommand(seq, unit);
    case wl_OpCode_OPC_BOOT_DUMP: return makeBootDumpCommand(seq, unit);
    case wl_OpCode_OPC_BOOT_UPDATE: return makeBootUpdateCommand(seq, unit);
    case wl_OpCode_OPC_UPDATE_UNITS: {
      // The job shows the present text again when it is done.
      const WebContentSnapshot content = webDisplayContentSnapshot();
      return makeReflashUnitsCommand(seq, String(displaySnapshotGet().currentText),
                                     content.alignment, content.flapSpeed, unit, op.arg != 0);
    }
    case wl_OpCode_OPC_SET_ADDRESS: return makeSetAddressCommand(seq, unit, (uint8_t)op.arg);
    case wl_OpCode_OPC_CLEAR_ADDRESS: return makeClearAddressCommand(seq, unit);
    case wl_OpCode_OPC_HOME_ALL: {
      // The row shows the present text again once every unit is home.
      const WebContentSnapshot content = webDisplayContentSnapshot();
      return makeResetUnitsCommand(seq, String(displaySnapshotGet().currentText),
                                   content.alignment, content.flapSpeed);
    }
    default: return makeProbeCommand();
  }
}

// Which of its units a job may address is the board's to say: here, for the
// master's own. The checks are the ones the /unit/... routes make.
const char* checkOwnUnit(const WallJobKind& kind, const wl_Op& op, int& status) {
  status = 400;
  if (op.address == 0 || kind.opcode == wl_OpCode_OPC_RESTART_UNIT) return nullptr;
  if (kind.opcode == wl_OpCode_OPC_UPDATE_UNITS) {
    // No running-unit check: a unit on another protocol is updated to get it back.
    return reflashAddressInRange((long)op.address, SFP_I2C_ADDRESS_BASE, UNITS_AMOUNT)
               ? nullptr
               : "target.unit is not an address this row can hold";
  }
  std::unique_ptr<DisplaySnapshot> own(new DisplaySnapshot(displaySnapshotGet()));
  char raw[12];
  snprintf(raw, sizeof(raw), "%lu", (unsigned long)op.address);
  int parsed = 0;
  MaintVerdict verdict = maintValidateAddress(raw, own->units, UNITS_AMOUNT, parsed);
  if (verdict.httpStatus == 200 && kind.opcode == wl_OpCode_OPC_SET_ADDRESS) {
    // The display task judges it again against the units of that moment.
    verdict = maintValidateSetAddressTarget((long)op.arg, parsed, own->units, UNITS_AMOUNT);
  }
  if (verdict.httpStatus == 200) return nullptr;
  status = verdict.httpStatus;
  return verdict.message;
}

void startOwnJob(AsyncWebServerRequest* request, const WallJobKind& kind, wl_Op& op) {
  int status = 400;
  if (const char* refusal = checkOwnUnit(kind, op, status)) {
    return sendError(request, status, refusal);
  }
  bool rowBusy = false;
  op.op_id = wallJobBegin(kind.name, WALL_OP_OWN_ROW, rowBusy, (uint8_t)op.address);
  if (op.op_id == 0) {
    return rowBusy ? sendError(request, 409, "another unit job is running on that row")
                   : sendError(request, 503, "too many jobs are running");
  }
  const bool reported = kind.opcode != wl_OpCode_OPC_PROBE;
  const uint32_t seq = reported ? displayNextMaintSeq() : 0;
  if (!displayEnqueue(ownCommand(op, seq))) {
    wallOpFinish(op.op_id, false, "the display queue was full");
    return sendError(request, 503, "the display queue is full, try again in a moment");
  }
  if (reported) {
    WallOwnJob job;
    job.opId = op.op_id;
    job.seq = seq;
    job.startedMs = millis();
    job.op = op;
    wallOwnJobSet(job);
  } else {
    wallOpFinish(op.op_id, true, "rescan asked for");
  }
  sendOp(request, op.op_id);
}

void startRowJob(AsyncWebServerRequest* request, const WallJobKind& kind, wl_Op& op, int row,
                 uint32_t generation) {
  {
    std::unique_ptr<WallSnapshot> wall(new WallSnapshot(wallStateGet()));
    const WallRowLink& link = wall->link[row];
    if (!link.contact.connected || !link.contact.helloSeen) {
      return sendError(request, 409, "that row is not connected");
    }
    if (link.rescue) {
      return sendError(request, 409, wallJobRefusalText(wl_OpRefusal_REFUSAL_RESCUE));
    }
    if (wall->updatePhase != (uint8_t)WallUpdatePhase::Idle && wall->updateRow == row) {
      return sendError(request, 409, "that row is taking a firmware update");
    }
  }
  bool rowBusy = false;
  op.op_id = wallJobBegin(kind.name, row, rowBusy, (uint8_t)op.address);
  if (op.op_id == 0) {
    return rowBusy ? sendError(request, 409, "another unit job is running on that row")
                   : sendError(request, 503, "too many jobs are running");
  }
  if (!wallJobStage(row, op, generation)) {
    wallOpFinish(op.op_id, false, "the row could not be handed the job");
    return sendError(request, 409, "the row cannot be handed a job right now");
  }
  sendOp(request, op.op_id);
}

void handleJob(AsyncWebServerRequest* request, JsonVariantConst body, const WallJobKind& kind,
               const WallRowsTable& table, uint32_t generation) {
  JsonVariantConst unit = body["target"]["unit"];
  if (!unit.isNull() && !unit.is<long>()) {
    return sendError(request, 400, "target.unit is the unit's address, a whole number");
  }
  JsonVariantConst arg;
  if (kind.arg != nullptr) arg = body["args"][kind.arg];
  const bool haveArg = !arg.isNull();
  if (haveArg && !arg.is<long>() && !arg.is<bool>()) {
    return sendError(request, 400, "the job's value under args is a whole number");
  }
  // "No args" is judged by the job: any other key under args is not for it.
  const bool strayArgs = kind.arg == nullptr && body["args"].as<JsonObjectConst>().size() > 0;
  const long value = arg.is<bool>() ? (arg.as<bool>() ? 1 : 0) : arg.as<long>();
  wl_Op op;
  if (const char* refusal = wallJobBuild(kind, !unit.isNull(), unit.as<long>(),
                                         haveArg || strayArgs, value, op)) {
    return sendError(request, 400, refusal);
  }
  JsonVariantConst rowId = body["target"]["row"];
  if (!rowId.isNull() && !rowId.is<const char*>()) {
    return sendError(request, 400, "target.row is a row's id (\"\" = the master's own row)");
  }
  const char* id = rowId.isNull() ? "" : rowId.as<const char*>();
  const int row = id[0] == 0 ? -1 : wallRowsFind(table, id);
  if (id[0] != 0 && row < 0) return sendError(request, 400, "target.row is not a row of this wall");
  // The producer gate of a unit update (#205), on whichever row it runs:
  // nothing else touches the units meanwhile.
  if (reflashInProgress(displaySnapshotGet().reflash) || wallUnitUpdateRunning()) {
    return sendError(request, 409, "a unit update is running, retry when it has finished");
  }
  if (row < 0) return startOwnJob(request, kind, op);
  startRowJob(request, kind, op, row, generation);
}

// Not a change to the table and not a unit job: the link task is told, and
// the row is offered the image at its next turn (GET /api/v2/wall shows it).
void handleUpdateRetry(AsyncWebServerRequest* request, JsonVariantConst body,
                       const WallRowsTable& table, uint32_t generation) {
  const char* id = body["target"]["row"].as<const char*>();
  const int row = id == nullptr || id[0] == 0 ? -1 : wallRowsFind(table, id);
  if (row < 0) return sendError(request, 400, "target.row is not a row board of this wall");
  if (!followerImageStored()) return sendError(request, 409, "no row image is stored");
  const uint32_t op = wallOpBegin("update", row);
  if (op == 0) return sendError(request, 503, "too many jobs are running");
  if (!wallStateAskUpdateRetry(row, generation)) {
    wallOpFinish(op, false, "the wall changed meanwhile");
    return sendError(request, 409, "the wall changed meanwhile, ask again");
  }
  wallOpFinish(op, true, "the row is offered the stored image again");
  sendOp(request, op);
}

void sendDone(AsyncWebServerRequest* request) {
  AsyncJsonResponse* response = new AsyncJsonResponse();
  response->getRoot()["done"] = true;
  response->setLength();
  request->send(response);
}

void handleContent(AsyncWebServerRequest* request, const char* name, JsonVariantConst body) {
  PendingSettingsPost post;
  if (const char* refusal = wallActionBuild(name, body["args"], post)) {
    return sendError(request, 400, refusal);
  }
  // Text for a quiet wall is dropped when it is drained: say so here rather
  // than accept what will not be shown.
  if ((post.inputTextProvided || post.transientTextProvided) && webQuietNow()) {
    return sendError(request, 409, "the wall is quiet: turn quiet off to show text");
  }
  bool needsReboot = false;
  bool deviceNameChanged = false;
  switch (webStagePost(post, needsReboot, deviceNameChanged)) {
    case WebStage::Staged: return sendDone(request);
    case WebStage::UnitUpdate:
      return sendError(request, 409, "a unit update is running, retry when it has finished");
    case WebStage::QueueFull:
      return sendError(request, 503, "the display queue is full, try again in a moment");
  }
}

void handleStop(AsyncWebServerRequest* request) {
  SerialPrintln(F("Stop asked for through the API"));
  uint32_t seq = 0;
  if (!webStopWall(seq)) {
    return sendError(request, 503, "the display queue is full, try again in a moment");
  }
  sendDone(request);
}

// The master restarts itself; a row board is told to over the link.
void handleRestart(AsyncWebServerRequest* request, JsonVariantConst body,
                   const WallRowsTable& table, uint32_t generation) {
  // Only a request that plainly means the master restarts it: no target at
  // all, or target.row "". A target that cannot be read is refused, never
  // taken for "this board".
  JsonVariantConst target = body["target"];
  JsonVariantConst rowId = target["row"];
  if (!target.isNull() && !rowId.is<const char*>()) {
    return sendError(request, 400,
                     "target is {\"row\":\"<row id>\"} (\"\" or no target = this master)");
  }
  const char* id = target.isNull() ? "" : rowId.as<const char*>();
  if (id[0] == 0) {
    if (const char* refusal = webStageReboot("restart asked for through the API")) {
      return sendError(request, 409, refusal);
    }
    SerialPrintln(F("Restart asked for through the API"));
    return sendDone(request);
  }
  const int row = wallRowsFind(table, id);
  if (row < 0) return sendError(request, 400, "target.row is not a row of this wall");
  {
    std::unique_ptr<WallSnapshot> wall(new WallSnapshot(wallStateGet()));
    const WallRowLink& link = wall->link[row];
    if (!link.contact.connected || !link.contact.helloSeen) {
      return sendError(request, 409, "that row is not connected");
    }
    if (wall->updatePhase != (uint8_t)WallUpdatePhase::Idle && wall->updateRow == row) {
      return sendError(request, 409, "that row is taking a firmware update");
    }
  }
  // A restart in the middle of a unit job leaves a unit half done.
  if (wallJobRunningOn(row)) {
    return sendError(request, 409, "a unit job is running on that row");
  }
  if (!wallStateAskRestart(row, generation)) {
    return sendError(request, 409, "the wall changed meanwhile, ask again");
  }
  sendDone(request);
}

// The job of a search for row boards that was asked for and has not run yet,
// 0 = none. Under the web state lock; netTask runs the search.
uint32_t findRowsOp = 0;

void handleFindRows(AsyncWebServerRequest* request) {
  uint32_t op = 0;
  bool looking;
  {
    WebStateLock lock;
    looking = findRowsOp != 0;
    if (!looking) op = findRowsOp = wallOpBegin("find-rows", -1);
  }
  if (looking) return sendError(request, 409, "a search for row boards is still running");
  if (op == 0) return sendError(request, 503, "too many jobs are running");
  sendOp(request, op);
}

// It ends in a restart, so it waits for what a restart waits for.
void handleForgetWifi(AsyncWebServerRequest* request) {
  if (const char* refusal = webRestartRefusal()) return sendError(request, 409, refusal);
  SerialPrintln(F("Forgetting the WiFi asked for through the API"));
  sendDone(request);
  wifiStageReset();
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
  // Before the web state lock: a job needs none of what it guards, and
  // building an own-row unit update takes that lock itself.
  if (const WallJobKind* job = wallJobFind(name)) {
    return handleJob(request, body, *job, table, generation);
  }
  if (strcmp(name, "update") == 0) return handleUpdateRetry(request, body, table, generation);
  if (wallActionIsContent(name)) return handleContent(request, name, body);
  if (strcmp(name, "stop") == 0) return handleStop(request);
  if (strcmp(name, "restart") == 0) return handleRestart(request, body, table, generation);
  if (strcmp(name, "find-rows") == 0) return handleFindRows(request);
  if (strcmp(name, "forget-wifi") == 0) return handleForgetWifi(request);
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
  sendOp(request, staged.opId);
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
  // What the job handed back: a JSON object as the board wrote it, the bytes
  // of a boot section, or the row boards a search found.
  const WallJobKind* kind = wallJobFind(op.name);
  const WallJobData handed = strcmp(op.name, "find-rows") == 0 ? WallJobData::Json
                             : kind != nullptr                 ? kind->data
                                                               : WallJobData::None;
  if (handed != WallJobData::None && op.phase != WallOpPhase::Running) {
    std::unique_ptr<uint8_t[]> data(new uint8_t[WALL_OP_DATA_MAX]);
    const size_t n = wallOpDataGet(op.id, data.get(), WALL_OP_DATA_MAX);
    if (n > 0 && handed == WallJobData::Json) {
      String text;
      text.concat((const char*)data.get(), n);
      JsonDocument parsed;
      // Embedded only when it reads as JSON: a cut document must not break this answer.
      if (deserializeJson(parsed, text) == DeserializationError::Ok) root["data"] = parsed;
    } else if (n > 0) {
      static const char digits[] = "0123456789abcdef";
      String hex;
      hex.reserve(2 * n);
      for (size_t i = 0; i < n; i++) {
        hex += digits[data[i] >> 4];
        hex += digits[data[i] & 0x0F];
      }
      char crc[9];
      snprintf(crc, sizeof(crc), "%08lx", (unsigned long)bootDumpCrc32(data.get(), n));
      JsonObject bytes = root["data"].to<JsonObject>();
      bytes["len"] = n;
      bytes["crc32"] = jsonCopied(crc);
      bytes["hex"] = hex;
    }
  }
  response->setCode(op.phase == WallOpPhase::Running ? 202 : 200);
  response->setLength();
  request->send(response);
}

void handleWall(AsyncWebServerRequest* request) {
  // Heap, not this task's stack: the snapshot and one row's unit facts.
  const uint32_t generation = wallStateRowsGeneration();
  std::unique_ptr<WallSnapshot> wall(new WallSnapshot(wallStateGet()));
  // Verdicts of the same rows table, or none: a row's number must mean the
  // same board in both.
  std::unique_ptr<WallVerdicts> verdicts(new WallVerdicts);
  const bool judged = wallVerdictsGet(*verdicts, generation) &&
                      generation == wallStateRowsGeneration();
  std::unique_ptr<UnitFactsDoc> units(new UnitFactsDoc);
  const DisplaySnapshot own = displaySnapshotGet();
  const uint32_t nowMs = millis();
  AsyncJsonResponse* response = new AsyncJsonResponse();
  JsonVariant root = response->getRoot();
  root["master"]["id"] = effectiveName;
  root["master"]["rev"] = GIT_REV;
  root["master"]["units"] = own.displayWidth;
  if (judged) {
    root["verdict"] = verdictLevelName(verdicts->wall);
    writeVerdict(root["master"], verdicts->own());
  }
  // The image the rows are to run, and the offer of it that is out now.
  FollowerImageFacts image;
  if (followerImageFacts(image)) {
    JsonObject stored = root["rowImage"].to<JsonObject>();
    stored["rev"] = jsonCopied(image.rev);
    stored["size"] = image.size;
    stored["packed"] = image.packed;
  }
  root["update"]["phase"] = wallUpdatePhaseName((WallUpdatePhase)wall->updatePhase);
  if (wall->updateRow >= 0 && wall->updateRow < wall->rows.count) {
    root["update"]["row"] = jsonCopied(wall->rows.rows[wall->updateRow].id);
  }
  JsonArray rows = root["rows"].to<JsonArray>();
  if (wall->rows.count == 0 && own.displayWidth > 0) {
    // A master on its own keeps no table: its units are the wall's one row
    // (as the stream's "wall" topic has it).
    JsonObject row = rows.add<JsonObject>();
    row["id"] = "";
    row["own"] = true;
    row["row"] = 0;
    row["col"] = 0;
    row["width"] = own.displayWidth;
    row["text"] = jsonCopied(own.currentText);
    if (judged) writeVerdict(row, verdicts->own());
    row["showing"] = jsonCopied(own.currentText);
  }
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
    if (judged) writeVerdict(row, verdicts->ofRow(i));
    if (wallRowIsOwn(def)) {
      row["showing"] = jsonCopied(own.currentText);
      continue;
    }
    const WallRowLink& link = wall->link[i];
    writeRowLink(row, def, link, nowMs);
    if (!link.everWelcomed) continue;
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

void handleHistory(AsyncWebServerRequest* request) {
  if (!eventRecordAvailable()) return sendError(request, 503, "the storage is not mounted");
  uint32_t before = 0;
  int limit = EVENT_PAGE_MAX;
  if (request->hasParam("before")) {
    before = (uint32_t)strtoul(request->getParam("before")->value().c_str(), nullptr, 10);
  }
  if (request->hasParam("limit")) {
    const long asked = strtol(request->getParam("limit")->value().c_str(), nullptr, 10);
    if (asked < 1 || asked > EVENT_PAGE_MAX) {
      return sendError(request, 400, "limit is 1 to 50");
    }
    limit = (int)asked;
  }
  // One more than asked for says whether a further page exists.
  std::unique_ptr<EventRecord[]> page(new EventRecord[EVENT_PAGE_MAX + 1]);
  const int found = eventRecordPage(before, page.get(), limit + 1);
  const int count = found > limit ? limit : found;
  uint32_t generation = 0;
  std::unique_ptr<WallRowsTable> table(new WallRowsTable(wallStateRows(generation)));
  AsyncJsonResponse* response = new AsyncJsonResponse();
  JsonVariant root = response->getRoot();
  JsonArray events = root["events"].to<JsonArray>();
  for (int i = 0; i < count; i++) {
    const EventRecord& r = page[i];
    JsonObject e = events.add<JsonObject>();
    e["seq"] = r.seq;
    if (r.timeS != 0) e["time"] = r.timeS;
    e["kind"] = eventKindName(r.kind);
    e["detail"] = r.detail;
    if (r.board == 0) {
      e["board"] = "";
    } else {
      for (int row = 0; row < table->count; row++) {
        if (eventBoardKey(table->rows[row].id) == r.board) {
          e["board"] = jsonCopied(table->rows[row].id);
          break;
        }
      }
    }
    e["unit"] = r.unit;
    e["a"] = r.a;
    e["b"] = r.b;
    // What the numbers stand for, by name, so the record reads without a table.
    switch ((EventKind)r.kind) {
      case EventKind::UnitReasonOn:
      case EventKind::UnitReasonOff:
        e["reason"] = unitReasonName((UnitReason)r.detail);
        break;
      case EventKind::BoardReasonOn:
      case EventKind::BoardReasonOff:
        e["reason"] = boardReasonName((BoardReason)r.detail);
        break;
      case EventKind::JobDone:
      case EventKind::JobFailed:
        e["job"] = wallJobNumberName(r.detail);
        break;
      case EventKind::UnitRestarted:
        e["cause"] = unitResetKindName(unitResetFromStatusByte(r.detail));
        break;
      case EventKind::RowEvent:
        e["event"] = wallRowEventName(r.detail);
        break;
      default:
        break;
    }
  }
  if (found > limit) root["next"] = page[count - 1].seq;
  response->setLength();
  request->send(response);
}

}  // namespace

// The search a find-rows job asked for. In netTask: the query blocks for a
// few seconds and takes LWIP locks, which must never nest inside the web
// state lock (as webSettingsDiscoverLoop).
void webWallFindRowsLoop() {
  uint32_t op;
  {
    WebStateLock lock;
    op = findRowsOp;
  }
  if (op == 0) return;
  uint32_t generation;
  const WallRowsTable table = wallStateRows(generation);
  std::unique_ptr<WallFound> found(new WallFound);
  const int answers = MDNS.queryService("splitflap", "tcp");
  for (int i = 0; i < answers; i++) {
    WallFoundBoard board;
    board.id = MDNS.txt(i, "name");
    const IPAddress address = MDNS.address(i);
    board.address = address == IPAddress() ? String() : address.toString();
    board.rev = MDNS.txt(i, "rev");
    board.plat = MDNS.txt(i, "plat");
    board.units = (int)MDNS.txt(i, "width").toInt();
    wallFoundAdd(*found, board, table);
  }
  static_assert(WALL_FIND_JSON_MAX <= WALL_OP_DATA_MAX, "a full list must fit a job's result");
  std::unique_ptr<char[]> json(new char[WALL_FIND_JSON_MAX]);
  const size_t length = wallFoundJson(*found, json.get(), WALL_FIND_JSON_MAX);
  SerialPrintf("Row search: %d answer(s), %u to pair\n", answers, (unsigned)found->count);
  if (length > 0) {
    wallOpDataPut(op, 0, (const uint8_t*)json.get(), length);
    char detail[24];
    snprintf(detail, sizeof(detail), "%u found", (unsigned)found->count);
    wallOpFinish(op, true, detail);
  } else {
    wallOpFinish(op, false, "the list did not fit");
  }
  WebStateLock lock;
  findRowsOp = 0;
}

void webWallRegister(AsyncWebServer& server) {
  AsyncCallbackJsonWebHandler* action =
      new AsyncCallbackJsonWebHandler("/api/v2/action", handleAction);
  action->setMethod(HTTP_POST);
  action->setMaxContentLength(ACTION_BODY_MAX);
  server.addHandler(action);
  server.on("/api/v2/op", HTTP_GET, handleOp);
  server.on("/api/v2/wall", HTTP_GET, handleWall);
  server.on("/api/v2/history", HTTP_GET, handleHistory);
}
