#pragma once
// ClockPolicy.h — clockTask's 1 Hz mode-ticker brain (#192), pure logic.
//
// v1's loop() re-showed the mode content every second and let showText()'s
// lastWrittenText comparison eat the duplicates. v2 relocates that gate in
// front of the queue: the ticker only enqueues when the desired content
// differs from what the display shows AND from what it already has in
// flight — clock mode costs one DisplayCommand per minute, not per second.
//
// Dedup state contract (the glue in Tasks.cpp): call clockTickObserve()
// with the fresh snapshot text first (clears lastQueued once the worker has
// executed it), then decideClockTick(); advance lastQueued to the decision
// text only after a SUCCESSFUL displayEnqueue — a full queue retries on the
// next tick. Known benign race: if a drain-enqueued message lands in the
// same tick window a ticker command executes, the observe-clear is missed
// and a clock re-show waits for the minute rollover.

#include <Arduino.h>

#include <ctime>

// v1 clockFormat parity; strftime(3) conversion specifiers.
#define CLOCK_FORMAT "%H:%M"

// v1 stamps message/mode submissions with this shape (#128).
#define CLOCK_STAMP_FORMAT "%d %b %y %H:%M:%S"

// SNTP has delivered a plausible wall clock (v1's boot-wait constant,
// 2001-09-09). Below this the clock is still counting from the 1970 epoch.
inline bool clockIsTimeSynced(time_t now) { return now >= 1000000000L; }

// Local-time strftime of an explicit epoch — the time source stays a
// parameter so this is host-testable (TZ env + tzset on native).
inline String formatDateTime(time_t now, const char* fmt) {
  struct tm tmInfo;
  localtime_r(&now, &tmInfo);
  char buf[64] = {0};  // strftime leaves buf indeterminate on overflow
  strftime(buf, sizeof(buf), fmt, &tmInfo);
  return String(buf);
}

// CONTRACT: every text field below is display-domain — already passed
// through truncateForDisplay() (DisplayCommand.h). A raw >display-width
// string here can never equal any snapshot text, wedging the dedup (H1).
struct ClockTickInput {
  String deviceMode;          // "text" | "clock" (web-boundary validated)
  String inputText;           // retained runtime message (never persisted)
  String formattedTime;       // formatDateTime(now, CLOCK_FORMAT)
  bool timeSynced = false;
  bool displayBusy = false;
  String displayCurrentText;  // DisplaySnapshot.currentText
  String lastQueued;          // ticker's in-flight command ("" = none)
};

struct ClockTickDecision {
  bool enqueue = false;
  String text;
};

// Clears the in-flight marker once the display has executed it. Run before
// decideClockTick() each tick, with the same snapshot text passed to it.
inline void clockTickObserve(String& lastQueued,
                             const String& displayCurrentText) {
  if (lastQueued.length() > 0 && lastQueued == displayCurrentText) {
    lastQueued = "";
  }
}

inline ClockTickDecision decideClockTick(const ClockTickInput& in) {
  ClockTickDecision d;

  String desired;
  if (in.deviceMode == "clock") {
    // Un-synced clock holds the current content (deliberate v1 deviation:
    // never flap epoch-1970 times while NTP is unreachable).
    if (!in.timeSynced) return d;
    desired = in.formattedTime;
  } else if (in.deviceMode == "text") {
    desired = in.inputText;
  } else {
    return d;  // unknown mode degrades to silence
  }

  if (desired.length() == 0) return d;
  if (in.displayBusy) return d;
  if (desired == in.displayCurrentText) return d;
  if (in.lastQueued.length() > 0 && desired == in.lastQueued) return d;

  d.enqueue = true;
  d.text = desired;
  return d;
}
