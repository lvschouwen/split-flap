#pragma once
// Self-documenting API index for the ESP-01 follower (#307/#308). Trimmed COPY
// of the master's ApiIndex.h (copy policy: keep the overlapping legend
// meanings byte-identical between trees). GET /api serves
// {"routes":[...],"legend":{...}} for the follower's route subset + the terse
// keys of its two terse endpoints (/units/health, /cluster/health).
#ifdef UNIT_TEST
  #include <cstddef>
  #include <cstdio>
  #include <cstring>
#else
  #include <Arduino.h>
  #include <stddef.h>
  #include <string.h>
#endif

struct ApiRoute { const char* m; const char* p; const char* d; };
struct ApiLegendEntry { const char* key; const char* meaning; };

static const ApiRoute API_ROUTES[] = {
  {"GET",  "/api",                   "this self-documenting index"},
  {"GET",  "/settings",              "tiny identity/rev/plat/width/phase/vitals"},
  {"GET",  "/log",                   "in-RAM log ring (?after=<cursor> for the leader pull)"},
  {"GET",  "/units/health",          "per-unit health/diagnostics table"},
  {"POST", "/units/health/refresh",  "re-poll unit health"},
  {"GET",  "/cluster/health",        "follower phase + diagnostics + unit health"},
  {"POST", "/cluster/join",          "leader assigns this row (leaderHost/row/epoch)"},
  {"POST", "/cluster/render",        "leader pushes a segment (epoch/seq/text)"},
  {"POST", "/cluster/ping",          "leader liveness ping"},
  {"POST", "/cluster/leave",         "leave the cluster (blank)"},
  {"POST", "/firmware/master",       "OTA this follower (?md5= required)"},
  {"POST", "/reflash-units",         "reflash every unit over twiboot"},
  {"POST", "/reboot",                "soft reboot the follower"},
  {"GET",  "/unit/offset",           "read a unit's calibration offset"},
  {"POST", "/unit/offset",           "set a unit's calibration offset"},
  {"POST", "/unit/jog",              "jog a unit N steps"},
  {"POST", "/unit/home",             "home a unit"},
  {"POST", "/unit/identify",         "blink a unit's LED"},
  {"POST", "/unit/reset-odometer",   "zero a unit's revolution odometer"},
  {"POST", "/unit/self-test",        "run a unit's self-test"},
  {"GET",  "/unit/self-test-result", "read a unit's self-test result"},
  {"POST", "/unit/reboot",           "reboot a unit"},
  {"GET",  "/unit/op-result",        "result of the last {seq} maintenance op"},
};
static const int API_ROUTES_COUNT = (int)(sizeof(API_ROUTES) / sizeof(API_ROUTES[0]));

// #358: routes the wider v2 surface serves but this platform deliberately
// does NOT (spec 2026-07-14-v2-esp01-follower-design.md "Not served, by
// design") — surfaced so tooling can distinguish "this platform doesn't do
// that" from "no such route" (both answer 404 on the wire). The
// tests/test_api_index.py drift gate asserts none of these is registered.
static const char* API_NOT_SERVED[] = {
  "/cluster/digest",
  "/cluster/promote",
  "/cluster/config",
  "/cluster/discover",
};
static const int API_NOT_SERVED_COUNT =
    (int)(sizeof(API_NOT_SERVED) / sizeof(API_NOT_SERVED[0]));

static const ApiLegendEntry API_LEGEND[] = {
  // --- /units/health headline + per-unit (must match the master's meanings) --
  {"width",  "display width in units"},
  {"faulty", "count of units flagged faulty"},
  {"vccMin", "lowest since-boot unit supply Vcc (mV) across the display — the brownout floor"},
  {"units",  "per-unit array, one entry per display column"},
  {"i",      "unit index (0-based column)"},
  {"a",      "I2C address"},
  {"st",     "unit state: 0 silent / 1 sketch / 2 bootloader"},
  {"v",      "1 = a CMD_GET_STATUS read succeeded"},
  {"fw",     "firmware vs bundle: 0 ok / 1 outdated / 2 unknown"},
  {"rev",    "firmware git short-rev"},
  {"up",     "uptime seconds (saturating)"},
  {"br",     "lifetime brownout reset count"},
  {"wd",     "lifetime watchdog reset count"},
  {"bc",     "bad-I2C-command count since boot"},
  {"mc",     "MCUSR reset-cause snapshot at boot"},
  {"fl",     "status flag bitfield (bit0 moving, bit1 home-failed, bit2 hall-never, bit4 addr-eeprom, bit5 homed)"},
  {"hs",     "last homing step count"},
  {"ae",     "1 = I2C address came from EEPROM, not DIP"},
  {"odo",    "drum revolution odometer"},
  {"de",     "drift events since boot"},
  {"ds",     "last drift magnitude in steps"},
  {"dp",     "1 = a drift re-home is pending"},
  {"phys",   "hall-corrected physical letter index"},
  {"mm",     "1 = physical letter disagrees with intended"},
  {"vcc",    "supply Vcc now (mV)"},
  {"vmin",   "since-boot minimum supply Vcc (mV), sampled mid-move"},
  {"cp",     "last commanded flap index"},
  {"ram",    "since-boot minimum free SRAM (bytes)"},
  {"age",    "ms since the last good scheduled health read (heartbeat freshness)"},
  {"hs2",    "boot-home state: 0 unhomed, 1 homing, 2 homed"},
  {"misses", "consecutive missed heartbeat reads"},
  {"stale",  "1 = unit missed >= the threshold of consecutive heartbeats (lost)"},
  {"se",     "step-excess on the last home (actual minus expected steps)"},
  {"sx",     "worst-seen step-excess since boot"},
  {"sag",    "minimum loaded supply Vcc (mV) during the last move"},
  {"he",     "hall edges seen in the last completed revolution"},
  {"dw",     "rolling ~60 s duty window (recent move count)"},
  {"sb",     "ext-diag status bitfield (bit0 last-move stall)"},
  // --- /cluster/health ---
  {"state",        "follower phase: standalone/clustered/grace/blank"},
  {"leaderName",   "name of the leader feeding this row"},
  {"leaderHost",   "host of the leader"},
  {"row",          "this follower's grid row"},
  {"epoch",        "leader epoch"},
  {"seq",          "last accepted render sequence number"},
  {"segment",      "the text currently held for this row"},
  {"detected",     "units responding on the bus"},
  {"msSinceRender","ms since the last leader render applied (-1 = none yet)"},
  {"secsUntilBlank","seconds until total-silence blanks the row (-1 = already blank)"},
  {"i2cTx",        "unit-bus read transactions since boot"},
  {"i2cErr",       "failed unit-bus reads since boot"},
  {"minHeap",      "since-boot minimum free heap (bytes)"},
  {"sntpSynced",   "1 = SNTP epoch synced (commitAt flips honored)"},
  {"hmac",         "1 = enforcing signed (HMAC) leader-wire requests (#313)"},
  {"foreign",      "refused foreign-leader contacts: joins/pings/renders counters + lastHost + msSince (-1 = never)"},
};
static const int API_LEGEND_COUNT = (int)(sizeof(API_LEGEND) / sizeof(API_LEGEND[0]));

inline bool legendHasKey(const char* key) {
  for (int i = 0; i < API_LEGEND_COUNT; i++) {
    if (strcmp(API_LEGEND[i].key, key) == 0) return true;
  }
  return false;
}

// #365 legend growth (se/sx/sag/he/dw/sb) pushed the full /api payload past
// the prior 4096 — bumped with headroom, same as the master's cap.
#define API_JSON_CAP 5120

#define API_APPEND(...) do { \
    if (o >= cap) return o; \
    o += (size_t)snprintf(buf + o, cap - o, __VA_ARGS__); \
  } while (0)

inline size_t buildApiJson(char* buf, size_t cap) {
  size_t o = 0;
  API_APPEND("{\"routes\":[");
  for (int i = 0; i < API_ROUTES_COUNT; i++) {
    API_APPEND("%s{\"m\":\"%s\",\"p\":\"%s\",\"d\":\"%s\"}", i == 0 ? "" : ",",
               API_ROUTES[i].m, API_ROUTES[i].p, API_ROUTES[i].d);
  }
  API_APPEND("],\"notServed\":[");
  for (int i = 0; i < API_NOT_SERVED_COUNT; i++) {
    API_APPEND("%s\"%s\"", i == 0 ? "" : ",", API_NOT_SERVED[i]);
  }
  API_APPEND("],\"legend\":{");
  for (int i = 0; i < API_LEGEND_COUNT; i++) {
    API_APPEND("%s\"%s\":\"%s\"", i == 0 ? "" : ",", API_LEGEND[i].key,
               API_LEGEND[i].meaning);
  }
  API_APPEND("}}");
  return o;
}
