#pragma once
// WallUpdatePolicy.h — when the master offers its stored row image to a row
// board, and what it makes of the answers (#559/#566). Pure, natively tested
// by test_wall_update_policy; the link task (WallLink.cpp) feeds it and
// writes the Update message. The row's half is FollowerUpdatePolicy.h.
//
// A row whose rev differs from the stored image's is offered it, in either
// direction: the stored image is what the wall runs. One row at a time, and
// the next only after the last one is back, so a bad image strands one board
// at most. A row in rescue mode is offered the image even at the rev it
// reports: its flash is what is in doubt, not its version.
//
// Every offer that fails costs the row an attempt; at the cap the row is
// given up on (blocked) until something speaks for a fresh look: a new stored
// image, the row reporting another rev than before, ten minutes of unbroken
// health, a changed rows table, or this master restarting. An offer to a row
// in rescue mode costs its attempt when it is made, and coming back healthy
// does not hand it back: an image that crashes after its Hello would
// otherwise be offered for ever.

#include <stdint.h>
#include <string.h>

#include "WallLinkCore.h"

// Failed offers per row before the master gives up on it.
#define WALL_UPDATE_ATTEMPT_CAP 3
// After a failed or refused offer nothing is offered for this long.
#define WALL_UPDATE_HOLDOFF_MS 30000UL
// A row answers an offer when it next reads its connection.
#define WALL_UPDATE_ANSWER_MS 20000UL
// The row gives a download up after 180 s by itself (FOLLOWER_UPDATE_TOTAL_MS,
// gated by tests/test_wall_update_glue.py); this only catches a row that
// never said how it ended.
#define WALL_UPDATE_DOWNLOAD_MS 200000UL
// From "installed" to the row's Hello on the new image.
#define WALL_UPDATE_RETURN_MS 120000UL
// Unbroken health that forgives a row its failed offers.
#define WALL_UPDATE_HEALTHY_FORGIVE_MS 600000UL

enum class WallUpdatePhase : uint8_t { Idle, Offered, Downloading, Returning };

// How an offer ended.
enum class WallUpdateEnd : uint8_t {
  None,         // nothing ended
  Converged,    // the row is back on the stored rev
  RolledBack,   // back on another rev
  StillRescue,  // back, still in rescue mode
  Failed,       // the row said the download or the install failed
  Refused,      // the row would not start, for a reason that will not pass
  HeldOff,      // a unit job holds the row: later, at no cost
  NoAnswer,     // the offer was never answered: later, at no cost
  TimedOut,     // the row never said how the download ended, or never came back
};

// What the offer rule needs to know about one row.
struct WallUpdateRow {
  bool reachable = false;  // welcomed, not busy, no job open
  bool rescue = false;
  const char* rev = "";
};

struct WallUpdater {
  WallUpdatePhase phase = WallUpdatePhase::Idle;
  int8_t row = -1;  // the row an offer is out to; -1 while Idle
  uint8_t attempts[WALL_LINK_MAX_ROWS] = {0};
  bool blocked[WALL_LINK_MAX_ROWS] = {false};

  // The row to offer the stored image to now, -1 for none.
  int nextCandidate(const WallUpdateRow* rows, int count, const char* storedRev,
                    uint32_t nowMs) const {
    if (phase != WallUpdatePhase::Idle || storedRev == nullptr || storedRev[0] == 0) return -1;
    if (heldOff && !wallLinkElapsed(nowMs, heldOffAtMs, WALL_UPDATE_HOLDOFF_MS)) return -1;
    if (count > WALL_LINK_MAX_ROWS) count = WALL_LINK_MAX_ROWS;
    // The rows take turns, from the one after the last offer: a row that
    // never answers, or is always busy, costs nothing and so is never
    // blocked, and must not keep the others waiting behind it.
    for (int n = 1; n <= count; n++) {
      const int i = (lastOffered + n) % count;
      const WallUpdateRow& r = rows[i];
      if (!r.reachable || blocked[i] || r.rev[0] == 0) continue;
      if (strcmp(r.rev, storedRev) == 0 && !r.rescue) continue;
      return i;
    }
    return -1;
  }

  // The Update message was written to this row.
  void offered(int r, bool rescue, uint32_t nowMs) {
    phase = WallUpdatePhase::Offered;
    row = (int8_t)r;
    lastOffered = (int8_t)r;
    sinceMs = nowMs;
    rescueOffer = rescue;
    charged = false;
    if (rescue) charge();
  }

  // An UpdateState from a row.
  WallUpdateEnd answer(int r, const wl_UpdateState& state, const char* storedRev,
                       uint32_t nowMs) {
    // A row repeats a result it could not deliver on its next connection:
    // one about another row, another image or no open offer is history.
    if (r != row || strcmp(state.rev, storedRev) != 0) return WallUpdateEnd::None;
    if (phase != WallUpdatePhase::Offered && phase != WallUpdatePhase::Downloading) {
      return WallUpdateEnd::None;
    }
    switch (state.phase) {
      case wl_UpdatePhase_UPDATE_DOWNLOADING:
        phase = WallUpdatePhase::Downloading;
        sinceMs = nowMs;
        return WallUpdateEnd::None;
      case wl_UpdatePhase_UPDATE_INSTALLED:
        phase = WallUpdatePhase::Returning;
        sinceMs = nowMs;
        return WallUpdateEnd::None;
      case wl_UpdatePhase_UPDATE_REFUSED:
        if (state.reason == wl_UpdateReason_UPDATE_UNITS_BUSY) {
          return end(WallUpdateEnd::HeldOff, false, nowMs);
        }
        return end(WallUpdateEnd::Refused, true, nowMs);
      default:
        return end(WallUpdateEnd::Failed, true, nowMs);
    }
  }

  // A row said Hello. `restarted`: with another boot id than before. Only
  // that is the row coming back: a redial on the old boot id is the same
  // program, still on its way.
  WallUpdateEnd hello(int r, const char* rev, bool rescue, bool restarted,
                      const char* storedRev, uint32_t nowMs) {
    if (r >= 0 && r < WALL_LINK_MAX_ROWS) healthy[r] = false;
    if (phase == WallUpdatePhase::Idle || r != row || !restarted) return WallUpdateEnd::None;
    // Still in rescue mode outranks a matching rev: the image did not cure it.
    if (rescue) return end(WallUpdateEnd::StillRescue, true, nowMs);
    if (strcmp(rev, storedRev) != 0) return end(WallUpdateEnd::RolledBack, true, nowMs);
    if (!rescueOffer) forgive(r);
    return end(WallUpdateEnd::Converged, false, nowMs, false);
  }

  // Once per pass of the link task.
  WallUpdateEnd tick(uint32_t nowMs) {
    switch (phase) {
      case WallUpdatePhase::Idle:
        return WallUpdateEnd::None;
      case WallUpdatePhase::Offered:
        if (!wallLinkElapsed(nowMs, sinceMs, WALL_UPDATE_ANSWER_MS)) return WallUpdateEnd::None;
        return end(WallUpdateEnd::NoAnswer, false, nowMs);
      case WallUpdatePhase::Downloading:
        if (!wallLinkElapsed(nowMs, sinceMs, WALL_UPDATE_DOWNLOAD_MS)) return WallUpdateEnd::None;
        return end(WallUpdateEnd::TimedOut, true, nowMs);
      case WallUpdatePhase::Returning:
        if (!wallLinkElapsed(nowMs, sinceMs, WALL_UPDATE_RETURN_MS)) return WallUpdateEnd::None;
        return end(WallUpdateEnd::TimedOut, true, nowMs);
    }
    return WallUpdateEnd::None;
  }

  // A row reported `newRev` where it reported `oldRev` before. A rev that
  // changed other than by this master's own offer (a direct upload) makes
  // what was held against the row history. Nothing known before is no change.
  void noteRev(int r, const char* oldRev, const char* newRev) {
    if (r < 0 || r >= WALL_LINK_MAX_ROWS || r == row) return;
    if (oldRev[0] == 0 || newRev[0] == 0 || strcmp(oldRev, newRev) == 0) return;
    forgive(r);
  }

  // Once per pass and row. `well`: welcomed and not in rescue mode. The
  // window starts afresh at every Hello, so a row that keeps restarting
  // never fills it.
  void health(int r, bool well, uint32_t nowMs) {
    if (r < 0 || r >= WALL_LINK_MAX_ROWS) return;
    if (!well) {
      healthy[r] = false;
    } else if (!healthy[r]) {
      healthy[r] = true;
      healthySinceMs[r] = nowMs;
    } else if (wallLinkElapsed(nowMs, healthySinceMs[r], WALL_UPDATE_HEALTHY_FORGIVE_MS)) {
      forgive(r);
      healthySinceMs[r] = nowMs;
    }
  }

  // Another image was stored: what was held against the rows was held
  // against the old one. Nothing is offered in the hold-off after it: an
  // offer made in the second the image was stored found the master not
  // answering the download, twice, and the one a hold-off later went through
  // (#568; why it does not answer then is not established).
  void newImage(uint32_t nowMs) {
    for (int i = 0; i < WALL_LINK_MAX_ROWS; i++) forgive(i);
    heldOff = true;
    heldOffAtMs = nowMs;
  }

  // An operator asked for this row to be offered the image again.
  void retry(int r) {
    forgive(r);
    heldOff = false;
  }

  // The rows table changed.
  void reset() { *this = WallUpdater{}; }

 private:
  bool rescueOffer = false;
  bool charged = false;  // this offer has cost its attempt
  bool heldOff = false;
  int8_t lastOffered = -1;
  uint32_t sinceMs = 0;  // when the current phase began
  uint32_t heldOffAtMs = 0;
  bool healthy[WALL_LINK_MAX_ROWS] = {false};
  uint32_t healthySinceMs[WALL_LINK_MAX_ROWS] = {0};

  void forgive(int r) {
    attempts[r] = 0;
    blocked[r] = false;
  }

  // An offer costs one attempt at most, however it goes wrong.
  void charge() {
    if (charged || row < 0) return;
    charged = true;
    if (attempts[row] < 255) attempts[row]++;
    if (attempts[row] >= WALL_UPDATE_ATTEMPT_CAP) blocked[row] = true;
  }

  WallUpdateEnd end(WallUpdateEnd how, bool costs, uint32_t nowMs, bool holdOff = true) {
    if (costs) charge();
    phase = WallUpdatePhase::Idle;
    row = -1;
    heldOff = holdOff;
    heldOffAtMs = nowMs;
    return how;
  }
};

inline const char* wallUpdatePhaseName(WallUpdatePhase phase) {
  switch (phase) {
    case WallUpdatePhase::Offered: return "offered";
    case WallUpdatePhase::Downloading: return "downloading";
    case WallUpdatePhase::Returning: return "returning";
    default: return "idle";
  }
}

inline const char* wallUpdateEndText(WallUpdateEnd how) {
  switch (how) {
    case WallUpdateEnd::Converged: return "is on the stored image";
    case WallUpdateEnd::RolledBack: return "came back on another rev";
    case WallUpdateEnd::StillRescue: return "came back still in rescue mode";
    case WallUpdateEnd::Failed: return "could not download or install it";
    case WallUpdateEnd::Refused: return "refused the offer";
    case WallUpdateEnd::HeldOff: return "is busy with its units, later";
    case WallUpdateEnd::NoAnswer: return "did not answer the offer, later";
    case WallUpdateEnd::TimedOut: return "was not heard from in time";
    default: return "";
  }
}
