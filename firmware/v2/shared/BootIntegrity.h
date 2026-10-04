#pragma once

#include <stdint.h>

#include "BootUpdateReport.h"  // BootUpdateReport + BootSectionState

// Boot-section integrity watch (#520). A unit's bootloader is the only way to
// reflash it without ICSP, and flash that rots there goes unnoticed until the
// day a reflash is needed. The unit re-reads and re-classifies its own boot
// section on a timer and publishes it in the GET_BOOT_INFO reply; each row
// master reads that reply on its health poll and judges it here. Nothing is
// repaired from this verdict — it only makes the damage visible while the
// application can still rewrite the section.
//
// Pure logic, natively tested (Unit test_boot_integrity).

// How often an idle unit recomputes its boot report. The CRC pass holds loop()
// for some tens of milliseconds, so it only runs while the drum is parked.
#define BOOT_INFO_REFRESH_INTERVAL_MS (10UL * 60UL * 1000UL)

// millis()-wrap safe. The boot-time report counts as the first refresh.
inline bool bootInfoRefreshDue(uint32_t nowMs, uint32_t lastRefreshMs,
                               bool drumIdle) {
  return drumIdle && (nowMs - lastRefreshMs) >= BOOT_INFO_REFRESH_INTERVAL_MS;
}

enum BootIntegrity : uint8_t {
  BOOT_INTEGRITY_UNREAD = 0,  // no valid report this poll
  BOOT_INTEGRITY_OK,          // the image this build expects
  BOOT_INTEGRITY_OUTDATED,    // a known image or update step, not the current one
  BOOT_INTEGRITY_CORRUPT,     // matches nothing known
};

// The CRC is the identity; the unit's own classification only speaks for the
// states a CRC cannot name (mid-update, or a target image newer than the one
// this master was built against). Unknown is the one state no intact image
// produces.
inline BootIntegrity bootIntegrityJudge(const BootUpdateReport& r,
                                        uint32_t expectedCrc32) {
  if (r.bootCrc32 == expectedCrc32) return BOOT_INTEGRITY_OK;
  if (r.state == BOOT_STATE_UNKNOWN) return BOOT_INTEGRITY_CORRUPT;
  return BOOT_INTEGRITY_OUTDATED;
}

inline const char* bootIntegrityName(uint8_t v) {
  switch (v) {
    case BOOT_INTEGRITY_OK:       return "ok";
    case BOOT_INTEGRITY_OUTDATED: return "outdated";
    case BOOT_INTEGRITY_CORRUPT:  return "corrupt";
    default:                      return "unread";
  }
}

// One log line per change of verdict. `logged` is the verdict the operator log
// last reflected (UNREAD until the first one); a poll that read nothing keeps
// it, so a lossy bus neither clears nor repeats a finding. A first reading of
// OK is the normal case and says nothing.
struct BootIntegrityEdge {
  bool log;         // emit a line for `cur`
  uint8_t logged;   // value to store back
};

inline BootIntegrityEdge bootIntegrityEdge(uint8_t logged, uint8_t cur) {
  if (cur == BOOT_INTEGRITY_UNREAD || cur == logged) return {false, logged};
  bool quietFirstOk = logged == BOOT_INTEGRITY_UNREAD && cur == BOOT_INTEGRITY_OK;
  return {!quietFirstOk, cur};
}
