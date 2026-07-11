#pragma once
// FlashLog — persistent boot/debug log on the `storage` partition (#206).
// Serial-less debugging: everything the SerialPrint helpers emit (already
// mirrored into the WebLog RAM ring) is also staged here and flushed to a
// LittleFS file by netTask, so a boot's log survives reboot and USB-unplug.
//
// Ownership: producers (any task) only touch the mutex-guarded staging
// buffer via flashLogStage(); ALL filesystem work happens in
// flashLogTick(), called from netTask (webEndpointsLoop) — one flash
// writer, open→append→close per flush so read handlers never share a write
// handle. Policy (capacities, cadence, rotation) is pure FlashLogPolicy.h.
//
// Files: /log.txt (current) → renamed to /log.prev.txt at the size cap.
// Web: GET /log/flash [?prev=1], POST /log/flash/clear (staged, drained by
// the tick — handlers never write flash).

#include <stddef.h>

// Mounts LittleFS on the `storage` partition (formats on first use) and
// writes the boot marker (reset reason + build rev). Call once from
// setup(), after webLogInit(), before tasksInit().
void flashLogInit();

// Stages bytes for the next flush. Safe from any task; drops (and counts)
// what doesn't fit — the flush emits a dropped-bytes marker line.
void flashLogStage(const char* data, size_t len);

// netTask only: flush per policy (or immediately when `force`), rotate at
// the file cap, drain a staged clear. `force` is for the pre-reboot drain
// so the last lines land before ESP.restart().
void flashLogTick(bool force = false);

// True when the mount succeeded and logging is live (web layer's 503 gate).
bool flashLogAvailable();

// Stages a clear (both files deleted on the next tick). Handler-safe.
void flashLogRequestClear();

// Absolute LittleFS paths for the web layer's file responses.
const char* flashLogCurrentPath();
const char* flashLogPreviousPath();
