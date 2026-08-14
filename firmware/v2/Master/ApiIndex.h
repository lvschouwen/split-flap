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
// browser-UI assets and the server-to-server cluster wire. Neither an
// undeclared endpoint nor a phantom one can survive CI.
static const ApiRoute API_ROUTES[] = {
  {"GET",  "/api",                    "this self-documenting index"},
  {"GET",  "/settings",               "full device + cluster settings snapshot"},
  {"GET",  "/system/info",            "static hardware/partition inventory"},
  {"GET",  "/system/stats",           "live vitals + ~10 min history ring"},
  {"GET",  "/status",                 "one-shot aggregate: settings+stats.now+units+cluster+ota"},
  {"GET",  "/units/health",           "per-unit health/diagnostics table"},
  {"POST", "/units/health/refresh",   "re-probe the bus + re-poll health"},
  {"GET",  "/health",                 "liveness text"},
  {"GET",  "/log",                    "in-RAM log tail"},
  {"GET",  "/log/flash",              "persistent flash log"},
  {"POST", "/log/flash/clear",        "truncate the flash log"},
  {"GET",  "/tz.json",                "IANA timezone table"},
  {"GET",  "/events",                 "SSE display + cluster-wall stream"},
  {"POST", "/",                       "set display text / mode (per-card fields)"},
  {"POST", "/reboot",                 "soft reboot the master"},
  {"POST", "/stop",                   "blank + halt the display"},
  {"GET",  "/wifi-setup",             "WiFi portal page"},
  {"GET",  "/wifi/scan",              "last WiFi scan result"},
  {"POST", "/wifi/scan",              "start a WiFi scan"},
  {"POST", "/wifi/config",            "set WiFi credentials"},
  {"POST", "/reset-wifi",             "erase WiFi credentials"},
  {"POST", "/firmware/master",        "OTA the master (?md5= required)"},
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
  {"POST", "/unit/reboot",            "reboot a unit"},
  {"POST", "/unit/set-address",       "burn a unit's EEPROM I2C address"},
  {"POST", "/unit/clear-address",     "clear a unit's EEPROM I2C address"},
  {"GET",  "/unit/op-result",         "result of the last {seq} maintenance op"},
  {"POST", "/reset-units",            "home every unit"},
  {"POST", "/reflash-units",          "reflash units over twiboot (?address=N for one)"},
  {"POST", "/mqtt/discover",          "start an mDNS MQTT broker scan"},
  {"GET",  "/mqtt/discover",          "mDNS MQTT broker scan result"},
  {"POST", "/cluster/config",         "set cluster leader member table"},
  {"GET",  "/cluster/status",         "cluster leader supervision status"},
  {"GET",  "/cluster/health",         "this board's follower/unit health"},
  {"GET",  "/cluster/digest",         "cluster-wide digest (follower copy)"},
  {"POST", "/cluster/promote",        "promote this follower to leader"},
  {"POST", "/cluster/leave",          "drop this board's cluster membership"},
  {"POST", "/cluster/discover",       "start a cluster mDNS scan"},
  {"GET",  "/cluster/discover",       "cluster mDNS scan result"},
  {"POST", "/cluster/follower-firmware", "store an ESP-01 follower image for relay"},
  {"GET",  "/coredump/summary",       "last-crash task + backtrace + dump ELF sha"},
  {"GET",  "/coredump/raw",           "raw ELF coredump for esp-coredump (#431)"},
  {"POST", "/coredump/erase",         "queue a coredump partition purge"},
};
static const int API_ROUTES_COUNT = (int)(sizeof(API_ROUTES) / sizeof(API_ROUTES[0]));

// Terse-key legend, covering /units/health, /system/stats and /cluster/status.
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
  {"pv",     "wire protocol version the unit reports"},
  {"pmm",    "1 = protocol version we do not speak; unit is untouched and is a reflash target"},
  {"hf",     "lifetime failed-homing count (survives power cycles)"},
  {"gates",  "active unit feature-gate bits (bit0 idle hall check, bit1 scheduled re-home)"},
  {"sxl",    "worst-seen step-excess over the unit's lifetime (sx forgets at reboot)"},
  {"stw0",   "hall window measured by the unit's FIRST self-test (baseline)"},
  {"stw1",   "hall window measured by its most recent self-test"},
  {"str0",   "steps/rev measured by the unit's FIRST self-test (baseline)"},
  {"str1",   "steps/rev measured by its most recent self-test"},
  // --- /system/stats (now object) ---
  {"rssi",     "WiFi RSSI (dBm)"},
  {"heap",     "free heap (bytes)"},
  {"maxAlloc", "largest allocatable heap block (bytes)"},
  {"psram",    "free PSRAM (bytes)"},
  {"cpu0",     "core 0 load percent"},
  {"cpu1",     "core 1 load percent"},
  {"temp",     "die temperature x10 (°C)"},
  {"uptime",   "uptime seconds"},
  {"minHeap",  "lifetime minimum free heap (bytes)"},
  {"i2cTx",    "unit-bus transactions since boot"},
  {"i2cErr",   "failed unit-bus transactions since boot"},
  {"mqttDrops","MQTT broker disconnects since boot"},
  {"ntpAge",   "seconds since last SNTP sync (-1 = never)"},
  {"reset",    "last reset reason"},
  {"hist",     "history ring of the spark series"},
  {"interval", "history sample interval (s)"},
  // --- /cluster/status ---
  {"enabled",   "cluster leader mode on"},
  {"epoch",     "leader epoch"},
  {"seq",       "render sequence number"},
  {"members",   "leader member table"},
  {"host",      "member host (empty = own row)"},
  {"self",      "1 = this board's own row"},
  {"row",       "grid row"},
  {"col",       "grid column offset"},
  {"joined",    "member has joined"},
  {"degraded",  "member marked degraded (30 s without a successful contact, #385)"},
  {"suspect",   "member failing contacts but not yet degraded (#385 quiet tier)"},
  {"renderStuck", "member alive but its segment undeliverable for 30 s (#385)"},
  {"failures",  "contact failures since the last success"},
  {"plat",      "member platform (esp01/esp32; absent = leader's)"},
  {"role",      "member deviceRole (#332 tiers; absent = pre-#332 peer, keeps the old width-0-preferred slot)"},
  {"rollout",   "fleet firmware rollout state"},
  {"updating",  "a member firmware push is in flight"},
  {"updateBlocked", "rollout gave up after the attempt cap"},
  {"hmac",      "leader is signing wire-auth (HMAC) to this member (#313)"},
  {"imageVerifyFailed", "the running image failed self-verify for streaming"},
  {"followerImage", "a stored ESP-01 follower image is present"},
  {"followerPush",  "an ESP-01 follower image push is in flight"},
  {"gen",       "grid generation counter"},
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

#define API_JSON_CAP 8192

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
