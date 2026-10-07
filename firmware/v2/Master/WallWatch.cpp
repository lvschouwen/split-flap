// WallWatch.cpp — glue for WallWatch.h (#570): gathers the facts, judges,
// publishes. What is recorded is WallWatchPolicy.h's decision.

#include "WallWatch.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <time.h>

#include "ClockPolicy.h"  // clockIsTimeSynced
#include "EventRecord.h"
#include "FollowerImageStore.h"
#include "HelpersSerialHandling.h"
#include "LargeAlloc.h"
#include "ReflashPlan.h"  // reflashInProgress
#include "Tasks.h"
#include "WallState.h"
#include "WallUpdatePolicy.h"
#include "WallWatchPolicy.h"

#define WALL_WATCH_INTERVAL_MS 1000UL

namespace {

// Everything the worker's look needs, taken once (LargeAlloc.h): several
// kilobytes of snapshots that have no business on its stack.
struct Work {
  WatchTable<WALL_VERDICT_BOARDS> watch;
  WallSnapshot wall;
  DisplaySnapshot own;
  UnitFactsDoc units;
  WallVerdicts next;
};
Work* work = nullptr;

SemaphoreHandle_t verdictsMutex = nullptr;  // guards `published`; a leaf
WallVerdicts* published = nullptr;
uint32_t lastLookMs = 0;

struct BoardSink {
  uint16_t key;
  void event(EventKind kind, uint8_t detail, uint8_t unit, uint32_t a, uint32_t b) {
    eventRecordKeyed(kind, detail, key, unit, a, b);
  }
};

BoardReach reachOf(WallRowReach reach) {
  switch (reach) {
    case WallRowReach::Up: return BoardReach::Up;
    case WallRowReach::Busy: return BoardReach::Busy;
    case WallRowReach::Away: return BoardReach::Away;
    case WallRowReach::Lost: return BoardReach::Lost;
    case WallRowReach::Never: return BoardReach::Never;
  }
  return BoardReach::Never;
}

// Judges `width` units into the board's verdict slots and counts them into
// its facts.
void judgeUnits(WatchBoard& watch, const UnitFacts* units, int width, uint32_t nowMs, bool hold,
                BoardSink& sink, WallVerdictBoard& board, BoardFacts& facts) {
  if (width > UNITS_AMOUNT) width = UNITS_AMOUNT;
  watchUnits(watch, units, width, nowMs, hold, board.unit, sink);
  board.units = (uint8_t)width;
  UnitLevelCounts counts;
  uint8_t found = 0;
  for (int i = 0; i < width; i++) {
    unitLevelCount(counts, board.unit[i].level);
    if (units[i].state != 0) found++;
  }
  facts.unitsKnown = true;
  facts.unitsFound = found;
  facts.unitsFault = counts.fault;
  facts.unitsNote = counts.note;
}

void lookAtOwn(int ownRow, uint32_t nowMs, WallVerdictBoard& board) {
  WatchBoard* watch = work->watch.find(0);
  if (watch == nullptr) return;
  BoardSink sink{0};
  const DisplaySnapshot& own = work->own;
  BoardFacts facts;
  facts.own = true;
  facts.uptimeS = nowMs / 1000UL;
  facts.clockSet = clockIsTimeSynced(time(nullptr));
  facts.updatingUnits = reflashInProgress(own.reflash);
  facts.unitsPlaced =
      ownRow >= 0 ? work->wall.rows.rows[ownRow].width : own.displayWidth;
  board.own = true;
  board.row = (int8_t)ownRow;
  // Units a job is working on restart and pass through their bootloaders.
  const bool hold = facts.updatingUnits || wallJobRunningOn(WALL_OP_OWN_ROW);
  if (own.probed) {
    judgeUnits(*watch, own.units, own.displayWidth, nowMs, hold, sink, board, facts);
  }
  board.verdict = boardVerdict(facts);
  watchBoard(*watch, facts, board.verdict, sink);
}

void lookAtRow(int row, uint32_t nowMs, bool haveImage, const FollowerImageFacts& image,
               WallVerdictBoard& board) {
  const WallRowDef& def = work->wall.rows.rows[row];
  const WallRowLink& link = work->wall.link[row];
  const uint16_t key = eventBoardKey(def.id);
  WatchBoard* watch = work->watch.find(key);
  if (watch == nullptr) return;
  BoardSink sink{key};
  BoardFacts facts;
  facts.reach = reachOf(wallRowReach(link.contact, nowMs));
  facts.silentS =
      (link.contact.everHeard ? (uint32_t)(nowMs - link.contact.lastHeardMs) : nowMs) / 1000UL;
  // A row never heard is gone once the time a connected one would have been
  // written off has passed since this master started, or since the rows table
  // changed (which drops every connection).
  facts.startGraceOver = (uint32_t)(nowMs - work->wall.rowsSinceMs) >= WALL_LINK_LOST_MS;
  facts.rescue = link.everWelcomed && link.rescue;
  facts.busDead = link.haveStatus && link.status.bus_dead;
  facts.busEpisodes = link.status.bus_episodes;
  facts.updateBlocked = link.updateBlocked;
  facts.updateAttempts = link.updateAttempts;
  facts.updating = work->wall.updatePhase != (uint8_t)WallUpdatePhase::Idle &&
                   work->wall.updateRow == row;
  facts.updatingUnits = wallUnitUpdateRunningOn(row);
  facts.clockSet = !link.haveStatus || link.status.time_synced;
  facts.firmwareDiffers = link.everWelcomed && haveImage && strcmp(link.rev, image.rev) != 0;
  facts.uptimeS = link.haveStatus ? link.status.up_s : 0;
  facts.unitsPlaced = def.width;
  board.row = (int8_t)row;

  if (link.everWelcomed) {
    watchRowRestarts(*watch, link.restarts, link.rescue, link.rev, sink);
  }
  const bool reachable = facts.reach == BoardReach::Up || facts.reach == BoardReach::Busy;
  uint32_t atMs = 0;
  if (reachable && !facts.rescue && wallStateRowUnits(row, work->units, atMs)) {
    const bool hold = facts.updatingUnits || wallJobRunningOn(row) ||
                      (link.haveStatus && link.status.busy);
    judgeUnits(*watch, work->units.units, work->units.width, nowMs, hold, sink, board, facts);
  }
  board.verdict = boardVerdict(facts);
  watchBoard(*watch, facts, board.verdict, sink);
}

}  // namespace

void wallWatchInit() {
  verdictsMutex = xSemaphoreCreateMutex();
  void* workPlace = largeAlloc(sizeof(Work));
  void* publishedPlace = largeAlloc(sizeof(WallVerdicts));
  if (verdictsMutex == nullptr || workPlace == nullptr || publishedPlace == nullptr) {
    SerialPrintln(F("wall: no memory to judge the wall — no verdicts, no events"));
    return;
  }
  work = new (workPlace) Work();
  published = new (publishedPlace) WallVerdicts();
}

void wallWatchTick() {
  if (work == nullptr) return;
  const uint32_t nowMs = millis();
  if (lastLookMs != 0 && (uint32_t)(nowMs - lastLookMs) < WALL_WATCH_INTERVAL_MS) return;
  lastLookMs = nowMs;

  // The table and its number together: a look that straddles a change of the
  // table is thrown away by its readers, who ask for the number they hold.
  const uint32_t generation = wallStateRowsGeneration();
  work->wall = wallStateGet();
  work->own = displaySnapshotGet();
  FollowerImageFacts image;
  const bool haveImage = followerImageFacts(image);

  const WallRowsTable& table = work->wall.rows;
  uint16_t keys[WALL_VERDICT_BOARDS];
  int keyCount = 0;
  keys[keyCount++] = 0;
  for (int i = 0; i < table.count; i++) {
    if (!wallRowIsOwn(table.rows[i])) keys[keyCount++] = eventBoardKey(table.rows[i].id);
  }
  work->watch.keep(keys, keyCount);

  WallVerdicts& next = work->next;
  next = WallVerdicts();
  next.rowsGeneration = generation;
  lookAtOwn(wallRowsOwn(table), nowMs, next.boards[next.count++]);
  for (int i = 0; i < table.count && next.count < WALL_VERDICT_BOARDS; i++) {
    if (wallRowIsOwn(table.rows[i])) continue;
    lookAtRow(i, nowMs, haveImage, image, next.boards[next.count++]);
  }
  for (int i = 0; i < next.count; i++) {
    next.wall = verdictWorse(next.wall, next.boards[i].verdict.level);
  }
  next.valid = generation == wallStateRowsGeneration();

  xSemaphoreTake(verdictsMutex, portMAX_DELAY);
  *published = next;
  xSemaphoreGive(verdictsMutex);
}

bool wallVerdictsGet(WallVerdicts& out, uint32_t rowsGeneration) {
  if (published == nullptr) return false;
  bool ok = false;
  xSemaphoreTake(verdictsMutex, portMAX_DELAY);
  if (published->valid && published->rowsGeneration == rowsGeneration) {
    out = *published;
    ok = true;
  }
  xSemaphoreGive(verdictsMutex);
  return ok;
}
