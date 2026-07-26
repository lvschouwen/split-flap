#pragma once
// RescueSlotStatus.h — pure verdict for the factory rescue slot (#391).
//
// The rescue image is the last line of defence, but nothing about it was
// observable from the running app: the boot banner said only "image
// present", and no API carried its identity. A board could hold a rescue
// build months behind the running firmware — missing later hardening — and
// nothing would say so until someone rebooted into it.
//
// The slot's own app descriptor cannot identify it: pioarduino's hybrid
// compile freezes the build stamp at framework-assembly time (#200), so
// every image from one framework cache carries the same version/date. The
// rev therefore comes from an NVS record written at INSTALL time
// (POST /firmware/rescue, key slotRecF, SlotRecord.h format) and is trusted
// only while its sha256 still matches the partition's actual content — the
// same guard ensureSlotRecord() applies to app0/app1. A slot rewritten
// behind the record's back demotes to UNIDENTIFIED rather than reporting a
// stale rev as truth.
//
// Reporting the partition (not a LittleFS sidecar) is deliberate: a file
// records what was uploaded, the partition records what is actually
// installed, and only the latter answers "what will I boot in a crisis".
//
// Natively tested (test/test_rescue_slot).

#include <stdio.h>
#include <string.h>

#include "SlotRecord.h"

enum RescueSlotState {
  RESCUE_SLOT_OK = 0,        // bootable and identified, at the running rev
  RESCUE_SLOT_ABSENT,        // no factory partition (mismatched table)
  RESCUE_SLOT_EMPTY,         // partition present, no bootable image
  RESCUE_SLOT_UNIDENTIFIED,  // bootable, but we cannot say what it is
  RESCUE_SLOT_STALE,         // bootable and identified, behind the app
};

struct RescueSlotFacts {
  bool present = false;
  bool valid = false;
  bool identified = false;
  bool stale = false;
  // Anything an operator should act on. Only OK clears it — an
  // unidentifiable rescue image is a finding, not a neutral state.
  bool warn = true;
  RescueSlotState state = RESCUE_SLOT_ABSENT;
  char rev[SLOT_RECORD_REV_MAX_LEN + 1] = {0};
};

// runningRev may be null/empty (no identity to compare against): the rev is
// still reported, but staleness is unknowable and must never be claimed.
inline RescueSlotFacts rescueSlotFacts(bool present, bool valid,
                                       const SlotRecord& rec, bool shaMatches,
                                       const char* runningRev) {
  RescueSlotFacts f;

  f.present = present;
  if (!present) {
    f.state = RESCUE_SLOT_ABSENT;
    return f;
  }

  // The image outranks the bookkeeping: a record left behind by a previous
  // install must never make an empty slot look identified.
  f.valid = valid;
  if (!valid) {
    f.state = RESCUE_SLOT_EMPTY;
    return f;
  }

  // A parsed record with an empty rev carries no identity — that is
  // unidentified, not a rev of "".
  if (!rec.ok || !shaMatches || rec.rev[0] == '\0') {
    f.state = RESCUE_SLOT_UNIDENTIFIED;
    return f;
  }

  f.identified = true;
  snprintf(f.rev, sizeof(f.rev), "%s", rec.rev);

  if (runningRev != nullptr && runningRev[0] != '\0' &&
      strcmp(f.rev, runningRev) != 0) {
    f.stale = true;
    f.state = RESCUE_SLOT_STALE;
    return f;
  }

  f.state = RESCUE_SLOT_OK;
  f.warn = false;
  return f;
}

// Short operator-facing label — one source of truth for the boot banner,
// the JSON and the web UI so they can never disagree.
inline const char* rescueSlotStateLabel(RescueSlotState s) {
  switch (s) {
    case RESCUE_SLOT_OK: return "ok";
    case RESCUE_SLOT_ABSENT: return "absent";
    case RESCUE_SLOT_EMPTY: return "empty";
    case RESCUE_SLOT_UNIDENTIFIED: return "unidentified";
    case RESCUE_SLOT_STALE: return "stale";
  }
  return "unknown";
}
