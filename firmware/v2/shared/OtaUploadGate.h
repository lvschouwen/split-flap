#pragma once
// OtaUploadGate.h — the decisions around a firmware upload that every board
// makes the same way: is the md5 a digest, may the upload start at all, has
// it stalled, and what does the completion handler answer. Each tree keeps
// its own flash calls (Update on the ESP8266/ESP32, raw esp_partition writes
// for the factory slot); none of them decides these by hand. Natively tested
// by test_rescue_ota (Rescue) and test_ota_status (Master).

#include <Arduino.h>

// Lowercases the digest in place, then demands exactly 32 hex chars — a
// digest that can't be a digest is a client bug, rejected with 400 before
// any flash work starts: Update.begin erases the target region, so a
// malformed md5 must never get that far.
inline bool normalizeOtaMd5(String& md5) {
  md5.toLowerCase();
  if (md5.length() != 32) return false;
  for (size_t i = 0; i < 32; i++) {
    char c = md5[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// --- may this upload start? ---------------------------------------------------

enum class OtaGate : uint8_t {
  Pass = 0,
  CrossOrigin,      // a browser POST from outside the LAN (LanOrigin.h)
  UnitReflashBusy,  // a unit reflash owns the bus; a reboot would strand it
  Md5Missing,
  Md5Malformed,
};

// Runs on the first chunk, BEFORE any flash is touched. The CSRF check comes
// first: an attacker can match ?md5= to their own bytes (MD5 is integrity,
// not authenticity). On Pass, `md5` holds the normalized digest.
inline OtaGate otaUploadGate(bool crossOrigin, bool unitReflashBusy,
                             String& md5) {
  if (crossOrigin) return OtaGate::CrossOrigin;
  if (unitReflashBusy) return OtaGate::UnitReflashBusy;
  if (md5.length() == 0) return OtaGate::Md5Missing;
  if (!normalizeOtaMd5(md5)) return OtaGate::Md5Malformed;
  return OtaGate::Pass;
}

inline int otaGateHttpStatus(OtaGate g) {
  switch (g) {
    case OtaGate::Pass:            return 200;
    case OtaGate::CrossOrigin:     return 403;
    case OtaGate::UnitReflashBusy: return 409;
    default:                       return 400;
  }
}

inline const __FlashStringHelper* otaGateReason(OtaGate g) {
  switch (g) {
    case OtaGate::CrossOrigin:
      return F("Cross-origin firmware upload refused (CSRF guard)");
    case OtaGate::UnitReflashBusy:
      return F("Unit reflash in progress — retry when it finishes");
    case OtaGate::Md5Missing:
      return F("md5 query parameter is required (compute it over the .bin "
               "and pass ?md5=...)");
    case OtaGate::Md5Malformed:
      return F("md5 must be exactly 32 hex characters");
    default:
      return F("");
  }
}

// --- the rejection an upload callback leaves for the completion handler -------

// The upload callback cannot answer the request, so it records why it refused
// and the completion handler sends it. take() hands the verdict over AND
// clears it: left set, a later POST whose upload callback never runs (no file
// part) would echo this stale status instead of its own 400.
struct OtaRejection {
  int status = 0;  // 0 = not rejected
  String reason;

  bool rejected() const { return status != 0; }
  void clear() {
    status = 0;
    reason = String();
  }
  void set(int httpStatus, const String& why) {
    status = httpStatus;
    reason = why;
  }
  void set(OtaGate g) { set(otaGateHttpStatus(g), String(otaGateReason(g))); }
  bool take(int& outStatus, String& outReason) {
    if (status == 0) return false;
    outStatus = status;
    outReason = reason;
    clear();
    return true;
  }
};

// --- what the completion handler answers --------------------------------------

enum class OtaCompletion : uint8_t {
  Rejected = 0,  // the upload callback refused: send its status + reason
  FlashError,    // the flash layer latched an error: 500 + its text
  NoFile,        // no multipart file part streamed: 400, nothing was flashed
  Incomplete,    // the stream ended before the image was complete: 500
  Flashed,       // 200, and the tree does what follows (reboot / record)
};

// `uploadRan` = the upload callback established a session for THIS request.
// It is checked before everything the flash layer says: that state is a
// singleton which outlives requests. A POST with no file part never begins
// an Update, and a never-begun Update reports itself finished — answering
// 200 there was a spurious, unauthenticated reboot (#347) — and it still
// carries the error of whatever upload failed last, which is not this
// request's error either.
inline OtaCompletion otaUploadCompletion(bool rejected, bool flashError,
                                         bool uploadRan, bool finished) {
  if (rejected) return OtaCompletion::Rejected;
  if (!uploadRan) return OtaCompletion::NoFile;
  if (flashError) return OtaCompletion::FlashError;
  if (!finished) return OtaCompletion::Incomplete;
  return OtaCompletion::Flashed;
}

// --- stall watchdog -------------------------------------------------------------

// A client that opens an upload and goes silent holds the flash session (and
// whatever the tree froze for it). No chunk for this long = abandon it; the
// next upload's begin path recovers the stale session.
#define OTA_STALL_TIMEOUT_MS 30000UL

// Signed: the stamp is written by the upload task and read by another, so
// it can be NEWER than the `nowMs` the caller sampled a moment earlier. An
// unsigned difference would read that as ~49 days of silence.
inline bool otaUploadStalled(uint32_t lastChunkMs, uint32_t nowMs) {
  return (int32_t)(nowMs - lastChunkMs) > (int32_t)OTA_STALL_TIMEOUT_MS;
}
