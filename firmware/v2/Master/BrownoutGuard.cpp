#include "BrownoutGuard.h"

#include <Preferences.h>
#include <esp_system.h>

#include <atomic>

#include "HelpersSerialHandling.h"
#include "TaskWatchdog.h"
#include "UnitBus.h"       // unitBusSetMotionCeiling (displayTask's call)
#include "WebEndpoints.h"  // webResetReasonName

static const char* kNamespace = "sfboot";  // shared with RebootCause.cpp
static const char* kRecordKey = "bGuard";

// Written by setup() and then by netTask only.
static BrownoutGuardRecord record;
static std::atomic<bool> held{false};
static std::atomic<bool> clearRequested{false};
static std::atomic<uint8_t> streakNow{0};
static int resetAtBoot = 0;

static void persist() {
  uint8_t blob[BROWNOUT_GUARD_BLOB_LEN];
  brownoutGuardEncode(record, blob);
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/false)) return;
  prefs.putBytes(kRecordKey, blob, sizeof(blob));
  prefs.end();
}

void brownoutGuardBoot() {
  uint8_t blob[BROWNOUT_GUARD_BLOB_LEN] = {0};
  size_t n = 0;
  Preferences prefs;
  if (prefs.begin(kNamespace, /*readOnly=*/true)) {
    if (prefs.isKey(kRecordKey)) n = prefs.getBytes(kRecordKey, blob, sizeof(blob));
    prefs.end();
  }
  BrownoutGuardRecord prev;
  brownoutGuardDecode(blob, n, prev);  // nothing readable = no history
  resetAtBoot = (int)esp_reset_reason();
  record = brownoutGuardStart(prev, resetAtBoot);
  held.store(record.held);
  streakNow.store(record.streak);
  persist();
}

void brownoutGuardLogReport() {
  if (record.held) {
    SerialPrintf("brownout guard: HELD after %u brownout resets in a row, each "
                 "within %lu min of its start — no unit moves until the action "
                 "clear-brownout-hold; check the supply first\n",
                 (unsigned)record.streak,
                 (unsigned long)(BROWNOUT_LOOP_WINDOW_MS / 60000UL));
  } else if (record.streak > 0) {
    SerialPrintf("brownout guard: %u of %d brownouts in a row cut a boot short "
                 "(last reset: %s)\n",
                 (unsigned)record.streak, BROWNOUT_LOOP_LIMIT,
                 webResetReasonName(resetAtBoot));
  }
}

void brownoutGuardTick() {
  if (clearRequested.exchange(false)) {
    if (brownoutGuardClear(record)) {
      persist();
      streakNow.store(0);
      held.store(false);
      SerialPrintln(F("brownout guard: hold cleared by the operator — the "
                      "units home one at a time"));
    }
    return;
  }
  if (brownoutGuardSettle(record, millis())) {
    persist();
    streakNow.store(0);
  }
}

bool brownoutGuardHeld() { return held.load(); }
uint8_t brownoutGuardStreak() { return streakNow.load(); }

bool brownoutGuardRequestClear() {
  if (!held.load()) return false;
  clearRequested.store(true);
  return true;
}

void brownoutGuardHoldDisplay() {
  if (!held.load()) return;
  unitBusSetMotionCeiling(BROWNOUT_HOLD_MOTION_CAP);
  SerialPrintln(F("brownout guard: display held — no boot-home, no command "
                  "runs until the hold is cleared"));
  while (held.load()) {
    wdtFeed();  // #314: a watched task waiting on an operator
    delay(200);
  }
  SerialPrintf("brownout guard: display released, at most %d unit(s) move at "
               "once for the rest of this boot\n", BROWNOUT_HOLD_MOTION_CAP);
}

String brownoutGuardJson() {
  String out = "{\"held\":";
  out += held.load() ? "true" : "false";
  out += ",\"streak\":";
  out += String((unsigned)streakNow.load());
  out += ",\"limit\":";
  out += String(BROWNOUT_LOOP_LIMIT);
  out += ",\"windowS\":";
  out += String((unsigned long)(BROWNOUT_LOOP_WINDOW_MS / 1000UL));
  out += '}';
  return out;
}
