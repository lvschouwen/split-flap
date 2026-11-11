#pragma once
// WallShow.h — showing text on a wall of several rows (#559/#566). The
// producers (web, MQTT, the clock ticker) hand the wall's LOGICAL text here;
// it is laid out on the grid (ClusterLayout.h, over the rows table of
// WallRows.h), every row gets its piece, and all rows flip at one instant
// (WallShowPolicy.h). The master's own row goes through the DisplayCommand
// queue like any other producer's text; a row board's piece goes out on the
// wall link.
//
// Active only while the rows table has rows. A master on its own shows text
// the way it always did, and nothing here is called.
//
// Producers call from any task and only stage (one leaf mutex). The link
// task does the rest every pass: it queues the own row at its instant and
// takes the row boards' pieces for its connections.

#include <Arduino.h>

#include "WallLinkPolicy.h"  // WALL_ROW_TEXT_MAX
#include "WallRows.h"
#include "WallShowPolicy.h"

// setup(), after wallStateInit().
void wallShowInit();

// Does the wall have rows, so that text goes through here? Any task, no lock.
bool wallShowActive();

// The wall's text. The same content again is nothing new.
void wallShowText(const String& text, const String& alignment, int speed);
// The clock: time on the first grid row, date on the second. `commitAtMs` is
// the flip instant (WallShowPolicy.h's wallClockTarget).
void wallShowClock(const String& timeText, const String& dateText, const String& alignment,
                   int speed, uint64_t commitAtMs);
// A text shown over the mode (#616). With `keepClock` (#669: a clock wall that
// is not quiet) the first grid row keeps the time and the text takes the rows
// under it, when it fits there whole and the clock is synced; otherwise the
// text takes the whole wall. The caller raises the notification gate first:
// the clock frame drops a kept-clock text it meets without one.
void wallShowOverMode(const String& text, const String& alignment, int speed, bool keepClock);
// Is a text up under the clock? The ticker then keeps the minute going.
bool wallShowClockKept();
// Stop: every row board blank. The master's own row is blanked by its Stop.
void wallShowBlank();

// What a row is to show, for the API. False when there is no such row.
bool wallShowRowText(int row, char* out, size_t cap);

// ---- link task only ----------------------------------------------------------------

// The rows table changed (rows are named by their index in it): forget
// everything; the producers hand the text in again within a second.
void wallShowRowsChanged(const WallRowsTable& table);

// The master's own row: queue it at its instant, show it again after
// something else was on it.
void wallShowServiceOwnRow(uint32_t nowMs);

struct WallRowShow {
  char text[WALL_ROW_TEXT_MAX + 1];
  uint16_t speed;
  uint64_t commitAtMs;
};
// True once for every change of a row board's piece.
bool wallShowTakeRow(int row, WallRowShow& out);
