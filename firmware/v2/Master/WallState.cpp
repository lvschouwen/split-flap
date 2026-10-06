// WallState.cpp — contract in WallState.h.
#include "WallState.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>

#include "HelpersSerialHandling.h"

namespace {

SemaphoreHandle_t wallMutex = nullptr;
WallSnapshot wall;
std::atomic<uint32_t> rowsGeneration{0};

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
}
