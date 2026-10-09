// WallShow.cpp — contract in WallShow.h.
#include "WallShow.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sys/time.h>

#include <atomic>

#include "ClockPolicy.h"  // clockIsTimeSynced
#include "ClusterLayout.h"
#include "DisplayCommand.h"
#include "HelpersSerialHandling.h"
#include "MqttService.h"  // mqttNotificationActive
#include "ReflashPlan.h"
#include "Tasks.h"
#include "WallState.h"

namespace {

// How often the own row is checked for showing something else than its text
// (it costs a copy of the display snapshot). A text that waits for its
// instant is checked every pass.
constexpr uint32_t OWN_ROW_CHECK_MS = 250;

SemaphoreHandle_t showMutex = nullptr;
std::atomic<bool> active{false};

struct Locked {
  Locked() { xSemaphoreTake(showMutex, portMAX_DELAY); }
  ~Locked() { xSemaphoreGive(showMutex); }
};

WallRowsTable table;
int ownRow = -1;
String lastContentKey;
String segments[CLUSTER_MAX_MEMBERS];  // by index in the rows table
bool rowChanged[CLUSTER_MAX_MEMBERS] = {false};
uint64_t commitAtMs = 0;
int speed = 0;

bool ownPending = false;
uint32_t ownDueMs = 0;
uint32_t ownCheckedMs = 0;

uint64_t epochNowMs(bool& synced) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  synced = clockIsTimeSynced(tv.tv_sec);
  return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

// Lays the content out and stages the rows whose piece changed. `flipAtMs`
// 0 = the short lead from now.
void submit(const String& contentKey, bool isClock, const String& textOrTime, const String& date,
            const String& alignment, int flapSpeed, uint64_t flipAtMs) {
  if (!wallShowActive()) return;
  bool synced = false;
  const uint64_t nowE = epochNowMs(synced);
  Locked lock;
  if (contentKey == lastContentKey) return;
  String pieces[CLUSTER_MAX_MEMBERS];
  const ClusterMemberTable layout = wallRowsLayout(table);
  const DisplayAlignment align = displayAlignmentFromString(alignment);
  const bool ok = isClock ? clusterClockSegments(textOrTime, date, align, layout, pieces)
                          : layoutGridText(textOrTime, align, layout, pieces);
  if (!ok) {
    SerialPrintln(F("wall: text not shown, the rows table does not hold"));
    return;
  }
  lastContentKey = contentKey;
  speed = flapSpeed;
  commitAtMs = !synced ? 0 : flipAtMs != 0 ? flipAtMs : wallCommitAtMs(nowE, synced);
  for (int i = 0; i < table.count; i++) {
    if (pieces[i] == segments[i]) continue;
    segments[i] = pieces[i];
    if (i == ownRow) {
      ownPending = true;
      ownDueMs = millis() +
                 clusterRenderDelayMs(wallRowFlipAtMs(commitAtMs, i), nowE, synced);
    } else {
      rowChanged[i] = true;
    }
  }
}

}  // namespace

void wallShowInit() {
  showMutex = xSemaphoreCreateMutex();
  if (showMutex == nullptr) {
    Serial.println(F("FATAL: wall show mutex allocation failed"));
    abort();
  }
  uint32_t generation;
  wallShowRowsChanged(wallStateRows(generation));
}

bool wallShowActive() { return active.load(std::memory_order_relaxed); }

void wallShowText(const String& text, const String& alignment, int flapSpeed) {
  submit("t:" + alignment + ":" + String(flapSpeed) + ":" + text, false, text, "", alignment,
         flapSpeed, 0);
}

void wallShowClock(const String& timeText, const String& dateText, const String& alignment,
                   int flapSpeed, uint64_t flipAtMs) {
  submit("c:" + alignment + ":" + String(flapSpeed) + ":" + timeText + "|" + dateText, true,
         timeText, dateText, alignment, flapSpeed, flipAtMs);
}

// The content key is left as it is: the clock ticker's next call is the same
// content and changes nothing, so the wall stays blank until the content
// itself moves (the next minute, a new text), as a single board does.
void wallShowBlank() {
  if (!wallShowActive()) return;
  bool synced = false;
  const uint64_t nowE = epochNowMs(synced);
  Locked lock;
  commitAtMs = wallCommitAtMs(nowE, synced);
  for (int i = 0; i < table.count; i++) {
    if (i == ownRow || segments[i].length() == 0) continue;
    segments[i] = "";
    rowChanged[i] = true;
  }
  if (ownRow >= 0) {
    segments[ownRow] = "";
    ownPending = false;
  }
}

bool wallShowRowText(int row, char* out, size_t cap) {
  Locked lock;
  if (row < 0 || row >= table.count) return false;
  strlcpy(out, segments[row].c_str(), cap);
  return true;
}

void wallShowRowsChanged(const WallRowsTable& next) {
  Locked lock;
  table = next;
  ownRow = wallRowsOwn(table);
  lastContentKey = "";
  for (int i = 0; i < CLUSTER_MAX_MEMBERS; i++) {
    segments[i] = "";
    rowChanged[i] = false;
  }
  ownPending = false;
  active.store(table.count > 0, std::memory_order_relaxed);
}

void wallShowServiceOwnRow(uint32_t nowMs) {
  String text;
  int flapSpeed;
  WallOwnRow own;
  {
    Locked lock;
    if (ownRow < 0) return;
    if (!ownPending && (uint32_t)(nowMs - ownCheckedMs) < OWN_ROW_CHECK_MS) return;
    own.pending = ownPending;
    const int32_t untilDue = (int32_t)(ownDueMs - nowMs);
    own.msUntilDue = untilDue > 0 ? (uint32_t)untilDue : 0;
    if (own.pending && own.msUntilDue > 0) return;
    ownCheckedMs = nowMs;
    text = segments[ownRow];
    flapSpeed = speed;
  }
  // Outside the lock: these take other modules' locks.
  const DisplaySnapshot snap = displaySnapshotGet();
  own.text = text.c_str();
  own.displayText = snap.currentText;
  own.reflashing = reflashInProgress(snap.reflash);
  own.notification = mqttNotificationActive();
  own.quiet = tasksQuiet();
  own.displayBusy = snap.busy;
  if (wallOwnRowAction(own) != WallOwnAction::Show) return;
  // The piece is already placed on the row: shown from the left as it is.
  if (!displayEnqueue(makeShowTextCommand(text, "left", flapSpeed))) return;
  Locked lock;
  // Only when no newer text was staged meanwhile.
  if (ownRow >= 0 && segments[ownRow] == text) ownPending = false;
}

bool wallShowTakeRow(int row, WallRowShow& out) {
  Locked lock;
  if (row < 0 || row >= table.count || !rowChanged[row]) return false;
  rowChanged[row] = false;
  strlcpy(out.text, segments[row].c_str(), sizeof(out.text));
  out.speed = (uint16_t)speed;
  out.commitAtMs = wallRowFlipAtMs(commitAtMs, row);
  return true;
}
