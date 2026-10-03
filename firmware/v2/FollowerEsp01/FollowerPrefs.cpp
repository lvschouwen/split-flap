// FollowerPrefs.cpp — see FollowerPrefs.h.

#include "FollowerPrefs.h"

#include <Arduino.h>
#include <EEPROM.h>

#include "FollowerConfig.h"
#include "FollowerSettings.h"

static FollowerPrefs prefs;
static volatile bool stagedWrite = false;
static volatile bool stagedReflashOnBoot = true;

void prefsInit() {
  uint8_t rec[FOLLOWER_PREFS_LEN];
  for (int i = 0; i < FOLLOWER_PREFS_LEN; i++) {
    rec[i] = EEPROM.read(FOLLOWER_PREFS_OFF + i);
  }
  prefs = followerPrefsDecode(rec);
}

bool prefsReflashOnBoot() { return prefs.reflashOnBoot; }

void prefsStageReflashOnBoot(bool on) {
  stagedReflashOnBoot = on;
  stagedWrite = true;  // set last (flag-handoff rule)
}

#define PREFS_COMMIT_MIN_GAP_MS 10000UL
static bool everCommitted = false;
static uint32_t lastCommitMs = 0;

void prefsLoopTick(bool force) {
  if (!stagedWrite) return;
  if (!force && everCommitted &&
      millis() - lastCommitMs < PREFS_COMMIT_MIN_GAP_MS) {
    return;  // stays staged; the newest value wins when the gap has passed
  }
  stagedWrite = false;
  bool want = stagedReflashOnBoot;
  if (prefs.reflashOnBoot == want) return;
  FollowerPrefs next = prefs;
  next.reflashOnBoot = want;
  uint8_t rec[FOLLOWER_PREFS_LEN];
  followerPrefsEncode(next, rec);
  for (int i = 0; i < FOLLOWER_PREFS_LEN; i++) {
    EEPROM.write(FOLLOWER_PREFS_OFF + i, rec[i]);
  }
  everCommitted = true;
  lastCommitMs = millis();
  if (!EEPROM.commit()) {
    // GET /settings keeps reporting the value that is actually on flash.
    SerialPrintln(F("reflashOnBoot: EEPROM commit FAILED — will retry"));
    stagedWrite = true;
    return;
  }
  prefs = next;
  SerialPrint(F("reflashOnBoot set to "));
  SerialPrintln(want ? F("true") : F("false"));
}
