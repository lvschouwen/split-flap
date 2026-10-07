// WebStream.cpp — GET /api/v2/stream (#559/#572): server-sent events for a
// page that follows the wall without asking over and over. What is sent and
// when is StreamPolicy.h. netTask builds the documents from snapshot copies
// four times a second while anyone is reading, and sends the ones that
// changed; the event source queues per reader.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <atomic>
#include <memory>

#include "EventRecord.h"
#include "JsonCopied.h"
#include "StreamPolicy.h"
#include "Tasks.h"
#include "WallJobs.h"
#include "WallShow.h"
#include "WallState.h"
#include "WallWatch.h"
#include "WebEndpoints.h"
#include "WebEndpointsInternal.h"

namespace {

AsyncEventSource stream("/api/v2/stream");
// Set by a connecting reader (web server task), taken by netTask.
std::atomic<bool> readerJoined{false};

constexpr uint32_t STREAM_CHECK_MS = 250;

void sendIfDue(StreamTracker& tracker, StreamTopic topic, JsonDocument& doc) {
  String text;
  serializeJson(doc, text);
  if (tracker.due(topic, text.c_str())) {
    stream.send(text.c_str(), streamTopicName(topic), millis());
  }
}

void buildWall(JsonDocument& doc, const WallSnapshot& wall, const DisplaySnapshot& own) {
  const WebContentSnapshot content = webDisplayContentSnapshot();
  doc["mode"] = content.deviceMode;
  doc["quiet"] = tasksQuiet();
  JsonArray rows = doc["rows"].to<JsonArray>();
  if (wall.rows.count == 0) {
    // A master on its own: its row is the wall.
    JsonObject row = rows.add<JsonObject>();
    row["id"] = "";
    row["text"] = jsonCopied(own.currentText);
    return;
  }
  for (int i = 0; i < wall.rows.count; i++) {
    JsonObject row = rows.add<JsonObject>();
    row["id"] = jsonCopied(wall.rows.rows[i].id);
    char text[WALL_ROW_TEXT_MAX + 1];
    if (wallShowRowText(i, text, sizeof(text))) row["text"] = jsonCopied(text);
  }
}

void buildVerdict(JsonDocument& doc, const WallSnapshot& wall, const WallVerdicts& verdicts) {
  doc["wall"] = verdictLevelName(verdicts.wall);
  JsonArray boards = doc["boards"].to<JsonArray>();
  for (int i = 0; i < verdicts.count; i++) {
    const WallVerdictBoard& b = verdicts.boards[i];
    JsonObject board = boards.add<JsonObject>();
    const bool placed = b.row >= 0 && b.row < wall.rows.count;
    board["id"] = placed ? jsonCopied(wall.rows.rows[b.row].id) : "";
    board["level"] = verdictLevelName(b.verdict.level);
    board["reason"] = boardReasonName(b.verdict.reason);
    char levels[UNITS_AMOUNT + 1] = {0};
    for (int u = 0; u < b.units && u < UNITS_AMOUNT; u++) {
      levels[u] = verdictLevelName(b.unit[u].level)[0];
    }
    board["unitLevels"] = jsonCopied(levels);
  }
}

void buildJobs(JsonDocument& doc, const WallSnapshot& wall) {
  static WallOp ops[WALL_OPS_KEPT];  // netTask only; not on its stack
  const int n = wallOpsCopy(ops);
  JsonArray jobs = doc.to<JsonArray>();
  for (int i = 0; i < n; i++) {
    const WallOp& op = ops[i];
    JsonObject job = jobs.add<JsonObject>();
    job["op"] = op.id;
    job["name"] = jsonCopied(op.name);
    job["state"] = op.phase == WallOpPhase::Running ? "running"
                   : op.phase == WallOpPhase::Done  ? "done"
                                                    : "failed";
    if (op.row >= 0 && op.row < wall.rows.count) {
      job["board"] = jsonCopied(wall.rows.rows[op.row].id);
    } else if (op.row == WALL_OP_OWN_ROW) {
      job["board"] = "";
    }
    if (op.unit != 0) job["unit"] = op.unit;
    if (op.phase != WallOpPhase::Running) job["detail"] = jsonCopied(op.detail);
  }
}

}  // namespace

void webStreamRegister(AsyncWebServer& server) {
  stream.onConnect([](AsyncEventSourceClient*) { readerJoined.store(true); });
  server.addHandler(&stream);
}

void webStreamTick() {
  static uint32_t nextCheckMs = 0;
  static StreamTracker tracker;
  const uint32_t nowMs = millis();
  if ((int32_t)(nowMs - nextCheckMs) < 0) return;
  nextCheckMs = nowMs + STREAM_CHECK_MS;
  // count() before the flag is taken: the library tells of a reader before
  // it is in its list, and a send to nobody would be lost to that reader.
  if (stream.count() == 0) return;
  if (readerJoined.exchange(false)) tracker.sendAllAgain();

  // Heap, not netTask's stack.
  const uint32_t generation = wallStateRowsGeneration();
  std::unique_ptr<WallSnapshot> wall(new WallSnapshot(wallStateGet()));
  std::unique_ptr<WallVerdicts> verdicts(new WallVerdicts);
  const bool judged =
      wallVerdictsGet(*verdicts, generation) && generation == wallStateRowsGeneration();
  std::unique_ptr<DisplaySnapshot> own(new DisplaySnapshot(displaySnapshotGet()));

  JsonDocument doc;
  buildWall(doc, *wall, *own);
  sendIfDue(tracker, StreamTopic::Wall, doc);
  if (judged) {
    doc.clear();
    buildVerdict(doc, *wall, *verdicts);
    sendIfDue(tracker, StreamTopic::Verdict, doc);
  }
  doc.clear();
  buildJobs(doc, *wall);
  sendIfDue(tracker, StreamTopic::Jobs, doc);
  doc.clear();
  doc["seq"] = eventRecordNewestSeq();
  sendIfDue(tracker, StreamTopic::History, doc);
}
