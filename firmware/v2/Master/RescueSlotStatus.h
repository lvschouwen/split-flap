#pragma once
// RescueSlotStatus.h — pure verdict for the factory rescue slot (#391, #613).
//
// The rescue image is the last line of defence, and two things about it are
// worth knowing from the running app: whether the board can be recovered by
// it at all, and whether it is the rescue image this build knows.
//
// The second is answered by content (#440): every build carries the rescue
// source head (RESCUE_SOURCE_HEAD, the rev of the last commit touching the
// rescue image's inputs) and the rescue image embeds the same value as a
// marker in its binary (../release/RescueSourceHead.h). The master reads
// the marker off the partition. Equal = OK, whatever the repo rev of either
// image; different = OUTDATED (install the current rescue); no marker =
// vintage unknown (install the current rescue to be sure). Comparing the
// slot's repo rev with the running one, as before #613, made every master
// commit call a good rescue image old.
//
// The rev still comes from an NVS record written at INSTALL time (POST
// /firmware/rescue, key slotRecF, SlotRecord.h format): the slot's own app
// descriptor cannot identify it (pioarduino's hybrid compile freezes the
// build stamp at framework-assembly time, #200). It is trusted only while
// its sha256 still matches the partition's actual content — the same guard
// ensureSlotRecord() applies to app0/app1.
//
// Reporting the partition (not a LittleFS sidecar) is deliberate: a file
// records what was uploaded, the partition records what is actually
// installed, and only the latter answers "what will I boot in a crisis".
//
// Natively tested (test/test_rescue_slot).

#include <stdio.h>
#include <string.h>

#include "RescueSourceHead.h"
#include "SlotRecord.h"

enum RescueSlotState {
  RESCUE_SLOT_OK = 0,        // bootable, built from the rescue sources this build knows
  RESCUE_SLOT_ABSENT,        // no factory partition (mismatched table)
  RESCUE_SLOT_EMPTY,         // partition present, no bootable image
  RESCUE_SLOT_UNIDENTIFIED,  // bootable, but nothing says what it is: no marker, no record
  RESCUE_SLOT_OUTDATED,      // bootable, built from other rescue sources than this build knows
  RESCUE_SLOT_UNKNOWN_VINTAGE,  // bootable and recorded, but from before the marker
};

struct RescueSlotFacts {
  bool present = false;
  bool valid = false;
  bool identified = false;  // the record names its rev
  bool outdated = false;
  // Whether this board can still be recovered by its break-glass image —
  // the only question the flag answers, and the only one an operator can
  // act on. ABSENT, EMPTY and UNIDENTIFIED say no. OUTDATED and UNKNOWN
  // VINTAGE do not: they report vintage, not capability (#438).
  bool warn = true;
  RescueSlotState state = RESCUE_SLOT_ABSENT;
  char rev[SLOT_RECORD_REV_MAX_LEN + 1] = {0};
  char sourceHead[RESCUE_SOURCE_HEAD_MAX + 1] = {0};  // the slot's marker, "" = none
  const char* todo = "";  // what an operator does about it, "" = nothing
};

// `slotSourceHead` is the marker read off the partition ("" or null = none),
// `ownSourceHead` this build's RESCUE_SOURCE_HEAD.
inline RescueSlotFacts rescueSlotFacts(bool present, bool valid, const SlotRecord& rec,
                                       bool shaMatches, const char* slotSourceHead,
                                       const char* ownSourceHead) {
  RescueSlotFacts f;

  f.present = present;
  if (!present) {
    f.state = RESCUE_SLOT_ABSENT;
    f.todo = "the partition table has no factory slot";
    return f;
  }

  // The image outranks the bookkeeping: a record left behind by a previous
  // install must never make an empty slot look identified.
  f.valid = valid;
  if (!valid) {
    f.state = RESCUE_SLOT_EMPTY;
    f.todo = "install the current rescue";
    return f;
  }

  // A record carrying no identity is no rev. Two shapes mean that: an empty
  // rev, and the literal "?" that formatSlotRecord substitutes when a rev
  // filters down to nothing (SlotRecord.h).
  if (rec.ok && shaMatches && rec.rev[0] != '\0' &&
      !(rec.rev[0] == '?' && rec.rev[1] == '\0')) {
    f.identified = true;
    snprintf(f.rev, sizeof(f.rev), "%s", rec.rev);
  }

  const bool headKnown = slotSourceHead != nullptr && slotSourceHead[0] != '\0';
  if (headKnown) snprintf(f.sourceHead, sizeof(f.sourceHead), "%s", slotSourceHead);

  if (!headKnown && !f.identified) {
    f.state = RESCUE_SLOT_UNIDENTIFIED;
    f.todo = "install the current rescue to be sure";
    return f;
  }
  // Something says what it is: recoverable, so the warn is answered.
  f.warn = false;
  if (!headKnown) {
    f.state = RESCUE_SLOT_UNKNOWN_VINTAGE;
    f.todo = "install the current rescue to be sure";
    return f;
  }
  if (ownSourceHead == nullptr || ownSourceHead[0] == '\0' ||
      strcmp(f.sourceHead, ownSourceHead) != 0) {
    f.outdated = true;
    f.state = RESCUE_SLOT_OUTDATED;
    f.todo = "install the current rescue";
    return f;
  }
  f.state = RESCUE_SLOT_OK;
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
    case RESCUE_SLOT_OUTDATED: return "outdated";
    case RESCUE_SLOT_UNKNOWN_VINTAGE: return "unknown-vintage";
  }
  return "unknown";
}
