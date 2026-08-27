#pragma once
// OtaStatus.h — pure decision logic for the OTA slice (#190): md5 query-param
// validation for POST /firmware/master and the /settings verdict fields
// synthesized from esp_ota partition state. No esp_ota types in here —
// OtaService.cpp reads the hardware state and feeds these; everything
// decision-shaped is natively tested (test/test_ota_status).

#include <Arduino.h>

// Lowercases the digest in place, then demands exactly 32 hex chars — a
// digest that can't be a digest is a client bug, rejected with 400 before
// any flash work starts (v1 #144, tightened: v1 let Update discover
// non-hex at end()).
inline bool normalizeOtaMd5(String& md5) {
  md5.toLowerCase();
  if (md5.length() != 32) return false;
  for (size_t i = 0; i < 32; i++) {
    char c = md5[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

struct OtaVerdict {
  String lastFlashResult;  // "", "pending", "ok", "reverted"
  bool otaReverted = false;
};

// This boot's own image state outranks rollback history: while a fresh
// image is pending/confirmed, a lingering last-invalid mark is a previous
// attempt's corpse and must not read as a failure of THIS flash
// (ota-master.sh retry flow: attempt 1 reverted, attempt 2 running).
inline OtaVerdict synthesizeOtaVerdict(bool rolledBack, bool pendingVerify,
                                       bool confirmedThisBoot) {
  OtaVerdict v;
  if (confirmedThisBoot) {
    v.lastFlashResult = "ok";
  } else if (pendingVerify) {
    v.lastFlashResult = "pending";
  } else if (rolledBack) {
    v.lastFlashResult = "reverted";
    v.otaReverted = true;
  }
  return v;
}

// #390: an out-of-band recovery (rescue upload to app0, USB flash) leaves
// the last normal OTA's ?v= diagnostic pointing at an image this boot is
// not running — misleading to anyone reading intendedVersion as "what
// should be running". Blank it on boot when it mismatches the running rev,
// UNLESS the verdict is a genuine revert: there version != intendedVersion
// is exactly the signal the field exists to produce, and erasing it would
// destroy the revert diagnosis. Gates on the synthesized verdict, not the
// raw rolledBack flag — that one can be a previous attempt's corpse (see
// synthesizeOtaVerdict above).
inline bool otaShouldBlankIntendedVersion(const String& intendedVersion,
                                          const char* runningRev,
                                          const OtaVerdict& verdict) {
  return intendedVersion.length() > 0 && intendedVersion != runningRev &&
         !verdict.otaReverted;
}
