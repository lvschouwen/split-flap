#pragma once
// FlashLog — persistent boot/debug log on the `storage` partition (#206).
// Serial-less debugging: everything the SerialPrint helpers emit is staged
// here and flushed to a LittleFS file by netTask, so a boot's log survives
// reboot and USB-unplug. This is now the ONLY log tee — the in-RAM ring
// that used to sit in front of it existed to answer GET /log and went with
// the HTTP layer.
//
// Ownership: producers (any task) only touch the mutex-guarded staging
// buffer via flashLogStage(); ALL filesystem work happens in
// flashLogTick(), called from netTask — one flash writer, open→append→close
// per flush. Policy (capacities, cadence, rotation) is pure
// FlashLogPolicy.h.
//
// Files: /log.txt (current) → renamed to /log.prev.txt at the size cap.
// Nothing reads them back on-device today; whatever API comes next should
// expose them, because a previous boot's log is how a crash gets diagnosed.

#include <Arduino.h>

#include <stddef.h>

// Mounts LittleFS on the `storage` partition (formats on first use) and
// writes the boot marker (reset reason + build rev). Call once from
// setup(), before the first SerialPrint* and before tasksInit().
// A Print-derived sink so every type Serial can format (int, float, String,
// IPAddress, Printable, ...) tees to flash without a pile of template
// overloads in the SerialPrint helpers. Inherited from the in-RAM web log
// that used to sit in front of this tee; that ring existed only to answer
// GET /log and went with the HTTP layer.
class FlashLogPrinter : public Print {
 public:
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};

extern FlashLogPrinter flashLogPrinter;

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
