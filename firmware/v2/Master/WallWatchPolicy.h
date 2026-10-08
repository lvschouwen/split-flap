#pragma once
// WallWatchPolicy.h — following the verdicts over time (#559/#570): which
// changes are worth an entry in the event record. Pure, natively tested by
// test_wall_watch; WallWatch.cpp gathers the facts and owns the state.
//
// An entry is made when a reason starts, and when a fault ends. What a read
// could not speak for keeps what was known (unitReasonsObservable), so a
// unit that goes quiet does not "recover" from its failed home.
//
// The first look at a row's units after this master started records only what
// is wrong: their notes were there before and are not news. A board's
// recorded reasons are all news at any time: they are faults, or something
// it is doing now.
//
// Left out on purpose, as reasons that come and go by design or are another
// screen's subject: a unit finding home or being updated, a status that did
// not arrive, firmware and bootloader that are behind; a board briefly away,
// its units' own faults and notes (each unit has its entry), its clock, its
// rev.
//
// A failed home the unit got over between two looks leaves only its lifetime
// count one higher. Every rise of that count is recorded by itself, with how
// many this master has seen and the lifetime count, and notes the unit until
// this master starts again (#579).

#include <stdint.h>

#include "BoardVerdict.h"
#include "EventRecordPolicy.h"
#include "SplitFlapProtocol.h"  // SFP_I2C_ADDRESS_BASE
#include "UnitVerdict.h"
#include "WearPolicy.h"

inline uint32_t unitFaultReasons() {
  uint32_t mask = 0;
  for (UnitReason r : UNIT_REASON_ORDER) {
    if (unitReasonLevel(r) == VerdictLevel::Fault) mask |= unitReasonBit(r);
  }
  return mask;
}

inline uint32_t unitRecordedReasons() {
  const auto flag = unitReasonBit;
  const uint32_t never = flag(UnitReason::Working) | flag(UnitReason::BeingUpdated) |
                         flag(UnitReason::FindingHome) | flag(UnitReason::NotRead) |
                         flag(UnitReason::FirmwareOutdated) | flag(UnitReason::BootloaderOutdated) |
                         flag(UnitReason::HomeFailedBefore);
  return ((1UL << UNIT_REASON_COUNT) - 1) & ~never;
}

// Both a start and an end are recorded for these.
inline uint32_t boardRecordedReasons() {
  const auto flag = boardReasonBit;
  return flag(BoardReason::Lost) | flag(BoardReason::NeverSeen) | flag(BoardReason::Rescue) |
         flag(BoardReason::BusDead) | flag(BoardReason::UpdateBlocked) |
         flag(BoardReason::UnitsMissing) | flag(BoardReason::Updating) |
         flag(BoardReason::UpdatingUnits);
}

inline uint32_t boardFaultReasons() {
  uint32_t mask = 0;
  for (BoardReason r : BOARD_REASON_ORDER) {
    if (boardReasonLevel(r) == VerdictLevel::Fault) mask |= boardReasonBit(r);
  }
  return mask;
}

struct ReasonEdges {
  uint32_t on = 0;     // reasons to record as started
  uint32_t off = 0;    // reasons to record as ended
  uint32_t state = 0;  // what to remember
};

// `prior` = what was remembered, `now` = what applies, `observable` = what
// this look could speak for.
inline ReasonEdges watchReasonEdges(uint32_t prior, uint32_t now, uint32_t observable,
                                    uint32_t recordOn, uint32_t recordOff) {
  ReasonEdges e;
  e.state = (now & observable) | (prior & ~observable);
  e.on = e.state & ~prior & recordOn;
  e.off = prior & ~e.state & recordOff;
  return e;
}

// What is remembered about one board between looks.
struct WatchBoard {
  uint16_t key = 0;
  bool used = false;
  uint32_t boardReasons = 0;
  bool linkSeen = false;
  uint32_t restarts = 0;
  bool unitsSeen = false;
  uint32_t unitReasons[UNITS_AMOUNT] = {0};
  UnitRebootWatch reboot[UNITS_AMOUNT];
  HomeFailWatch homeFail[UNITS_AMOUNT];
};

// A sink is anything with
//   void event(EventKind kind, uint8_t detail, uint8_t unit, uint32_t a, uint32_t b);

// Judges a board's units into `verdicts` and records what changed. `hold`: the
// board is updating its units, which restart and sit in their bootloaders by
// design — nothing is recorded and nothing remembered is changed.
template <class Sink>
void watchUnits(WatchBoard& w, const UnitFacts* units, int width, uint32_t nowMs, bool hold,
                UnitVerdict* verdicts, Sink& sink) {
  if (width > UNITS_AMOUNT) width = UNITS_AMOUNT;
  WearAssessment wear;
  assessWear(units, width, wear);
  const uint32_t faults = unitFaultReasons();
  const uint32_t recorded = unitRecordedReasons();
  for (int i = 0; i < width; i++) {
    const UnitFacts& u = units[i];
    // Only a read of the unit's firmware carries the count.
    uint8_t homeFailRise = 0;
    if (u.state == 1 && u.lifetimeValid && !unitIsLost(u)) {
      homeFailRise = homeFailObserve(w.homeFail[i], u.lifetime.homeFailedCount);
    }
    UnitVerdictContext ctx;
    ctx.nowMs = nowMs;
    ctx.updating = hold;
    ctx.worn = wear.flagged[i];
    ctx.homeFailedSince = w.homeFail[i].since;
    verdicts[i] = unitVerdict(u, ctx);
    const uint8_t address = (uint8_t)(SFP_I2C_ADDRESS_BASE + i);

    // Its uptime falling, or a reset counter climbing, is a restart.
    bool restarted = false;
    if (u.state == 1 && u.statusValid && !unitIsLost(u)) {
      restarted = unitRebootDetect(w.reboot[i], u.status.uptimeSeconds,
                                   u.status.lifetimeBrownoutCount,
                                   u.status.lifetimeWatchdogCount);
    }
    if (hold) {
      // A unit the update has in its bootloader comes back with a fresh
      // uptime: that restart is the update's, so its count starts over.
      if (u.state != 1) w.reboot[i] = UnitRebootWatch();
      continue;
    }
    if (restarted && w.unitsSeen) {
      sink.event(EventKind::UnitRestarted, u.status.mcusrAtBoot, address,
                 u.status.lifetimeBrownoutCount, u.status.lifetimeWatchdogCount);
    }
    if (homeFailRise > 0) {
      sink.event(EventKind::UnitReasonOn, (uint8_t)UnitReason::HomeFailedBefore, address,
                 w.homeFail[i].since, u.lifetime.homeFailedCount);
    }
    const ReasonEdges e =
        watchReasonEdges(w.unitReasons[i], verdicts[i].all, unitReasonsObservable(u),
                         w.unitsSeen ? recorded : (recorded & faults), faults);
    w.unitReasons[i] = e.state;
    for (UnitReason r : UNIT_REASON_ORDER) {
      if (e.on & unitReasonBit(r)) {
        uint32_t a = 0, b = 0;
        unitReasonNumbers(r, u, ctx, a, b);
        sink.event(EventKind::UnitReasonOn, (uint8_t)r, address, a, b);
      }
      if (e.off & unitReasonBit(r)) sink.event(EventKind::UnitReasonOff, (uint8_t)r, address, 0, 0);
    }
  }
  if (!hold) w.unitsSeen = true;
}

template <class Sink>
void watchBoard(WatchBoard& w, const BoardFacts& facts, const BoardVerdict& verdict, Sink& sink) {
  const uint32_t recorded = boardRecordedReasons();
  const ReasonEdges e =
      watchReasonEdges(w.boardReasons, verdict.all, boardReasonsObservable(facts), recorded,
                       recorded);
  w.boardReasons = e.state;
  for (BoardReason r : BOARD_REASON_ORDER) {
    if (e.on & boardReasonBit(r)) {
      uint32_t a = 0, b = 0;
      boardReasonNumbers(r, facts, a, b);
      sink.event(EventKind::BoardReasonOn, (uint8_t)r, 0, a, b);
    }
    if (e.off & boardReasonBit(r)) sink.event(EventKind::BoardReasonOff, (uint8_t)r, 0, 0, 0);
  }
}

// A row board that came back with a new boot id has restarted. `restarts` is
// the link's count of those since this master started; the first look only
// takes note of it, and a count that starts over (the rows table changed)
// is no restart.
template <class Sink>
void watchRowRestarts(WatchBoard& w, uint32_t restarts, bool rescue, const char* rev, Sink& sink) {
  if (w.linkSeen && restarts > w.restarts) {
    sink.event(EventKind::RowStarted, rescue ? 1 : 0, 0, eventRevNumber(rev), 0);
  }
  w.linkSeen = true;
  w.restarts = restarts;
}

// The boards being followed, by key. A board that left the wall gives its
// place to the next new one.
template <int N>
struct WatchTable {
  WatchBoard boards[N];

  // Call with every key of the wall before find(): forgets the others.
  void keep(const uint16_t* keys, int count) {
    for (WatchBoard& b : boards) {
      if (!b.used) continue;
      bool present = false;
      for (int i = 0; i < count; i++) present = present || keys[i] == b.key;
      if (!present) b = WatchBoard();
    }
  }

  // nullptr when every place is taken (more boards than the wall can have).
  WatchBoard* find(uint16_t key) {
    for (WatchBoard& b : boards) {
      if (b.used && b.key == key) return &b;
    }
    for (WatchBoard& b : boards) {
      if (!b.used) {
        b = WatchBoard();
        b.used = true;
        b.key = key;
        return &b;
      }
    }
    return nullptr;
  }
};
