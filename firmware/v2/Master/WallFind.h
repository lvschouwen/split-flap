#pragma once
// WallFind.h — the row boards that answered a search of the network and can
// be paired with this master (#559/#574). Pure, natively tested by
// test_wall_find. The search itself (mDNS, `_splitflap._tcp`) is WebWall.cpp's.
//
// A board is listed when it says it is a row board, has a name and an
// address a pairing can be posted to, and is not on this wall already. A row
// that obeys another master is listed too: the search cannot know, and the
// pairing is what refuses.
//
// What the search hands back (GET /api/v2/op/<id>, "data"):
//   {"boards":[{"id":"split-flap-261bb6","address":"192.168.1.50",
//               "rev":"de38289","units":5}]}

#include <ArduinoJson.h>

#include "WallRows.h"

#define WALL_FIND_MAX CLUSTER_MAX_MEMBERS
// The platform a row board names in its mDNS record.
#define WALL_FIND_ROW_PLAT "esp01"

struct WallFoundBoard {
  String id;  // the name it would say in Hello
  String address;
  String rev;
  String plat;
  int units = 0;
};

struct WallFound {
  uint8_t count = 0;
  WallFoundBoard boards[WALL_FIND_MAX];
};

// Keeps `board` when it can be paired and is not held yet (a board may
// answer a search twice). False when it was left out.
inline bool wallFoundAdd(WallFound& found, const WallFoundBoard& board,
                         const WallRowsTable& table) {
  uint8_t address[4];
  if (board.plat != WALL_FIND_ROW_PLAT) return false;
  if (board.id.length() == 0 || board.id.length() > WALL_ROW_ID_MAX) return false;
  if (!wallRowHostParse(board.address.c_str(), address)) return false;
  if (wallRowsFind(table, board.id.c_str()) >= 0) return false;
  for (int i = 0; i < found.count; i++) {
    if (found.boards[i].id == board.id) return false;
  }
  if (found.count >= WALL_FIND_MAX) return false;
  found.boards[found.count++] = board;
  return true;
}

// Writes the document into `out`; its length, 0 when it does not fit.
inline size_t wallFoundJson(const WallFound& found, char* out, size_t cap) {
  JsonDocument doc;
  JsonArray boards = doc["boards"].to<JsonArray>();
  for (int i = 0; i < found.count; i++) {
    const WallFoundBoard& b = found.boards[i];
    JsonObject o = boards.add<JsonObject>();
    o["id"] = b.id;
    o["address"] = b.address;
    o["rev"] = b.rev;
    o["units"] = b.units;
  }
  const size_t need = measureJson(doc);
  if (need >= cap) return 0;
  return serializeJson(doc, out, cap);
}
