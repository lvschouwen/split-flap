#pragma once
// BoardVerdict.h — what the master makes of one board of the Split-Flap
// (#559/#570): its own row of units, or a row board on the wall link. Same
// shape as a unit's verdict (UnitVerdict.h): a level, the reason that leads
// with two numbers, and every reason that applies. Pure, natively tested by
// test_board_verdict; the caller fills BoardFacts from the wall snapshot.
//
// A board the master cannot reach is judged on that alone: what it last said
// about its units is not evidence of anything now.
//
// Numbers and names are fixed as in UnitVerdict.h; a new reason goes at the
// end, and BOARD_REASON_ORDER says which leads.

#include <stdint.h>

#include "UnitVerdict.h"

enum class BoardReach : uint8_t {
  Up = 0,
  Busy,   // connected, in a unit job: silence is expected
  Away,   // no connection, heard a moment ago
  Lost,
  Never,  // not heard since this master started
};

//                                   a                        b
enum class BoardReason : uint8_t {
  Working = 0,          // units found                 seconds it has run
  Lost = 1,             // seconds since it was heard
  NeverSeen = 2,
  Rescue = 3,
  BusDead = 4,          // times its bus has gone dead
  UpdateBlocked = 5,    // offers of the image that failed
  UnitsMissing = 6,     // units found                 units it should have
  UnitsFault = 7,       // units with a fault          units found
  Updating = 8,
  UpdatingUnits = 9,
  Away = 10,            // seconds since it was heard
  UnitsUnknown = 11,
  ClockNotSet = 12,
  FirmwareDiffers = 13,
  UnitsNote = 14,       // units with a note           units found
};
#define BOARD_REASON_COUNT 15

inline uint32_t boardReasonBit(BoardReason r) { return 1UL << (uint8_t)r; }

inline const char* boardReasonName(BoardReason r) {
  switch (r) {
    case BoardReason::Working: return "working";
    case BoardReason::Lost: return "lost";
    case BoardReason::NeverSeen: return "never-seen";
    case BoardReason::Rescue: return "rescue";
    case BoardReason::BusDead: return "bus-dead";
    case BoardReason::UpdateBlocked: return "update-blocked";
    case BoardReason::UnitsMissing: return "units-missing";
    case BoardReason::UnitsFault: return "units-fault";
    case BoardReason::Updating: return "updating";
    case BoardReason::UpdatingUnits: return "updating-units";
    case BoardReason::Away: return "away";
    case BoardReason::UnitsUnknown: return "units-unknown";
    case BoardReason::ClockNotSet: return "clock-not-set";
    case BoardReason::FirmwareDiffers: return "firmware-differs";
    case BoardReason::UnitsNote: return "units-note";
  }
  return "?";
}

inline VerdictLevel boardReasonLevel(BoardReason r) {
  switch (r) {
    case BoardReason::Working:
      return VerdictLevel::Working;
    case BoardReason::Lost:
    case BoardReason::NeverSeen:
    case BoardReason::Rescue:
    case BoardReason::BusDead:
    case BoardReason::UpdateBlocked:
    case BoardReason::UnitsMissing:
    case BoardReason::UnitsFault:
      return VerdictLevel::Fault;
    default:
      return VerdictLevel::Note;
  }
}

static const BoardReason BOARD_REASON_ORDER[] = {
    BoardReason::Lost,
    BoardReason::NeverSeen,
    BoardReason::Rescue,
    BoardReason::BusDead,
    BoardReason::UpdateBlocked,
    BoardReason::UnitsMissing,
    BoardReason::UnitsFault,
    BoardReason::Updating,
    BoardReason::UpdatingUnits,
    BoardReason::Away,
    BoardReason::UnitsUnknown,
    BoardReason::ClockNotSet,
    BoardReason::FirmwareDiffers,
    BoardReason::UnitsNote,
};

struct BoardFacts {
  bool own = false;  // the master's own row: always reachable, never offered an image
  BoardReach reach = BoardReach::Up;
  uint32_t silentS = 0;        // since it was last heard
  bool startGraceOver = true;  // this master has run long enough for a row to have dialled
  bool rescue = false;
  bool busDead = false;
  uint32_t busEpisodes = 0;
  bool updateBlocked = false;
  uint8_t updateAttempts = 0;
  bool updating = false;       // it is taking the stored image now
  bool updatingUnits = false;
  bool clockSet = true;
  bool firmwareDiffers = false;  // runs another rev than the image stored for the rows
  uint32_t uptimeS = 0;
  uint8_t unitsPlaced = 0;  // how many the rows table says it has
  bool unitsKnown = false;  // the three counts below are its report
  uint8_t unitsFound = 0;
  uint8_t unitsFault = 0;
  uint8_t unitsNote = 0;
};

struct BoardVerdict {
  VerdictLevel level = VerdictLevel::Working;
  BoardReason reason = BoardReason::Working;
  uint32_t a = 0;
  uint32_t b = 0;
  uint32_t all = 0;  // boardReasonBit() of every reason that applies
};

inline uint32_t boardReasonsOf(const BoardFacts& f) {
  uint32_t all = 0;
  const auto add = [&all](BoardReason r) { all |= boardReasonBit(r); };
  if (!f.own) {
    // A row taking its image restarts into it: being away is the update.
    if (f.updating) return boardReasonBit(BoardReason::Updating);
    if (f.reach == BoardReach::Lost) return boardReasonBit(BoardReason::Lost);
    if (f.reach == BoardReach::Never) {
      return boardReasonBit(f.startGraceOver ? BoardReason::NeverSeen : BoardReason::Away);
    }
    if (f.reach == BoardReach::Away) return boardReasonBit(BoardReason::Away);
    if (f.rescue) return boardReasonBit(BoardReason::Rescue);  // it runs no units there
    if (f.busDead) add(BoardReason::BusDead);
    if (f.updateBlocked) add(BoardReason::UpdateBlocked);
    if (f.firmwareDiffers) add(BoardReason::FirmwareDiffers);
  }
  if (f.updatingUnits) add(BoardReason::UpdatingUnits);
  if (!f.clockSet) add(BoardReason::ClockNotSet);
  if (!f.unitsKnown) {
    add(BoardReason::UnitsUnknown);
    return all;
  }
  // A dead bus is why no unit answers: one reason, not one per unit. Units a
  // board is updating come and go by design.
  if (f.busDead || f.updatingUnits) return all;
  if (f.unitsFound < f.unitsPlaced) add(BoardReason::UnitsMissing);
  if (f.unitsFault > 0) add(BoardReason::UnitsFault);
  if (f.unitsNote > 0) add(BoardReason::UnitsNote);
  return all;
}

inline void boardReasonNumbers(BoardReason r, const BoardFacts& f, uint32_t& a, uint32_t& b) {
  a = 0;
  b = 0;
  switch (r) {
    case BoardReason::Working:
      a = f.unitsFound;
      b = f.uptimeS;
      break;
    case BoardReason::Lost:
    case BoardReason::Away:
      a = f.silentS;
      break;
    case BoardReason::BusDead:
      a = f.busEpisodes;
      break;
    case BoardReason::UpdateBlocked:
      a = f.updateAttempts;
      break;
    case BoardReason::UnitsMissing:
      a = f.unitsFound;
      b = f.unitsPlaced;
      break;
    case BoardReason::UnitsFault:
      a = f.unitsFault;
      b = f.unitsFound;
      break;
    case BoardReason::UnitsNote:
      a = f.unitsNote;
      b = f.unitsFound;
      break;
    default:
      break;
  }
}

inline BoardReason boardReasonLeading(uint32_t all) {
  for (BoardReason r : BOARD_REASON_ORDER) {
    if (all & boardReasonBit(r)) return r;
  }
  return BoardReason::Working;
}

inline BoardVerdict boardVerdict(const BoardFacts& f) {
  BoardVerdict v;
  v.all = boardReasonsOf(f);
  v.reason = boardReasonLeading(v.all);
  v.level = boardReasonLevel(v.reason);
  boardReasonNumbers(v.reason, f, v.a, v.b);
  return v;
}

// A row's units counted by their verdicts, for BoardFacts.
struct UnitLevelCounts {
  uint8_t fault = 0;
  uint8_t note = 0;
};

inline void unitLevelCount(UnitLevelCounts& counts, VerdictLevel level) {
  if (level == VerdictLevel::Fault) counts.fault++;
  if (level == VerdictLevel::Note) counts.note++;
}

// The wall is as well as its worst board.
inline VerdictLevel verdictWorse(VerdictLevel a, VerdictLevel b) {
  return (uint8_t)a >= (uint8_t)b ? a : b;
}
