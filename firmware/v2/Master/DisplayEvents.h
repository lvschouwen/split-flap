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
  String lastRowsKey;  // "" whenever this master isn't leading a cluster
};

// Injective join of the wall rows (#277): length-prefixed so row contents
// can never fake a row boundary. "" for rowCount 0 — the not-leading key.
inline String displayEventRowsKey(const String* rows, int rowCount) {
  String key;
  for (int i = 0; i < rowCount; i++) {
    key += (int)rows[i].length();
    key += ':';
    key += rows[i];
  }
  return key;
}

// True when `text` OR the wall rows differ from the last observation
// (which they then become). The tracker starts at ""/"" — a boot-time text
// is a change, and pushing it to zero connected clients is harmless. A
// cluster-leader row changing while this master's own row didn't is a
// change too, as is leaving cluster mode (the browser must collapse its
// wall back to the single-row mirror).
inline bool displayEventDue(DisplayEventTracker& t, const char* text,
                            const String& rowsKey) {
  if (strncmp(t.lastText, text, DISPLAY_CMD_TEXT_LEN) == 0 &&
      t.lastRowsKey == rowsKey) {
    return false;
  }
  strncpy(t.lastText, text, DISPLAY_CMD_TEXT_LEN);
  t.lastText[DISPLAY_CMD_TEXT_LEN] = '\0';
  t.lastRowsKey = rowsKey;
  return true;
}

inline bool displayEventDue(DisplayEventTracker& t, const char* text) {
  return displayEventDue(t, text, String());
}

// {"text":"..."} with full JSON escaping — display text is user input.
// While leading a cluster (#277), the payload adds the reconstructed wall:
// ,"selfRow":N,"rows":["...", ...] — the browser renders those verbatim
// (they arrive pre-positioned) and keys the health strip to selfRow
// (-1 = the leader owns no row; the strip stays below the wall).
// `source`/`sourceAgeSeconds` (#403) ride every push: the console names the
// producer at the instant the flaps turn instead of chasing a /settings read
// behind each event. Both are required — an unattributed push would put the
// console back to guessing.
inline String buildDisplayEventJson(const char* text, DisplaySource source,
                                    uint32_t sourceAgeSeconds,
                                    const String* rows = nullptr,
                                    int rowCount = 0, int selfRow = 0) {
  String out = "{\"text\":";
  appendJsonString(out, String(text));
  out += ",\"source\":";
  appendJsonString(out, String(displaySourceName(source)));
  out += ",\"sourceAge\":";
  out += sourceAgeSeconds;
  if (rows != nullptr && rowCount > 0) {
    out += ",\"selfRow\":";
    out += selfRow;
    out += ",\"rows\":[";
    for (int i = 0; i < rowCount; i++) {
      if (i > 0) out += ',';
      appendJsonString(out, rows[i]);
    }
    out += ']';
  }
  out += '}';
  return out;
}
