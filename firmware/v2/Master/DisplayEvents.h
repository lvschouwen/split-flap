#pragma once
// DisplayEvents.h — pure logic for the /events SSE display push (#251):
// change detection against the display snapshot's text plus the event
// payload. netTask ticks the tracker (webDisplayEventsTick); the
// AsyncEventSource itself lives in WebEndpoints.cpp. Natively tested by
// test_display_events.
//
// Payload is text-only by design: alignment/speed/mode changes are rare
// and ride the /settings poll the mirror already follows — the event's job
// is making the flaps flip the moment displayTask executes a command.

#include <string.h>

#include "DisplayCommand.h"
#include "SettingsJson.h"  // appendJsonString

struct DisplayEventTracker {
  char lastText[DISPLAY_CMD_TEXT_LEN + 1] = {0};
};

// True when `text` differs from the last observation (which it then
// becomes). The tracker starts at "" — a boot-time text is a change, and
// pushing it to zero connected clients is harmless.
inline bool displayEventDue(DisplayEventTracker& t, const char* text) {
  if (strncmp(t.lastText, text, DISPLAY_CMD_TEXT_LEN) == 0) return false;
  strncpy(t.lastText, text, DISPLAY_CMD_TEXT_LEN);
  t.lastText[DISPLAY_CMD_TEXT_LEN] = '\0';
  return true;
}

// {"text":"..."} with full JSON escaping — display text is user input.
inline String buildDisplayEventJson(const char* text) {
  String out = "{\"text\":";
  appendJsonString(out, String(text));
  out += '}';
  return out;
}
