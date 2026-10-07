// WallPair.cpp — contract in WallPair.h.
#include "WallPair.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_http_client.h>

#include "HelpersSerialHandling.h"
#include "Tasks.h"
#include "WallState.h"

namespace {

// A row in a unit job answers late; the task watchdog (30 s) bounds this.
constexpr int PAIR_HTTP_TIMEOUT_MS = 5000;
// How long the link task gets to say Release before the row is dropped from
// the table anyway (it passes every 20 ms).
constexpr uint32_t RELEASE_WAIT_MS = 2000;

String masterName;
uint32_t lastRepairMs[CLUSTER_MAX_MEMBERS] = {0};

// POST master=<id> to the row's /pair. -1 = no answer; else the HTTP status,
// with the start of the body in `body`.
int postPair(const char* host, uint16_t port, char* body, size_t cap) {
  body[0] = 0;
  uint8_t address[4];
  if (!wallRowHostParse(host, address)) return -1;  // the local network only
  const String url = String("http://") + host + ":" + port + "/pair";
  const String form = "master=" + masterName;  // a board name: nothing to escape
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.timeout_ms = PAIR_HTTP_TIMEOUT_MS;
  cfg.disable_auto_redirect = true;  // never follow a redirect off the LAN
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) return -1;
  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
  int status = -1;
  if (esp_http_client_open(client, form.length()) == ESP_OK &&
      esp_http_client_write(client, form.c_str(), form.length()) == (int)form.length() &&
      esp_http_client_fetch_headers(client) >= 0) {
    status = esp_http_client_get_status_code(client);
    const int got = esp_http_client_read_response(client, body, cap - 1);
    body[got > 0 ? got : 0] = 0;
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return status;
}

// `board`: the row the job was about, when it has a name by then.
void finish(const WallRequest& request, bool ok, const String& detail,
            const char* board = nullptr) {
  SerialPrintf("wall: job %u %s: %s\n", (unsigned)request.opId, ok ? "done" : "failed",
               detail.c_str());
  wallOpFinish(request.opId, ok, detail.c_str(), board);
}

void runPair(const WallRequest& request) {
  static char body[640];
  const int status = postPair(request.host, request.port, body, sizeof(body));
  if (status < 0) return finish(request, false, "no answer from the row");
  JsonDocument doc;
  const bool json = deserializeJson(doc, body) == DeserializationError::Ok;
  if (status == 409) {
    const char* other = json ? doc["master"].as<const char*>() : nullptr;
    return finish(request, false, String("the row obeys another master: ") + (other ? other : "?"));
  }
  if (status != 200 || !json) {
    return finish(request, false, String("the row answered ") + status);
  }
  const char* id = doc["name"].as<const char*>();
  const char* plat = doc["plat"].as<const char*>();
  if (id == nullptr || id[0] == 0 || plat == nullptr || strcmp(plat, "esp01") != 0) {
    return finish(request, false, "that board is not a row board");
  }
  uint32_t generation;
  const WallRowsTable table = wallStateRows(generation);
  // The name in the answer is the board's own word. It must not move a row
  // that is connected right now to wherever that answer came from.
  const int known = wallRowsFind(table, id);
  if (known >= 0) {
    const WallRowLink link = wallStateGet().link[known];
    if (link.contact.connected && strcmp(link.address, request.host) != 0) {
      return finish(request, false,
                    String("a row of that name is connected from ") + link.address);
    }
  }
  WallRowsTable next;
  ClusterVerdict verdict = wallRowsWithPaired(table, id, request.host, doc["width"] | 0,
                                              request.place, displaySnapshotGet().displayWidth,
                                              next);
  if (verdict.ok) verdict = wallStateSetRows(next);
  // On a refusal the row stays paired with this master but is in no table: it
  // frees itself by the lost-master rule (120 s).
  if (!verdict.ok) return finish(request, false, verdict.message, id);
  finish(request, true, id, id);
}

void runRelease(const WallRequest& request) {
  uint32_t generation;
  const WallRowsTable table = wallStateRows(generation);
  WallRowsTable next;
  const ClusterVerdict verdict = wallRowsWithout(table, request.id, next);
  if (!verdict.ok) return finish(request, false, verdict.message);
  // The row is told while it is still in the table: afterwards it is nobody.
  wallStateAskRelease(request.id);
  const uint32_t askedMs = millis();
  while (wallStateReleasePending() && millis() - askedMs < RELEASE_WAIT_MS) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  wallStateReleaseAnswered();
  const ClusterVerdict stored = wallStateSetRows(next);
  finish(request, stored.ok, stored.ok ? request.id : stored.message, request.id);
}

void runArrange(const WallRequest& request) {
  const ClusterVerdict verdict = wallStateSetRows(request.table);
  finish(request, verdict.ok, verdict.ok ? "arranged" : verdict.message);
}

// A row the link has lost is posted the pairing again, once per lost mark.
void repairLostRows() {
  if (WiFi.status() != WL_CONNECTED) return;
  const WallSnapshot wall = wallStateGet();
  const uint32_t nowMs = millis();
  for (int i = 0; i < wall.rows.count; i++) {
    const WallRowDef& row = wall.rows.rows[i];
    if (wallRowIsOwn(row)) continue;
    const WallRowReach reach = wallRowReach(wall.link[i].contact, nowMs);
    const bool lost = reach == WallRowReach::Lost ||
                      (reach == WallRowReach::Never && nowMs >= WALL_LINK_LOST_MS);
    if (!lost || !wallLinkElapsed(nowMs, lastRepairMs[i], WALL_LINK_LOST_MS)) continue;
    lastRepairMs[i] = nowMs;
    static char body[640];
    const int status = postPair(row.host, 80, body, sizeof(body));
    SerialPrintf("wall: %s is lost, pairing posted again to %s: %d\n", row.id, row.host, status);
    return;  // one blocking call per pass
  }
}

}  // namespace

void wallPairInit(const String& masterId) { masterName = masterId; }

void wallPairTick() {
  WallRequest request;
  if (wallStateTakeRequest(request)) {
    switch (request.kind) {
      case WallRequestKind::Pair: runPair(request); break;
      case WallRequestKind::Release: runRelease(request); break;
      case WallRequestKind::Arrange: runArrange(request); break;
      case WallRequestKind::None: break;
    }
    wallStateRequestDone();
    return;
  }
  repairLostRows();
}
