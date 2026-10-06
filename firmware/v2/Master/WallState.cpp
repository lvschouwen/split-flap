// WallState.cpp — contract in WallState.h.
#include "WallState.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>

#include "HelpersSerialHandling.h"
#include "LargeAlloc.h"

namespace {

SemaphoreHandle_t wallMutex = nullptr;
WallSnapshot wall;
std::atomic<uint32_t> rowsGeneration{0};
SettingsStore* wallStore = nullptr;

// Unit facts per row board, outside internal RAM: about 2 KB a row.
struct RowUnits {
  bool have = false;
  uint32_t atMs = 0;
  UnitFactsDoc doc;
};
RowUnits* rowUnits = nullptr;  // CLUSTER_MAX_MEMBERS of them

WallOps ops;
WallRequest staged;
bool requestRunning = false;
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
  wall.rows = table;
  SerialPrintf("wall: %d board(s) on %d grid row(s)\n", (int)table.count, (int)grid.rows);
}

WallSnapshot wallStateGet() {
  Locked lock;
  return wall;
}

WallRowsTable wallStateRows(uint32_t& generation) {
  Locked lock;
  generation = rowsGeneration.load(std::memory_order_relaxed);
  return wall.rows;
}

uint32_t wallStateRowsGeneration() { return rowsGeneration.load(std::memory_order_relaxed); }

void wallStatePublishLink(int row, const WallRowLink& link) {
  if (row < 0 || row >= CLUSTER_MAX_MEMBERS) return;
  Locked lock;
  wall.link[row] = link;
  // Facts of an earlier connection are not this one's.
  if (!link.contact.connected) rowUnits[row].have = false;
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
  return ops.begin(name, row);
}

void wallOpFinish(uint32_t id, bool ok, const char* detail) {
  Locked lock;
  ops.finish(id, ok, detail);
}

bool wallOpGet(uint32_t id, WallOp& out) {
  Locked lock;
  const WallOp* op = ops.find(id);
  if (op == nullptr) return false;
  out = *op;
  return true;
}

void wallOpsFailRow(int row, const char* reason) {
  Locked lock;
  ops.failRow(row, reason);
}

bool wallStateStage(const WallRequest& request) {
  Locked lock;
  if (requestRunning || staged.kind != WallRequestKind::None) return false;
  staged = request;
  return true;
}

bool wallStateTakeRequest(WallRequest& out) {
  Locked lock;
  if (staged.kind == WallRequestKind::None) return false;
  out = staged;
  staged = WallRequest{};
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
    wall.rows = table;
    for (int i = 0; i < CLUSTER_MAX_MEMBERS; i++) {
      wall.link[i] = WallRowLink{};
      rowUnits[i].have = false;
    }
    // Inside the lock, after the table: a reader that sees the new number
    // gets the new table.
    rowsGeneration.fetch_add(1, std::memory_order_relaxed);
  }
  SerialPrintf("wall: rows table is now \"%s\"\n", wallRowsToString(table).c_str());
  return {true, ""};
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
