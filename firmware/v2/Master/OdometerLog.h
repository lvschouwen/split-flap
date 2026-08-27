#pragma once
// OdometerLog — append-only per-unit odometer history on the `storage`
// LittleFS (#465). The unit's EEPROM is authoritative; this is the
// historian, recording what the master read at probe/health-poll time so a
// fleet sweep or EEPROM re-layout no longer erases the only copy of the
// wear data (`WearPolicy.h`: nobody has real 28BYJ-48 drum-life numbers —
// these 21 units are the first collection of them).
//
// Ownership: odometerLogTick() runs on netTask ONLY (the storage
// partition's sole flash writer, Hard rules) and reads the mutex-copied
// DisplaySnapshot, never the live bus arrays. Decisions and the row format
// are pure OdometerLogPolicy.h (natively tested).
//
// Files: /odolog.csv (current) -> /odolog.prev.csv at the size cap.
// Web: GET /units/odometer-log [?prev=1] (WebSystem.cpp).

// netTask only: self-throttled (~60 s); appends rows per policy and
// rotates at the cap. No-op until the storage mount (flashLogInit) is up.
void odometerLogTick();

// True when the storage mount succeeded (web layer's 503 gate).
bool odometerLogAvailable();

// Absolute LittleFS paths for the web layer's file responses.
const char* odometerLogCurrentPath();
const char* odometerLogPreviousPath();
