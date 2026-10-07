#pragma once
// Self-documenting API index for the headless (curl-only) operator (#307).
// GET /api serves {"routes":[{"m","p","d"}...],"legend":{key:meaning,...}}.
// Pure + PROGMEM-free data tables so test_api can assert the legend covers
// every terse key buildUnitHealthJson emits (the legend can't silently drift
// from the data). The follower carries a trimmed copy (copy policy: keep the
// overlapping legend meanings identical between trees).
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

// Every operator-facing endpoint, method + path + one-line description.
// tests/test_api_index.py diffs this against the routes actually registered
// across the Web*.cpp family in BOTH directions (#448), so a served route is
// either listed here or named in that gate's deliberate-exclusion set — the
// browser-UI assets. Neither an
// undeclared endpoint nor a phantom one can survive CI.
static const ApiRoute API_ROUTES[] = {
  {"GET",  "/api",                    "this self-documenting index"},
  {"POST", "/api/v2/action",          "JSON {name,target,args}. Done when accepted, 200 {done}: show (args text, forS), mode, quiet, stop, restart (target row, none = this master). Jobs, 202 {op}: pair, release, arrange, update (offer a row board the stored image again), and the unit jobs (home, identify, jog, set-offset, self-test, restart-unit, reset-odometer, set-gates, boot-info, boot-dump, boot-update, update-units, probe, set-address, clear-address, home-all) with target {row,unit}"},
  {"GET",  "/api/v2/history",         "what happened on the wall, newest first: ?before=<seq> for the page after one (its \"next\"), ?limit=<1..50>"},
  {"GET",  "/api/v2/op",              "what became of a job: /api/v2/op/<id> (202 running, 200 finished, 404 unknown)"},
  {"GET",  "/api/v2/wall",            "the boards of this Split-Flap and what each row board last said"},
  {"GET",  "/settings",               "full device settings snapshot"},
  {"GET",  "/system/info",            "static hardware/partition inventory"},
  {"GET",  "/system/stats",           "live vitals + ~10 min history ring"},
  {"GET",  "/status",                 "one-shot aggregate: settings+stats.now+units+ota"},
  {"GET",  "/units/health",           "per-unit health/diagnostics table"},
  {"POST", "/units/health/refresh",   "re-probe the bus + re-poll health"},
  {"GET",  "/health",                 "liveness text"},
  {"GET",  "/log",                    "in-RAM log tail"},
  {"GET",  "/log/flash",              "persistent flash log"},
  {"GET",  "/units/odometer-log",     "append-only odometer history CSV: epoch,addr,revs[,R=reset] (?prev=1 = rotated file)"},
  {"POST", "/log/flash/clear",        "truncate the flash log"},
  {"GET",  "/tz.json",                "IANA timezone table"},
  {"GET",  "/events",                 "SSE display text stream"},
  {"POST", "/",                       "set display text / mode (per-card fields)"},
  {"POST", "/reboot",                 "soft reboot the master"},
  {"POST", "/stop",                   "blank + halt the display"},
  {"GET",  "/wifi-setup",             "WiFi portal page"},
  {"GET",  "/wifi/scan",              "last WiFi scan result"},
  {"POST", "/wifi/scan",              "start a WiFi scan"},
  {"POST", "/wifi/config",            "set WiFi credentials"},
  {"POST", "/reset-wifi",             "erase WiFi credentials"},
  {"POST", "/firmware/master",        "OTA the master (?md5= required)"},
  {"GET",  "/firmware/row",           "the stored row image, as the row boards fetch it"},
  {"POST", "/firmware/row",           "store a follower-<rev>.bin for the row boards (?md5= required); rows on another rev are offered it"},
  {"GET",  "/debug/ota",              "OTA/partition state"},
  {"POST", "/firmware/rescue",        "install the rescue image"},
  {"POST", "/firmware/rescue-boot",   "boot into the rescue slot"},
  {"GET",  "/unit/offset",            "read a unit's calibration offset"},
  {"POST", "/unit/offset",            "set a unit's calibration offset"},
  {"POST", "/unit/jog",               "jog a unit N steps"},
  {"POST", "/unit/home",              "home a unit"},
  {"POST", "/unit/identify",          "blink a unit's LED"},
  {"POST", "/unit/reset-odometer",    "zero a unit's revolution odometer"},
  {"POST", "/unit/gates",             "set a unit's feature-gate bits"},
  {"POST", "/unit/self-test",         "run a unit's self-test"},
  {"GET",  "/unit/self-test-result",  "read a unit's self-test result"},
  {"POST", "/unit/boot-dump",         "read a unit's bootloader section over I2C (unit restarts and re-homes)"},
  {"GET",  "/unit/boot-dump-result",  "boot-section dump result: crc32 + bytes as hex"},
  {"POST", "/unit/boot-update",       "in-system twiboot update: reads boot info, runs needed stages, verifies"},
  {"POST", "/unit/boot-info",         "read a unit's own boot-section report (no restart, nothing written)"},
  {"GET",  "/unit/boot-info-result",  "boot report: state, crc32, lock + fuse bytes, last update result"},
  {"POST", "/unit/reboot",            "reboot a unit"},
  {"POST", "/unit/set-address",       "burn a unit's EEPROM I2C address"},
  {"POST", "/unit/clear-address",     "clear a unit's EEPROM I2C address"},
  {"GET",  "/unit/op-result",         "result of the last {seq} maintenance op"},
  {"POST", "/reset-units",            "home every unit"},
  {"POST", "/reflash-units",          "update units: firmware, then bootloader (?address=N for one, &force=1 to reflash it regardless)"},
  {"POST", "/mqtt/discover",          "start an mDNS MQTT broker scan"},
  {"GET",  "/mqtt/discover",          "mDNS MQTT broker scan result"},
  {"GET",  "/coredump/summary",       "last-crash task + backtrace + dump ELF sha"},
  {"GET",  "/coredump/raw",           "raw ELF coredump for esp-coredump (#431)"},
  {"POST", "/coredump/erase",         "queue a coredump partition purge"},
};
static const int API_ROUTES_COUNT = (int)(sizeof(API_ROUTES) / sizeof(API_ROUTES[0]));

// Terse-key legend, covering /units/health and /system/stats.
// The /units/health block is machine-guarded by test_api against
// buildUnitHealthJson's actual output.
static const ApiLegendEntry API_LEGEND[] = {
  // --- /units/health headline + per-unit ---
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
  {"br",     "lifetime brownout reset count (history, not a fault by itself)"},
  {"wd",     "lifetime watchdog reset count (history, not a fault by itself)"},
  {"rs",     "1 = br or wd climbed while this master was watching: counted as faulty"},
  {"bc",     "bad-I2C-command count since boot"},
  {"mc",     "reset cause of the unit's last boot: MCUSR bits, +128 = the unit asked for it"},
  {"fl",     "status flag bitfield (bit0 moving, bit1 home-failed, bit2 hall-never, bit4 addr-eeprom, bit5 homed)"},
  {"hs",     "last homing step count"},
  {"ae",     "1 = I2C address came from EEPROM, not DIP"},
  {"odo",    "drum revolution odometer"},
  {"ofs",    "calibration offset in steps"},
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
  {"rsx",    "twiboot exits by the runtime lost-unit rescue since boot (#498)"},
  {"err",    "cumulative failed unit-bus transactions charged to this address"},
  {"errAge", "ms since this unit's last charged bus error"},
  {"se",     "step-excess on the last home (actual minus expected steps)"},
  {"sx",     "worst-seen step-excess since boot"},
  {"sag",    "minimum loaded supply Vcc (mV) during the last move"},
  {"he",     "hall edges seen in the last completed revolution"},
  {"dw",     "rolling ~60 s duty window (recent move count)"},
  {"sb",     "ext-diag status bitfield (bit0 last-move stall, bits1-3 TWI self-heals that freed the bus #489)"},
  {"ut",     "unit uptime in seconds since boot, full width (up saturates at 65535)"},
  {"rx",     "master writes this unit received since boot (u16, wraps; compare deltas)"},
  {"tx",     "master reads this unit answered since boot (u16, wraps; compare deltas)"},
  {"dh",     "TWI register self-check re-inits since boot (unit deafness self-heals)"},
  {"bv",     "bootloader verdict: 1 expected image, 2 known other image or update step, 3 corrupt (faulty)"},
  {"bcrc",   "boot-section crc32 the unit reported, only when it is not the expected image"},
  {"blv",    "unit in its bootloader: image version (1 = none reported, 255 = not recognised)"},
  {"blc",    "bootloader capability bits: 1 do_spm, 2 bounded pin, 4 crash record, 8 fuse bytes"},
  {"blk",    "lock byte from the bootloader (hex), when the chip serves it"},
  {"blf",    "low, high, extended fuse from the bootloader (hex), when the chip serves them"},
  {"blx",    "crash resets in a row per the bootloader, from 2 up; at 3 it holds the unit (faulty) until flashed"},
  {"pv",     "wire protocol version the unit reports"},
  {"pmm",    "1 = protocol version we do not speak; unit is untouched and is a reflash target"},
  {"hf",     "lifetime failed-homing count (survives power cycles)"},
  {"gates",  "active unit feature-gate bits (bit0 idle hall check; no other bit is implemented)"},
  {"sxl",    "worst-seen step-excess over the unit's lifetime (sx forgets at reboot)"},
  {"stw0",   "hall window measured by the unit's FIRST self-test (baseline)"},
  {"stw1",   "hall window measured by its most recent self-test"},
  {"str0",   "steps/rev measured by the unit's FIRST self-test (baseline)"},
  {"str1",   "steps/rev measured by its most recent self-test"},
  {"fr",     "idle hall re-homes this boot that found no drift (the window model, not the belief)"},
  {"frd",    "1 = the unit disarmed its own idle hall check; it is not protecting this unit"},
  // --- /system/stats (now object) ---
  {"rssi",     "WiFi RSSI (dBm)"},
  {"txPower",  "WiFi TX power cap x10 (dBm) — starts lowest, ramps one level at a time"},
  {"heap",     "free heap (bytes)"},
  {"maxAlloc", "largest allocatable heap block (bytes)"},
  {"psram",    "free PSRAM (bytes)"},
  {"cpu0",     "core 0 load percent"},
  {"cpu1",     "core 1 load percent"},
  {"temp",     "die temperature x10 (°C)"},
  {"uptime",   "uptime seconds"},
  {"minHeap",  "since-boot minimum free heap (bytes)"},
  {"i2cTx",    "unit-bus transactions since boot"},
  {"i2cErr",   "failed unit-bus transactions since boot"},
  {"mqttDrops","MQTT broker disconnects since boot"},
  {"ntpAge",   "seconds since last SNTP sync (-1 = never)"},
  {"reset",    "last reset reason"},
  {"hist",     "history ring of the spark series"},
  {"interval", "history sample interval (s)"},
};
static const int API_LEGEND_COUNT = (int)(sizeof(API_LEGEND) / sizeof(API_LEGEND[0]));

// Case-sensitive exact-match lookup. test_api uses it to prove the legend
// covers every key buildUnitHealthJson emits.
inline bool legendHasKey(const char* key) {
  for (int i = 0; i < API_LEGEND_COUNT; i++) {
    if (strcmp(API_LEGEND[i].key, key) == 0) return true;
  }
  return false;
}

// Sized to the reply (just over 10 KB with the bootloader legend rows) plus
// room for a few more; heap-built by the handler, so the cost is transient,
// not BSS.
#define API_JSON_CAP 11264

#define API_APPEND(...) do { \
    if (o >= cap) return o; \
    o += (size_t)snprintf(buf + o, cap - o, __VA_ARGS__); \
  } while (0)

// Serializes the routes + legend index. Returns the would-be length like
// snprintf; the caller rejects >= cap. Descriptions/meanings are curated
// literals here (no `"`/`\`), so they never break the JSON.
inline size_t buildApiJson(char* buf, size_t cap) {
  size_t o = 0;
  API_APPEND("{\"routes\":[");
  for (int i = 0; i < API_ROUTES_COUNT; i++) {
    API_APPEND("%s{\"m\":\"%s\",\"p\":\"%s\",\"d\":\"%s\"}", i == 0 ? "" : ",",
               API_ROUTES[i].m, API_ROUTES[i].p, API_ROUTES[i].d);
  }
  API_APPEND("],\"legend\":{");
  for (int i = 0; i < API_LEGEND_COUNT; i++) {
    API_APPEND("%s\"%s\":\"%s\"", i == 0 ? "" : ",", API_LEGEND[i].key,
               API_LEGEND[i].meaning);
  }
  API_APPEND("}}");
  return o;
}
