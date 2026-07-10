#include "StatusLed.h"

#include <Update.h>

#include "OtaService.h"
#include "StatusLedPolicy.h"
#include "WifiService.h"

#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN 48
#endif

static const MasterSettings* settingsRef = nullptr;
static StatusLedColor lastWritten;
static uint32_t lastEvalMs = 0;

// otaVerdictSnapshot() builds Strings under a mutex — too heavy for every
// tick. A boot's verdict only ever moves pending -> ok/reverted, so poll
// while pending and latch the moment it settles (a normal boot settles on
// the first query).
static bool pendingSettled = false;
static bool pendingVerify = false;

static void writeColor(const StatusLedColor& c) {
  rgbLedWrite(STATUS_LED_PIN, c.r, c.g, c.b);
  lastWritten = c;
}

void statusLedInit(const MasterSettings& settings) {
  settingsRef = &settings;
  writeColor(decideStatusLed(StatusLedInput{}));  // boot white, GPIO check
}

void statusLedTick() {
  const uint32_t now = millis();
  if (now - lastEvalMs < 100) return;
  lastEvalMs = now;

  if (!pendingSettled) {
    pendingVerify = otaVerdictSnapshot().lastFlashResult == "pending";
    if (!pendingVerify) pendingSettled = true;
  }

  StatusLedInput in;
  in.wifiPhase = wifiServicePhase();
  in.credsStored = settingsRef != nullptr && settingsRef->wifiSsid.length() > 0;
  // Cross-task peek at the async-handler-owned Update: a stale bool for one
  // tick miscolors the LED for 100 ms, nothing more.
  in.otaUploadActive = Update.isRunning();
  in.otaPendingVerify = pendingVerify;
  in.nowMs = now;

  const StatusLedColor c = decideStatusLed(in);
  if (!statusLedColorEq(c, lastWritten)) writeColor(c);
}
