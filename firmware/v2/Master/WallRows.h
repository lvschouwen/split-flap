#pragma once
// WallRows.h — the boards that make up this Split-Flap, as the master stores
// them (#559/#566): its own row of units and the ESP-01 row boards paired
// with it, each with its place on the grid. Pure, natively tested by
// test_wall_rows.
//
// A row board is known by its id (the name it says in Hello). Its address is
// what the master posts a pairing to; the row dials the master, never the
// other way round. An entry without an id is the master's own row. An empty
// table is a master on its own: nothing about showing text changes.
//
// The grid rules (rows contiguous from 0, spans that do not overlap, boards
// side by side on one grid row) are ClusterLayout.h's; this header only adds
// who the boards are.

#include <Arduino.h>

#include "ClusterLayout.h"
#include "DeviceIdentity.h"
#include "LanOrigin.h"

#define WALL_ROW_ID_MAX 32  // Hello.id without its terminator

struct WallRowDef {
  char id[WALL_ROW_ID_MAX + 1] = {0};  // "" = the master's own row
  char host[CLUSTER_HOST_MAX_LEN + 1] = {0};
  uint8_t row = 0;
  uint8_t col = 0;
  uint8_t width = 0;
};

struct WallRowsTable {
  uint8_t count = 0;
  WallRowDef rows[CLUSTER_MAX_MEMBERS];
};

inline bool wallRowIsOwn(const WallRowDef& def) { return def.id[0] == 0; }

// A row's address is a private IPv4 address written the one plain way
// ("192.168.1.50"): it is compared with where the row's connection comes
// from, so a name, or a second spelling of the same address, would never
// match. On success `out` is the address in network byte order.
inline bool wallRowHostParse(const char* host, uint8_t out[4]) {
  int octet = 0;
  const char* p = host;
  for (;;) {
    if (*p < '0' || *p > '9') return false;
    if (*p == '0' && p[1] >= '0' && p[1] <= '9') return false;  // no leading zero
    int value = 0;
    while (*p >= '0' && *p <= '9') {
      value = value * 10 + (*p++ - '0');
      if (value > 255) return false;
    }
    out[octet++] = (uint8_t)value;
    if (octet == 4) break;
    if (*p++ != '.') return false;
  }
  return *p == 0 && lanPrivateIpv4(String(host));
}

// Index of the row board with this id, -1 when there is none.
inline int wallRowsFind(const WallRowsTable& table, const char* id) {
  if (id[0] == 0) return -1;
  for (int i = 0; i < table.count; i++) {
    if (strcmp(table.rows[i].id, id) == 0) return i;
  }
  return -1;
}

// Index of the master's own row, -1 when it drives no units of its own.
inline int wallRowsOwn(const WallRowsTable& table) {
  for (int i = 0; i < table.count; i++) {
    if (wallRowIsOwn(table.rows[i])) return i;
  }
  return -1;
}

// ---- stored form: id|host|row|col|width;... ------------------------------------

inline String wallRowsToString(const WallRowsTable& table) {
  String out;
  for (int i = 0; i < table.count; i++) {
    const WallRowDef& r = table.rows[i];
    if (i) out += ';';
    out += r.id;
    out += '|';
    out += r.host;
    out += '|';
    out += (int)r.row;
    out += '|';
    out += (int)r.col;
    out += '|';
    out += (int)r.width;
  }
  return out;
}

inline bool wallRowsParseByte(const String& field, uint8_t& out) {
  if (field.length() == 0 || field.length() > 3) return false;
  int value = 0;
  for (unsigned int i = 0; i < field.length(); i++) {
    if (field[i] < '0' || field[i] > '9') return false;
    value = value * 10 + (field[i] - '0');
  }
  if (value > 255) return false;
  out = (uint8_t)value;
  return true;
}

// The shape only; wallRowsValidate judges what it says. False on a damaged
// string. "" is an empty table.
inline bool wallRowsFromString(const String& stored, WallRowsTable& out) {
  out = WallRowsTable{};
  if (stored.length() == 0) return true;
  int start = 0;
  for (;;) {
    int end = stored.indexOf(';', start);
    const bool last = end < 0;
    if (last) end = stored.length();
    if (out.count >= CLUSTER_MAX_MEMBERS) return false;
    const String entry = stored.substring(start, end);
    int bar[4];
    int from = 0;
    for (int i = 0; i < 4; i++) {
      bar[i] = entry.indexOf('|', from);
      if (bar[i] < 0) return false;
      from = bar[i] + 1;
    }
    if (entry.indexOf('|', from) >= 0) return false;
    const String id = entry.substring(0, bar[0]);
    const String host = entry.substring(bar[0] + 1, bar[1]);
    if (id.length() > WALL_ROW_ID_MAX || host.length() > CLUSTER_HOST_MAX_LEN) return false;
    WallRowDef& def = out.rows[out.count];
    if (!wallRowsParseByte(entry.substring(bar[1] + 1, bar[2]), def.row) ||
        !wallRowsParseByte(entry.substring(bar[2] + 1, bar[3]), def.col) ||
        !wallRowsParseByte(entry.substring(bar[3] + 1), def.width)) {
      return false;
    }
    strcpy(def.id, id.c_str());
    strcpy(def.host, host.c_str());
    out.count++;
    if (last) return true;
    start = end + 1;
  }
}

// ---- the grid ------------------------------------------------------------------

// The same boards as ClusterLayout.h wants them: it tells the own row from the
// others by an empty host, and uses nothing else of it.
inline ClusterMemberTable wallRowsLayout(const WallRowsTable& table) {
  ClusterMemberTable out;
  out.count = table.count;
  for (int i = 0; i < table.count; i++) {
    const WallRowDef& r = table.rows[i];
    ClusterMemberDef& m = out.members[i];
    if (!wallRowIsOwn(r)) strcpy(m.host, "row");
    m.row = r.row;
    m.col = r.col;
    m.width = r.width;
  }
  return out;
}

// ok=false carries the message for the operator. An empty table is not judged
// here: it is a master on its own, which needs no grid.
inline ClusterVerdict wallRowsValidate(const WallRowsTable& table, ClusterGrid& outGrid) {
  bool own = false;
  for (int i = 0; i < table.count; i++) {
    const WallRowDef& r = table.rows[i];
    if (wallRowIsOwn(r)) {
      if (own) return {false, "Only one row can be the master's own"};
      own = true;
      if (r.host[0] != 0) return {false, "The master's own row has no address"};
      continue;
    }
    if (!isValidDeviceName(String(r.id))) return {false, "A row id is not a board name"};
    // The master posts to this address when it pairs: the local network only.
    uint8_t address[4];
    if (!wallRowHostParse(r.host, address)) {
      return {false, "A row's address is not on the local network"};
    }
    for (int j = 0; j < i; j++) {
      if (strcmp(table.rows[j].id, r.id) == 0) return {false, "Two rows carry the same id"};
    }
  }
  return validateMemberTable(wallRowsLayout(table), outGrid);
}

// ---- changing the table ------------------------------------------------------------
//
// Each builds the whole new table in `out` and judges it; on a refusal `out`
// is not to be used.

// Where a newly paired row goes; -1 = the default (below the last row, at the
// left edge, as wide as the row says it is).
struct WallRowPlace {
  int16_t row = -1;
  int16_t col = -1;
  int16_t width = -1;
};

// A row answered the pairing with its id and its unit count. A row already in
// the table keeps its place and takes the new address. The first row paired
// brings the master's own units (`ownWidth`, 0 = none) in as grid row 0.
inline ClusterVerdict wallRowsWithPaired(const WallRowsTable& table, const char* id,
                                         const char* host, int reportedWidth,
                                         const WallRowPlace& place, int ownWidth,
                                         WallRowsTable& out) {
  WallRowsTable next = table;
  ClusterGrid grid;
  if (strlen(id) > WALL_ROW_ID_MAX || strlen(host) > CLUSTER_HOST_MAX_LEN) {
    return {false, "A row id is not a board name"};
  }
  if (next.count == 0 && ownWidth > 0) {
    next.rows[0] = WallRowDef{};
    next.rows[0].width = (uint8_t)ownWidth;
    next.count = 1;
  }
  int at = wallRowsFind(next, id);
  if (at < 0) {
    if (next.count >= CLUSTER_MAX_MEMBERS) return {false, "A Split-Flap has at most 8 boards"};
    int below = 0;
    for (int i = 0; i < next.count; i++) {
      if (next.rows[i].row + 1 > below) below = next.rows[i].row + 1;
    }
    const int row = place.row >= 0 ? place.row : below;
    const int col = place.col >= 0 ? place.col : 0;
    const int width = place.width >= 0 ? place.width : reportedWidth;
    if (width <= 0) return {false, "A row without units has no place on the wall"};
    if (row > 255 || col > 255 || width > 255) return {false, "Row index beyond the member cap"};
    at = next.count++;
    next.rows[at] = WallRowDef{};
    strcpy(next.rows[at].id, id);
    next.rows[at].row = (uint8_t)row;
    next.rows[at].col = (uint8_t)col;
    next.rows[at].width = (uint8_t)width;
  }
  strcpy(next.rows[at].host, host);
  const ClusterVerdict verdict = wallRowsValidate(next, grid);
  if (verdict.ok) out = next;
  return verdict;
}

// The table without that row; rows below a grid row it leaves empty move up.
// With only the master's own row left it is a master on its own again: the
// empty table.
inline ClusterVerdict wallRowsWithout(const WallRowsTable& table, const char* id,
                                      WallRowsTable& out) {
  const int at = wallRowsFind(table, id);
  if (at < 0) return {false, "No such row"};
  const uint8_t gone = table.rows[at].row;
  WallRowsTable next;
  bool gridRowStillUsed = false;
  for (int i = 0; i < table.count; i++) {
    if (i == at) continue;
    next.rows[next.count++] = table.rows[i];
    if (table.rows[i].row == gone) gridRowStillUsed = true;
  }
  // A grid row left empty closes up, so the wall stays contiguous.
  for (int i = 0; i < next.count && !gridRowStillUsed; i++) {
    if (next.rows[i].row > gone) next.rows[i].row--;
  }
  if (next.count == 0 || (next.count == 1 && wallRowIsOwn(next.rows[0]))) {
    out = WallRowsTable{};
    return {true, ""};
  }
  ClusterGrid grid;
  const ClusterVerdict verdict = wallRowsValidate(next, grid);
  if (verdict.ok) out = next;
  return verdict;
}
