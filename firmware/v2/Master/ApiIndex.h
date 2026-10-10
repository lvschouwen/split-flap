#pragma once
// Self-documenting API index for the curl-only operator (#307).
// GET /api serves {"routes":[{"m","p","d"}...]}. Pure + PROGMEM-free data so
// it is natively testable.
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

// Every operator-facing endpoint, method + path + one-line description.
// tests/test_api_index.py diffs this against the routes actually registered
// across the Web*.cpp family in BOTH directions (#448), so a served route is
// either listed here or named in that gate's deliberate-exclusion set (the
// page and its icon). Neither an undeclared endpoint nor a phantom one can
// survive CI.
static const ApiRoute API_ROUTES[] = {
  {"GET",  "/api",                    "this self-documenting index"},
  {"POST", "/api/v2/action",          "JSON {name,target,args}. Done when accepted, 200 {done}: show (args text, forS), mode, quiet, stop, restart (target row, none = this master), forget-wifi. Jobs, 202 {op}: pair, release, arrange, update (offer a row board the stored image again), find-rows (data: row boards to pair), check-release (look for a release now), update-from-release (install the release that was found: rescue image, row image, master, then a restart), and the unit jobs (home, identify, jog, set-offset, self-test, restart-unit, reset-odometer, set-gates, boot-info, boot-dump, boot-update, update-units, probe, set-address, clear-address, home-all) with target {row,unit}"},
  {"GET",  "/api/v2/board",           "one board: /api/v2/board/<id> (a row board's id, or the master's name): place, verdict, how it is reached, what it runs, its vitals, and its units as a table {fields, rows}"},
  {"GET",  "/api/v2/firmware",        "should be / is for the whole wall: boards, the stored row image (rowImageHeld: one a release stored, waiting for this master to run it), unit firmware, bootloaders, the rescue image; release: what the look for a release found (state not-looked|up-to-date|newer|failed, tag, notes, the revs it would install) and the update that runs (step, done, size)"},
  {"GET",  "/api/v2/history",         "what happened on the wall, newest first: ?before=<seq> for the page after one (its \"next\"), ?limit=<1..50>"},
  {"GET",  "/api/v2/log",             "a board's raw log as text: ?row=<id> (none = the master), ?kind=ram|flash (flash: the master only, &prev=1 for the file before); headers X-Log-Board and X-Log-Kind say what it is; a row board's log arrives while it is being read, so read again"},
  {"GET",  "/api/v2/op",              "what became of a job: /api/v2/op/<id> (202 running, 200 finished, 404 unknown)"},
  {"GET",  "/api/v2/settings/board",  "a board's own settings: /api/v2/settings/board/<master's name> -> {name, unitCount}; a row board has none ({})"},
  {"PUT",  "/api/v2/settings/board",  "change them: any part of what the GET gives -> {done, restart}"},
  {"GET",  "/api/v2/settings/wall",   "the wall's settings: mode, quiet, alignment, speed, timezone (POSIX rule), updateUnitsAtStart, releaseCheck (the daily look), releaseChannel (stable|test), mqtt {host, port, user, passwordSet}"},
  {"PUT",  "/api/v2/settings/wall",   "change them: any part of what the GET gives (mqtt.password is write-only) -> {done, restart}; 400 names the key that was refused"},
  {"GET",  "/api/v2/stream",          "server-sent events (ask with Accept: text/event-stream), each topic when it changes and all of them to a new reader: wall (mode, quiet, every row's text), verdict (the wall's and every board's), jobs (the job table), history (the newest seq)"},
  {"GET",  "/api/v2/unit",            "one unit: /api/v2/unit/<board id>/<address>: its verdict with every reason, and its facts grouped as firmware, power, link, drum, bootloader (a flap is its place on the drum, 0 = blank)"},
  {"GET",  "/api/v2/wall",            "the boards of this Split-Flap and what each row board last said"},
  {"GET",  "/settings",               "what this board is and runs, as the flashing tools read it (the row board and the rescue image answer the same path)"},
  {"GET",  "/health",                 "liveness text"},
  {"GET",  "/units/odometer-log",     "append-only odometer history CSV: epoch,addr,revs[,R=reset] (?prev=1 = rotated file)"},
  {"GET",  "/tz.json",                "IANA timezone table"},
  {"GET",  "/wifi-setup",             "WiFi portal page"},
  {"GET",  "/wifi/scan",              "last WiFi scan result"},
  {"POST", "/wifi/scan",              "start a WiFi scan"},
  {"POST", "/wifi/config",            "set WiFi credentials"},
  {"POST", "/firmware/master",        "OTA the master (?md5= required)"},
  {"GET",  "/firmware/row",           "the stored row image, as the row boards fetch it"},
  {"POST", "/firmware/row",           "store a follower-<rev>.bin for the row boards (?md5= required); rows on another rev are offered it"},
  {"GET",  "/debug/ota",              "OTA/partition state"},
  {"POST", "/firmware/rescue",        "install the rescue image"},
  {"POST", "/firmware/rescue-boot",   "boot into the rescue slot"},
  {"GET",  "/coredump/summary",       "last-crash task + backtrace + dump ELF sha"},
  {"GET",  "/coredump/raw",           "raw ELF coredump for esp-coredump (#431)"},
  {"POST", "/coredump/erase",         "queue a coredump partition purge"},
};
static const int API_ROUTES_COUNT = (int)(sizeof(API_ROUTES) / sizeof(API_ROUTES[0]));

// Sized to the reply (just over 4 KB) plus room for a few more routes;
// heap-built by the handler, so the cost is transient, not BSS.
#define API_JSON_CAP 6144

#define API_APPEND(...) do { \
    if (o >= cap) return o; \
    o += (size_t)snprintf(buf + o, cap - o, __VA_ARGS__); \
  } while (0)

// Appends `text` at `o` as the inside of a JSON string, `"` and `\` escaped,
// and returns where it ends. Like snprintf it keeps counting past `cap` and
// writes only what fits, always terminated.
inline size_t apiJsonEscape(char* buf, size_t cap, size_t o, const char* text) {
  for (const char* c = text; *c != 0; c++) {
    if (*c == '"' || *c == '\\') {
      if (o + 1 < cap) buf[o] = '\\';
      o++;
    }
    if (o + 1 < cap) buf[o] = *c;
    o++;
  }
  if (cap > 0) buf[o < cap ? o : cap - 1] = 0;
  return o;
}

// Serializes the route index. Returns the would-be length like snprintf; the
// caller rejects >= cap.
inline size_t buildApiJson(char* buf, size_t cap) {
  size_t o = 0;
  API_APPEND("{\"routes\":[");
  for (int i = 0; i < API_ROUTES_COUNT; i++) {
    API_APPEND("%s{\"m\":\"%s\",\"p\":\"%s\",\"d\":\"", i == 0 ? "" : ",",
               API_ROUTES[i].m, API_ROUTES[i].p);
    o = apiJsonEscape(buf, cap, o, API_ROUTES[i].d);
    API_APPEND("\"}");
  }
  API_APPEND("]}");
  return o;
}
