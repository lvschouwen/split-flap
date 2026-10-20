// EventRecord.cpp — glue for EventRecord.h (#570); decisions live in
// EventRecordPolicy.h. Bench-tier (LittleFS + FreeRTOS; not native-buildable).

#include "EventRecord.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <time.h>

#include <memory>
#include <new>

#include "ClockPolicy.h"   // clockIsTimeSynced
#include "CrashContext.h"  // #504 flash-write mark
#include "EventDampPolicy.h"
#include "FlashLog.h"      // flashLogAvailable() — same storage mount
#include "HelpersSerialHandling.h"

#define EVENT_TICK_INTERVAL_MS 250UL

static const char* EVENTS_PATH = "/events.bin";
static const char* EVENTS_PREV_PATH = "/events.prev.bin";

// `ready` is written once in eventRecordInit() (single-threaded setup) and
// read-only afterwards.
static bool ready = false;
static SemaphoreHandle_t stageMutex = nullptr;  // guards `stage`; a leaf
static SemaphoreHandle_t filesMutex = nullptr;  // held over every use of the two files
static EventStage stage;
// netTask-only state.
static uint32_t nextSeq = 1;
static uint32_t lastTickMs = 0;
static EventRecord due[EVENT_STAGE_CAP + 1];
// #571: what repeats is counted, not written. In RTC memory so a restart
// that is not a power cut keeps the counts; netTask-only after setup().
RTC_NOINIT_ATTR static EventDamp damp;
// Each entry taken can bring one Repeated entry before it; windows that
// have passed bring one a slot.
static EventRecord toWrite[2 * (EVENT_STAGE_CAP + 1) + EVENT_DAMP_SLOTS];
// How many of the newest entries rebuild the table after a power cut.
#define EVENT_DAMP_SEED_ENTRIES 64

namespace {

// EventRecordPolicy.h's idea of a file, over an open LittleFS one.
struct OpenFile {
  File& f;
  size_t size() { return f.size(); }
  bool read(size_t offset, uint8_t* buf, size_t n) {
    return f.seek(offset) && f.read(buf, n) == n;
  }
};

uint32_t lastSeqOf(const char* path) {
  if (!LittleFS.exists(path)) return 0;
  File f = LittleFS.open(path, "r");
  if (!f) return 0;
  OpenFile file{f};
  const uint32_t seq = eventLastSeq(file);
  f.close();
  return seq;
}

int pageOf(const char* path, uint32_t before, EventRecord* out, int cap, int have) {
  if (have >= cap || !LittleFS.exists(path)) return have;
  File f = LittleFS.open(path, "r");
  if (!f) return have;
  OpenFile file{f};
  have = eventPageRead(file, before, out, cap, have);
  f.close();
  return have;
}

}  // namespace

bool eventRecordAvailable() { return ready; }

void eventRecordInit() {
  if (ready || !flashLogAvailable()) return;
  stageMutex = xSemaphoreCreateMutex();
  filesMutex = xSemaphoreCreateMutex();
  if (stageMutex == nullptr || filesMutex == nullptr) {
    SerialPrintln(F("events: allocation failed — no event record"));
    return;
  }
  // The current file is empty right after a rotation: then the older one
  // holds the newest entry.
  uint32_t last = lastSeqOf(EVENTS_PATH);
  if (last == 0) last = lastSeqOf(EVENTS_PREV_PATH);
  nextSeq = last + 1;
  ready = true;
  if (esp_reset_reason() == ESP_RST_POWERON || !damp.valid()) {
    damp.clear();
    std::unique_ptr<EventRecord[]> newest(new (std::nothrow) EventRecord[EVENT_DAMP_SEED_ENTRIES]);
    if (newest) {
      const int n = eventRecordPage(0, newest.get(), EVENT_DAMP_SEED_ENTRIES);
      for (int i = n; i > 0; i--) damp.seed(newest[i - 1]);
    }
  }
}

namespace {

// The entries taken, damped (#571), with the Repeated entries of what was not
// written, numbered again from the first seq taken. Returns how many.
int dampTaken(int n, uint32_t nowS, bool clockSet) {
  int out = 0;
  for (int i = 0; i < n; i++) {
    EventRecord summary;
    bool hasSummary = false;
    const bool written = damp.admit(due[i], summary, hasSummary);
    if (hasSummary) toWrite[out++] = summary;
    if (written) toWrite[out++] = due[i];
  }
  if (clockSet) out += damp.due(nowS, toWrite + out, EVENT_DAMP_SLOTS);
  return out;
}

}  // namespace

void eventRecordKeyed(EventKind kind, uint8_t detail, uint16_t boardKey, uint8_t unit, uint32_t a,
                      uint32_t b, uint32_t agoS) {
  if (!ready) return;
  EventRecord r;
  r.kind = (uint8_t)kind;
  r.detail = detail;
  r.board = boardKey;
  r.unit = unit;
  r.a = a;
  r.b = b;
  const uint32_t atMs = millis() - eventAgoMs(agoS);
  xSemaphoreTake(stageMutex, portMAX_DELAY);
  stage.put(r, atMs);
  xSemaphoreGive(stageMutex);
}

void eventRecord(EventKind kind, uint8_t detail, const char* boardId, uint8_t unit, uint32_t a,
                 uint32_t b, uint32_t agoS) {
  eventRecordKeyed(kind, detail, eventBoardKey(boardId), unit, a, b, agoS);
}

uint32_t eventRecordNewestSeq() { return ready ? nextSeq - 1 : 0; }

void eventRecordTick(bool force) {
  if (!ready) return;
  const uint32_t nowMs = millis();
  if (!force && (uint32_t)(nowMs - lastTickMs) < EVENT_TICK_INTERVAL_MS) return;
  lastTickMs = nowMs;

  const time_t now = time(nullptr);
  const bool clockSet = clockIsTimeSynced(now);
  // Numbered on a copy: damping drops some, and nextSeq only ever grows.
  const uint32_t firstSeq = nextSeq;
  uint32_t takenSeq = nextSeq;
  xSemaphoreTake(stageMutex, portMAX_DELAY);
  const int taken = stage.take((uint32_t)now, clockSet, nowMs, force, takenSeq, due,
                               EVENT_STAGE_CAP + 1);
  xSemaphoreGive(stageMutex);
  const int n = dampTaken(taken, (uint32_t)now, clockSet);
  for (int i = 0; i < n; i++) toWrite[i].seq = firstSeq + (uint32_t)i;
  nextSeq = firstSeq + (uint32_t)n;
  if (n == 0) return;

  // #504: netTask is the only caller; its loop re-marks on the next pass.
  crashCtxMark(CRASH_SLOT_NET, CRASH_ACT_FLASH_WRITE);
  xSemaphoreTake(filesMutex, portMAX_DELAY);
  File f = LittleFS.open(EVENTS_PATH, FILE_APPEND, true);
  if (!f) {
    xSemaphoreGive(filesMutex);
    SerialPrintf("events: append open failed — %d entries lost\n", n);
    return;
  }
  // A write cut short by a power cut leaves the file off the entry boundary.
  static const uint8_t zeros[EVENT_RECORD_SIZE] = {0};
  const size_t pad = eventPadNeeded(f.size());
  bool ok = pad == 0 || f.write(zeros, pad) == pad;
  for (int i = 0; ok && i < n; i++) {
    uint8_t raw[EVENT_RECORD_SIZE];
    eventEncode(toWrite[i], raw);
    ok = f.write(raw, sizeof(raw)) == sizeof(raw);
  }
  const size_t size = f.size();
  f.close();
  bool rotated = true;
  if (eventShouldRotate(size)) {
    LittleFS.remove(EVENTS_PREV_PATH);
    rotated = LittleFS.rename(EVENTS_PATH, EVENTS_PREV_PATH);
  }
  xSemaphoreGive(filesMutex);
  if (!ok) SerialPrintln(F("events: write failed"));
  if (!rotated) SerialPrintln(F("events: rotate rename failed"));
}

int eventRecordPage(uint32_t before, EventRecord* out, int cap) {
  if (!ready) return 0;
  xSemaphoreTake(filesMutex, portMAX_DELAY);
  int have = pageOf(EVENTS_PATH, before, out, cap, 0);
  have = pageOf(EVENTS_PREV_PATH, before, out, cap, have);
  xSemaphoreGive(filesMutex);
  return have;
}
