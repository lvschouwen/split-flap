#pragma once
// WebWallJson.h — the pieces of a board's JSON that more than one /api/v2
// route gives (#559/#572): its verdict, and what the link knows about a row
// board. Include from WebWall.cpp and WebBoard.cpp only.

#include <ArduinoJson.h>

#include "JsonCopied.h"
#include "WallState.h"
#include "WallWatch.h"

// A board's verdict: {"level","reason","a","b","also":[...]} and, when its
// units are judged, "unitLevels": one letter a unit in bus order (w working,
// n note, f fault). The reasons and what their two numbers mean are
// BoardVerdict.h's.
inline void writeVerdict(JsonObject into, const WallVerdictBoard* board) {
  if (board == nullptr) return;
  JsonObject v = into["verdict"].to<JsonObject>();
  v["level"] = verdictLevelName(board->verdict.level);
  v["reason"] = boardReasonName(board->verdict.reason);
  v["a"] = board->verdict.a;
  v["b"] = board->verdict.b;
  JsonArray also = v["also"].to<JsonArray>();
  for (BoardReason r : BOARD_REASON_ORDER) {
    if (r != board->verdict.reason && (board->verdict.all & boardReasonBit(r))) {
      also.add(boardReasonName(r));
    }
  }
  if (board->units == 0) return;
  char levels[UNITS_AMOUNT + 1] = {0};
  for (int i = 0; i < board->units && i < UNITS_AMOUNT; i++) {
    levels[i] = verdictLevelName(board->unit[i].level)[0];
  }
  into["unitLevels"] = jsonCopied(levels);
}

// What the link knows about a row board: where it is, how it is reached, what
// it runs, and the vitals it last reported.
inline void writeRowLink(JsonObject row, const WallRowDef& def, const WallRowLink& link,
                         uint32_t nowMs) {
  row["pairedAt"] = jsonCopied(def.host);
  row["reach"] = wallRowReachName(wallRowReach(link.contact, nowMs));
  row["connects"] = link.connects;
  row["restarts"] = link.restarts;
  if (link.contact.everHeard) row["heardMsAgo"] = (uint32_t)(nowMs - link.contact.lastHeardMs);
  if (!link.everWelcomed) return;
  row["address"] = jsonCopied(link.address);
  row["rev"] = jsonCopied(link.rev);
  row["rescue"] = link.rescue;
  row["units"] = link.reportedWidth;
  row["textShown"] = link.textApplied;
  row["shownCount"] = link.shownCount;
  row["lastLateMs"] = link.lastLateMs;
  row["worstLateMs"] = link.worstLateMs;
  row["updateAttempts"] = link.updateAttempts;
  row["updateBlocked"] = link.updateBlocked;
  if (link.haveStatus) {
    JsonObject s = row["status"].to<JsonObject>();
    s["uptimeS"] = link.status.up_s;
    s["heap"] = link.status.heap;
    s["heapMin"] = link.status.min_heap;
    s["heapLargestBlock"] = link.status.max_block;
    s["heap2"] = link.status.heap2;
    s["rssi"] = link.status.rssi;
    s["txPowerDbm"] = link.status.tx_power / 4.0f;
    s["busTx"] = link.status.bus_tx;
    s["busErrors"] = link.status.bus_err;
    s["busDead"] = link.status.bus_dead;
    s["busEpisodes"] = link.status.bus_episodes;
    s["escalations"] = link.status.escalations;
    s["busy"] = link.status.busy;
    s["imageSize"] = link.status.image_size;
    s["timeSynced"] = link.status.time_synced;
  }
}
