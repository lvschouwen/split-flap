#pragma once
// EventRecord — what happened on the wall, kept across restarts (#559/#570):
// fixed-size entries on the `storage` LittleFS, read back newest first by
// GET /api/v2/history. What an entry is, the wait for the clock and the
// paging are pure EventRecordPolicy.h.
//
// Ownership: any task says what happened with eventRecord(), which only
// stages. eventRecordTick() runs on netTask ONLY (the storage partition's
// sole flash writer, Hard rules) and writes what is staged. A reader takes
// the same lock the writer holds while it appends and rotates.
//
// Files: /events.bin (current) -> /events.prev.bin at the size cap.

#include "EventRecordPolicy.h"

// setup(), after flashLogInit() (the storage mount): finds where the seq
// count left off. No-op until the mount is up.
void eventRecordInit();

// Any task: something happened now. `boardId` is the board's id in the rows
// table, "" or nullptr for the master; `unit` a bus address, 0 for the board
// itself; `agoS`: it happened that long ago (a row board tells of its start
// once it is connected). Never blocks on flash.
void eventRecord(EventKind kind, uint8_t detail, const char* boardId, uint8_t unit, uint32_t a,
                 uint32_t b, uint32_t agoS = 0);
// The same for a caller that holds the board's key already.
void eventRecordKeyed(EventKind kind, uint8_t detail, uint16_t boardKey, uint8_t unit, uint32_t a,
                      uint32_t b, uint32_t agoS = 0);

// netTask only: writes the staged entries that may be written (all of them
// when `force`: the board is about to restart).
void eventRecordTick(bool force = false);

// Any task: up to `cap` entries with seq below `before` (0 = from the
// newest), newest first. Returns how many.
int eventRecordPage(uint32_t before, EventRecord* out, int cap);

// netTask only: the seq of the newest entry written, 0 when there is none.
uint32_t eventRecordNewestSeq();

// True when the storage mount succeeded (web layer's 503 gate).
bool eventRecordAvailable();
