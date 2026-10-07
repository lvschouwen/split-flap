#pragma once
// RowLog — the row boards' logs as the master holds them (#559/#572). A row
// sends its log over the wall link only while asked (RowLogPolicy.h): a read
// here is what makes the link task ask, and what it then receives is kept
// per row, in PSRAM, from the first read on. One leaf mutex.
//
// Rows are named by their index in the rows table: a changed table forgets
// every log.

#include <Arduino.h>

// setup(), before tasksInit().
void rowLogInit();

// Web side: what has arrived of this row's log, and the mark that someone is
// reading. False when `row` is no place in the table of `generation`, or
// there is no memory for it.
bool rowLogRead(int row, uint32_t generation, String& out);

// ---- link task only ----------------------------------------------------------------
void rowLogAppend(int row, const uint8_t* line, size_t n);
bool rowLogWanted(int row, uint32_t nowMs);
// The rows table changed.
void rowLogRowsChanged(uint32_t generation);
