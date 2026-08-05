#pragma once
// Pure edge-detection for the master-side unit-health event log (#322). The
// master surfaces per-unit health only as passive /units/health JSON counters;
// a unit going faulty (last home failed / hall never fired), falling off the
// bus (#310 heartbeat), or disagreeing with its intended letter (#264 mm) never
// reaches the flash/web log an operator actually watches — the same silent gap
// as the drift auto re-home (DriftLogPolicy.h, the counter half of #322).
//
// This folds the current per-unit condition mask into the TRANSITIONS worth one
// log line since we last looked, so each onset (and each recovery, for the
// recoverable conditions) is logged exactly once. The last-logged mask lives in
// UnitFacts (per probe epoch; a probe rescan re-zeroes it, and a transient
// unreadable poll carries the prior via validMask — a unit going silent must
// never read as "recovered"). Natively tested by test_unit_event_log; the
// SerialPrintf glue + label strings stay in DisplayTask.cpp.

#include <stdint.h>
#include "UnitHealth.h"  // UnitRebootWatch reboot edge-detect state (#368)
#include "UnitExtDiag.h"  // UnitExtDiag + EXT_DIAG_STATUS_STALL (#365, also
                          // reached transitively via UnitHealth.h; included
                          // directly since this header now names both)

// Loggable per-unit conditions, one bit each (master-side derived).
#define UNIT_EVT_HOME_FAILED  (1 << 0)  // UNIT_FLAG_LAST_HOME_FAILED (status)
#define UNIT_EVT_HALL_NEVER   (1 << 1)  // UNIT_FLAG_HALL_NEVER (status)
#define UNIT_EVT_STALE        (1 << 2)  // heartbeat lost — off the bus (#310)
#define UNIT_EVT_MISMATCH     (1 << 3)  // displayed letter != intended (#264 mm)
#define UNIT_EVT_LOW_VCC      (1 << 4)  // since-boot vccMin below the floor (#366)
#define UNIT_EVT_JAM          (1 << 5)  // last move stalled (#365 ext-diag statusBits)
#define UNIT_EVT_DRAG         (1 << 6)  // steps-to-home excess over threshold (#365)
#define UNIT_EVT_HALL_ANOMALY (1 << 7)  // hall edges/rev != 1, degrading sensor (#365)

// Excess steps over the geometry-expected home distance that flags mechanical
// drag. Well above normal slop, below a jam. TODO(#370): tune on the bench.
#define EXT_DIAG_DRAG_EXCESS_STEPS 200

// Conditions whose RECOVERY (true->false) is also worth a log line. Onset is
// always logged; recovery only here — a unit rejoining the bus or homing OK
// again is real good-news, whereas hall-never is effectively permanent, a
// mismatch clearing is usually just the next frame catching up (noise), and
// low-Vcc rides a since-boot MINIMUM that only ever falls (it can't "recover"
// without a unit reboot, which re-baselines the whole mask at the next probe).
// The three #365 ext-diag bits are onset-only too: a jam clearing on the next
// move is the same "next sample catches up" noise as mismatch, stepExcessMax
// is itself a since-boot worst-case like vccMin (only a reboot re-baselines
// it), and hallEdgesLastRev flipping back to 1 next revolution is ordinary
// odometer/marker phase noise (see the bench caveat on unitEventEvaluate's
// ext-diag args below), not a sensor that "recovered".
#define UNIT_EVT_RECOVERABLE (UNIT_EVT_HOME_FAILED | UNIT_EVT_STALE)

// Fixed low-supply floor (#366): a unit whose since-boot vccMin (mid-move
// sampled, #306) has dipped below this is logged once as a brownout precursor
// where an operator actually looks — the vccMin JSON is otherwise a passive
// number. AVR units run 5V nominal with BOD typically ~2.7V, so 4000 mV is a
// clear ~1V sag warning: well above a hard reset yet below normal mid-move
// droop, so it flags real trouble without flooding on healthy rails.
// TODO(#366): make NVS-configurable per-device (fixed compile-time for now).
#define UNIT_VCC_MIN_FLOOR_MV 4000

// True when a unit's supply diagnostics are valid AND its since-boot minimum
// Vcc has dipped below the floor. vccMin==0 is the "no reading" sentinel (a
// pre-vitals unit or a glitched ADC), never a low-Vcc alert. Pure so the
// threshold decision is natively tested; the SerialPrintf glue is bench tier.
inline bool unitVccIsLow(bool vitalsValid, uint16_t vccMin_mV,
                         uint16_t floor_mV) {
  return vitalsValid && vccMin_mV != 0 && vccMin_mV < floor_mV;
}

struct UnitEventTransitions {
  uint8_t onset;     // conditions that became true since the baseline
  uint8_t recovery;  // recoverable conditions that became false
  uint8_t newState;  // condition mask to store back into the unit's facts
};

// prior == the last-logged condition mask for this unit (0 at a fresh probe
// epoch). cur == the conditions true right now. validMask == the conditions
// actually observable this poll: bits outside it keep their prior value, so a
// unit that went unreadable neither onsets nor "recovers" its status-derived
// conditions.
//
// extDiagValid + extDiag (#365): the same "unreadable never fakes a verdict"
// rule folded in for the three ext-diag-derived bits (JAM/DRAG/HALL_ANOMALY)
// — !extDiagValid contributes NONE of them (neither into cur nor validMask),
// so a silent/pre-ext-diag-firmware unit carries its prior state instead of
// fabricating (or clearing) a jam/drag/anomaly. Defaulted so every pre-#365
// call site (this header's other tests, and any future non-ext-diag caller)
// keeps compiling and behaving identically.
//
// hallEdgesLastRev semantics (#418): 0 is the "no completed revolution
// measured" sentinel, NOT an anomaly — a freshly flashed/rebooted unit in
// clock mode may not complete a rev for days (the wall logged four false
// anomalies minutes after the #407 campaign), and the odometer-vs-marker
// phase coincidence reads 0 on a healthy unit too. Only >1 flags. A hall
// that truly misses edges surfaces through HOME_FAILED / HALL_NEVER, which
// homing depends on. BENCH CAVEAT (still open for the >1 side): phase
// coincidence can also read 2 on a healthy unit; a consecutive-count
// debounce is the follow-up if that noise shows up — it needs new persisted
// UnitFacts state (a shared-header change) so it is deliberately NOT done
// in this slice.
inline UnitEventTransitions unitEventEvaluate(
    uint8_t prior, uint8_t cur, uint8_t validMask, bool extDiagValid = false,
    const UnitExtDiag& extDiag = UnitExtDiag()) {
  if (extDiagValid) {
    validMask = (uint8_t)(validMask |
                          (UNIT_EVT_JAM | UNIT_EVT_DRAG | UNIT_EVT_HALL_ANOMALY));
    if (extDiag.statusBits & EXT_DIAG_STATUS_STALL) cur = (uint8_t)(cur | UNIT_EVT_JAM);
    if (extDiag.stepExcessMax > EXT_DIAG_DRAG_EXCESS_STEPS)
      cur = (uint8_t)(cur | UNIT_EVT_DRAG);
    if (extDiag.hallEdgesLastRev > 1) cur = (uint8_t)(cur | UNIT_EVT_HALL_ANOMALY);
  }
  uint8_t effective = (uint8_t)((cur & validMask) | (prior & ~validMask));
  UnitEventTransitions t;
  t.onset = (uint8_t)(effective & ~prior);
  t.recovery = (uint8_t)((prior & ~effective) & UNIT_EVT_RECOVERABLE);
  t.newState = effective;
  return t;
}

// Unit reset-cause decoded from the MCUSR snapshot GET_STATUS byte 1 carries
// (#368). Priority is most-actionable-first: a brownout (BORF) is the operator
// signal that matters, so it wins over a co-set watchdog/power-on flag.
enum UnitResetCause {
  RESET_UNKNOWN = 0, RESET_POWER_ON, RESET_EXTERNAL, RESET_BROWNOUT, RESET_WATCHDOG
};

inline UnitResetCause unitResetCauseDecode(uint8_t mcusr) {
  if (mcusr & (1 << 2)) return RESET_BROWNOUT;  // BORF
  if (mcusr & (1 << 3)) return RESET_WATCHDOG;  // WDRF
  if (mcusr & (1 << 1)) return RESET_EXTERNAL;  // EXTRF
  if (mcusr & (1 << 0)) return RESET_POWER_ON;  // PORF
  return RESET_UNKNOWN;
}

inline const char* unitResetCauseName(UnitResetCause c) {
  switch (c) {
    case RESET_BROWNOUT: return "brownout";
    case RESET_WATCHDOG: return "watchdog";
    case RESET_EXTERNAL: return "external";
    case RESET_POWER_ON: return "power-on";
    default:             return "unknown";
  }
}

// Reboot edge (#368): a unit that browns out / watchdog-resets just re-homes
// and looks healthy. GET_STATUS carries uptime + lifetime brownout/watchdog
// counts; a reboot shows as uptime falling OR either count climbing. State
// (UnitRebootWatch) lives per-unit in UnitFacts, defined in UnitHealth.h.
inline bool unitRebootDetect(UnitRebootWatch& w, uint16_t uptime,
                             uint8_t brownout, uint8_t watchdog) {
  if (!w.primed) {
    w.lastUptime = uptime; w.lastBrownout = brownout;
    w.lastWatchdog = watchdog; w.primed = true;
    return false;
  }
  bool rebooted = uptime < w.lastUptime ||
                  brownout != w.lastBrownout || watchdog != w.lastWatchdog;
  w.lastUptime = uptime; w.lastBrownout = brownout; w.lastWatchdog = watchdog;
  return rebooted;
}
