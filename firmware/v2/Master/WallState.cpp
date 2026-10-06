// WallState.cpp — contract in WallState.h.
#include "WallState.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>

#include "HelpersSerialHandling.h"
#include "LargeAlloc.h"

namespace {

SemaphoreHandle_t wallMutex = nullptr;

// Everything this module holds, taken once from PSRAM (LargeAlloc.h): a few
// kilobytes that internal RAM is better spent without.
struct OpData {
  uint16_t len = 0;
  uint8_t bytes[WALL_OP_DATA_MAX];
};

struct Held {
  WallSnapshot wall;
  WallOps ops;
  OpData opData[WALL_OPS_KEPT];  // by a job's place in `ops`
  WallRequest staged;
  bool jobStaged[CLUSTER_MAX_MEMBERS] = {false};
  wl_Op jobs[CLUSTER_MAX_MEMBERS];  // staged for the link task, by row
  WallOwnJob ownJob;
};
Held* held = nullptr;
std::atomic<uint32_t> rowsGeneration{0};
SettingsStore* wallStore = nullptr;

// Unit facts per row board, outside internal RAM: about 2 KB a row.
struct RowUnits {
  bool have = false;
  uint32_t atMs = 0;
  UnitFactsDoc doc;
};
RowUnits* rowUnits = nullptr;  // CLUSTER_MAX_MEMBERS of them

bool requestRunning = false;
wl_Config rowSettings = wl_Config_init_zero;
std::atomic<uint32_t> rowSettingsGeneration{0};
// Jobs waiting for the link task: it looks only when there are any.
std::atomic<int> jobsStaged{0};
uint32_t updateRetries = 0;  // a bit per row index, under the mutex
char releaseId[WALL_ROW_ID_MAX + 1] = {0};

struct Locked {
  Locked() { xSemaphoreTake(wallMutex, portMAX_DELAY); }
  ~Locked() { xSemaphoreGive(wallMutex); }
};

}  // namespace

void wallStateInit(SettingsStore& store) {
  wallMutex = xSemaphoreCreateMutex();
  if (wallMutex == nullptr) {
    Serial.println(F("FATAL: wall state mutex allocation failed"));
    abort();
  }
  wallStore = &store;
  held = (Held*)largeAlloc(sizeof(Held));
  if (held == nullptr) {
    Serial.println(F("FATAL: wall state allocation failed"));
    abort();
  }
  new (held) Held();
  rowUnits = (RowUnits*)largeAlloc(sizeof(RowUnits) * CLUSTER_MAX_MEMBERS);
  if (rowUnits == nullptr) {
    Serial.println(F("FATAL: wall state unit facts allocation failed"));
    abort();
  }
  for (int i = 0; i < CLUSTER_MAX_MEMBERS; i++) new (&rowUnits[i]) RowUnits();
  const String stored = store.getString(WALL_ROWS_NVS_KEY, "");
  WallRowsTable table;
  ClusterGrid grid;
  if (stored.length() == 0) return;
  if (!wallRowsFromString(stored, table)) {
    SerialPrintln(F("wall: stored rows table is damaged, not used"));
    return;
  }
  const ClusterVerdict verdict = wallRowsValidate(table, grid);
  if (!verdict.ok) {
    SerialPrintf("wall: stored rows table not used: %s\n", verdict.message);
    return;
  }
  held->wall.rows = table;
  SerialPrintf("wall: %d board(s) on %d grid row(s)\n", (int)table.count, (int)grid.rows);
}

WallSnapshot wallStateGet() {
  Locked lock;
  return held->wall;
}

WallRowsTable wallStateRows(uint32_t& generation) {
  Locked lock;
  generation = rowsGeneration.load(std::memory_order_relaxed);
  return held->wall.rows;
}

uint32_t wallStateRowsGeneration() { return rowsGeneration.load(std::memory_order_relaxed); }

void wallStatePublishLink(int row, const WallRowLink& link) {
  if (row < 0 || row >= CLUSTER_MAX_MEMBERS) return;
  Locked lock;
  held->wall.link[row] = link;
  // Facts of an earlier connection are not this one's.
  if (!link.contact.connected) rowUnits[row].have = false;
}

void wallStatePublishUpdate(uint8_t phase, int row) {
  Locked lock;
  held->wall.updatePhase = phase;
  held->wall.updateRow = (int8_t)row;
}

void wallStatePublishUnits(int row, const UnitFactsDoc& units, uint32_t nowMs) {
  if (row < 0 || row >= CLUSTER_MAX_MEMBERS) return;
  Locked lock;
  rowUnits[row].doc = units;
  rowUnits[row].atMs = nowMs;
  rowUnits[row].have = true;
}

bool wallStateRowUnits(int row, UnitFactsDoc& out, uint32_t& atMs) {
  if (row < 0 || row >= CLUSTER_MAX_MEMBERS) return false;
  Locked lock;
  if (!rowUnits[row].have) return false;
  out = rowUnits[row].doc;
  atMs = rowUnits[row].atMs;
  return true;
}

uint32_t wallOpBegin(const char* name, int row) {
  Locked lock;
  const uint32_t id = held->ops.begin(name, row);
  // The place may have held another job's result.
  if (id != 0) held->opData[held->ops.placeOf(id)].len = 0;
  return id;
}

uint32_t wallJobBegin(const char* name, int row, bool& rowBusy) {
  Locked lock;
  rowBusy = held->ops.runningOn(row);
  if (rowBusy) return 0;
  const uint32_t id = held->ops.begin(name, row);
  if (id != 0) held->opData[held->ops.placeOf(id)].len = 0;
  return id;
}

void wallOpDataPut(uint32_t id, uint32_t offset, const uint8_t* data, size_t n) {
  Locked lock;
  const int place = held->ops.placeOf(id);
  if (place < 0 || offset > WALL_OP_DATA_MAX || n > WALL_OP_DATA_MAX - offset) return;
  OpData& d = held->opData[place];
  // Pieces come in order; one that leaves a gap is not kept.
  if (offset > d.len) return;
  memcpy(d.bytes + offset, data, n);
  // A piece that comes twice never shortens what is held.
  if (offset + n > d.len) d.len = (uint16_t)(offset + n);
}

size_t wallOpDataGet(uint32_t id, uint8_t* out, size_t cap) {
  Locked lock;
  const int place = held->ops.placeOf(id);
  if (place < 0) return 0;
  const OpData& d = held->opData[place];
  const size_t n = d.len < cap ? d.len : cap;
  memcpy(out, d.bytes, n);
  return n;
}

bool wallJobStage(int row, const wl_Op& op, uint32_t generation) {
  if (row < 0 || row >= CLUSTER_MAX_MEMBERS) return false;
  Locked lock;
  if (held->jobStaged[row] || generation != rowsGeneration.load(std::memory_order_relaxed)) {
    return false;
  }
  held->jobs[row] = op;
  held->jobStaged[row] = true;
  jobsStaged.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool wallJobTake(int row, wl_Op& out) {
  if (jobsStaged.load(std::memory_order_relaxed) == 0) return false;
  if (row < 0 || row >= CLUSTER_MAX_MEMBERS) return false;
  Locked lock;
  if (!held->jobStaged[row]) return false;
  out = held->jobs[row];
  held->jobStaged[row] = false;
  jobsStaged.fetch_sub(1, std::memory_order_relaxed);
  return true;
}

void wallOwnJobSet(const WallOwnJob& job) {
  Locked lock;
  held->ownJob = job;
}

bool wallOwnJobGet(WallOwnJob& out) {
  Locked lock;
  if (held->ownJob.opId == 0) return false;
  out = held->ownJob;
  return true;
}

void wallOwnJobClear(uint32_t opId) {
  Locked lock;
  if (held->ownJob.opId == opId) held->ownJob = WallOwnJob{};
}

bool wallUnitUpdateRunning() {
  Locked lock;
  return held->ops.runningOnARowBoard("update-units");
}

void wallOpFinish(uint32_t id, bool ok, const char* detail) {
  Locked lock;
  held->ops.finish(id, ok, detail);
}

bool wallOpGet(uint32_t id, WallOp& out) {
  Locked lock;
  const WallOp* op = held->ops.find(id);
  if (op == nullptr) return false;
  out = *op;
  return true;
}

void wallOpsFailRow(int row, const char* reason) {
  Locked lock;
  held->ops.failRow(row, reason);
}

bool wallStateStage(const WallRequest& request) {
  Locked lock;
  if (requestRunning || held->staged.kind != WallRequestKind::None) return false;
  held->staged = request;
  return true;
}

bool wallStateTakeRequest(WallRequest& out) {
  Locked lock;
  if (held->staged.kind == WallRequestKind::None) return false;
  out = held->staged;
  held->staged = WallRequest{};
  requestRunning = true;
  return true;
}

void wallStateRequestDone() {
  Locked lock;
  requestRunning = false;
}

ClusterVerdict wallStateSetRows(const WallRowsTable& table) {
  ClusterGrid grid;
  if (table.count != 0) {
    const ClusterVerdict verdict = wallRowsValidate(table, grid);
    if (!verdict.ok) return verdict;
  }
  // Stored first: a table that is live but not stored would be gone at the
  // next start, with rows still paired to this master.
  wallStore->putString(WALL_ROWS_NVS_KEY, wallRowsToString(table));
  {
    Locked lock;
    held->wall.rows = table;
    for (int i = 0; i < CLUSTER_MAX_MEMBERS; i++) {
      held->wall.link[i] = WallRowLink{};
      rowUnits[i].have = false;
      if (held->jobStaged[i]) jobsStaged.fetch_sub(1, std::memory_order_relaxed);
      held->jobStaged[i] = false;
    }
    held->wall.updatePhase = 0;
    held->wall.updateRow = -1;
    // A row's number means another board now, or none.
    updateRetries = 0;
    held->ops.failRowBoards("the boards of the wall changed");
    // Inside the lock, after the table: a reader that sees the new number
    // gets the new table.
    rowsGeneration.fetch_add(1, std::memory_order_relaxed);
  }
  SerialPrintf("wall: rows table is now \"%s\"\n", wallRowsToString(table).c_str());
  return {true, ""};
}

void wallStateSetRowSettings(const String& tzPosix, bool updateUnitsAtStart) {
  Locked lock;
  // A row without its master shows the time: what an ESP-01 row has always
  // done. Per-row choices come with the board settings of the API.
  rowSettings.fallback = wl_Fallback_FALLBACK_TIME;
  rowSettings.update_units_at_start = updateUnitsAtStart;
  strlcpy(rowSettings.tz, tzPosix.c_str(), sizeof(rowSettings.tz));
  rowSettingsGeneration.fetch_add(1, std::memory_order_relaxed);
}

uint32_t wallStateRowSettings(wl_Config& out) {
  Locked lock;
  out = rowSettings;
  return rowSettingsGeneration.load(std::memory_order_relaxed);
}

uint32_t wallStateRowSettingsGeneration() {
  return rowSettingsGeneration.load(std::memory_order_relaxed);
}

void wallStateAskRelease(const char* id) {
  Locked lock;
  strlcpy(releaseId, id, sizeof(releaseId));
}

bool wallStateReleaseAsked(char* idOut, size_t cap) {
  Locked lock;
  if (releaseId[0] == 0) return false;
  strlcpy(idOut, releaseId, cap);
  return true;
}

void wallStateReleaseAnswered() {
  Locked lock;
  releaseId[0] = 0;
}

bool wallStateReleasePending() {
  Locked lock;
  return releaseId[0] != 0;
}

bool wallStateAskUpdateRetry(int row, uint32_t generation) {
  Locked lock;
  if (generation != rowsGeneration.load(std::memory_order_relaxed)) return false;
  if (row < 0 || row >= CLUSTER_MAX_MEMBERS) return false;
  updateRetries |= 1UL << row;
  return true;
}

uint32_t wallStateTakeUpdateRetries() {
  Locked lock;
  const uint32_t asked = updateRetries;
  updateRetries = 0;
  return asked;
}
