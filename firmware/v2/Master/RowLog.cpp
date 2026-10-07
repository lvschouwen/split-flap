// RowLog.cpp — contract in RowLog.h.
#include "RowLog.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "ClusterLayout.h"  // CLUSTER_MAX_MEMBERS
#include "LargeAlloc.h"
#include "RowLogPolicy.h"

namespace {

SemaphoreHandle_t mutex = nullptr;
RowLogRing rings[CLUSTER_MAX_MEMBERS];
RowLogWant wants[CLUSTER_MAX_MEMBERS];
uint32_t tableGeneration = 0;

struct Locked {
  Locked() { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Locked() { xSemaphoreGive(mutex); }
};

bool validRow(int row) { return row >= 0 && row < CLUSTER_MAX_MEMBERS; }

}  // namespace

void rowLogInit() { mutex = xSemaphoreCreateMutex(); }

bool rowLogRead(int row, uint32_t generation, String& out) {
  if (mutex == nullptr || !validRow(row)) return false;
  Locked lock;
  if (generation != tableGeneration) return false;
  RowLogRing& ring = rings[row];
  if (ring.buf == nullptr) {
    // Kept for good once a row's log was asked for: a wall has a few rows.
    char* memory = (char*)largeAlloc(ROW_LOG_KEEP_BYTES);
    if (memory == nullptr) return false;
    ring.begin(memory, ROW_LOG_KEEP_BYTES);
  }
  wants[row].asked(millis());
  out = "";
  if (!out.reserve(ring.len)) return false;
  out.concat(ring.buf, ring.len);
  return true;
}

void rowLogAppend(int row, const uint8_t* line, size_t n) {
  if (mutex == nullptr || !validRow(row)) return;
  Locked lock;
  rings[row].append(line, n);
}

bool rowLogWanted(int row, uint32_t nowMs) {
  if (mutex == nullptr || !validRow(row)) return false;
  Locked lock;
  return wants[row].wanted(nowMs);
}

void rowLogRowsChanged(uint32_t generation) {
  if (mutex == nullptr) return;
  Locked lock;
  tableGeneration = generation;
  for (int i = 0; i < CLUSTER_MAX_MEMBERS; i++) {
    rings[i].clear();
    wants[i] = RowLogWant{};
  }
}
