#pragma once
// FollowerImagePolicy.h — pure logic for the ESP-01 follower-image S3 relay
// (#304 Part B, epic #270; spec docs/superpowers/specs/2026-07-14-v2-esp01-
// follower-firmware-relay.md), natively tested by test_follower_image. No
// flash or HTTP here: the upload accumulator + LittleFS write live in
// WebEndpoints.cpp/Tasks.cpp (netTask, the sole storage writer), the
// on-demand streaming upload in ClusterLeader.cpp's clusterTask. This header
// owns the upload filename guard, the push eligibility matrix, the PSRAM
// accumulator cursor check, and the one-shot push phase machine.
//
// The relay is the inverse of #297's guard, not a hole in it: #297 keeps the
// leader's OWN (S3) image away from a foreign-plat member; here we push ONLY
// the stored follower image, and ONLY at an esp01 member.

#include <Arduino.h>

// The one follower platform today. The stored image is a follower-*.bin, so a
// push target must report this exact plat (an S3 member converges via #276; a
// future non-esp01 follower must not be handed an esp01 image).
static const char FOLLOWER_IMAGE_PLAT[] = "esp01";

// --- upload filename guard (mirrors ota-flash.sh #299 prefix check) ------------------

// Length of the leading lowercase-hex run (git short-rev alphabet).
inline int followerImageHexRunLen(const String& s) {
  int n = 0;
  while (n < (int)s.length()) {
    char c = s[n];
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) n++;
    else break;
  }
  return n;
}

// Accept only follower-<rev>[-dirty][-<suffix>].bin and hand back <rev>
// (the [-dirty] tag kept, any trailing size suffix dropped). Rejects an S3
// firmware-*.bin — flashing that at an ESP-01 bricks the row.
inline bool followerImageUploadAccepts(const String& filename, String& outRev) {
  if (!filename.startsWith("follower-")) return false;
  if (!filename.endsWith(".bin")) return false;
  String mid = filename.substring(9, filename.length() - 4);
  int hexLen = followerImageHexRunLen(mid);
  if (hexLen < 7 || hexLen > 40) return false;
  String rev = mid.substring(0, hexLen);
  String rest = mid.substring(hexLen);
  if (rest.startsWith("-dirty")) {
    rev += "-dirty";
    rest = rest.substring(6);
  }
  // Anything left is a size-style suffix and must be dash-led (a bare
  // trailing char means the rev wasn't clean hex — reject).
  if (rest.length() > 0 && rest[0] != '-') return false;
  outRev = rev;
  return true;
}

// --- on-demand push eligibility -----------------------------------------------------

enum class FollowerPushEligibility : uint8_t {
  Eligible = 0,
  NotEsp01,       // target is an S3 (or absent plat = same as leader)
  NoStoredImage   // nothing uploaded to relay yet
};

inline FollowerPushEligibility followerPushEligibility(const String& memberPlat,
                                                       bool storedPresent) {
  if (memberPlat != FOLLOWER_IMAGE_PLAT) return FollowerPushEligibility::NotEsp01;
  if (!storedPresent) return FollowerPushEligibility::NoStoredImage;
  return FollowerPushEligibility::Eligible;
}

inline const char* followerPushEligibilityReason(FollowerPushEligibility e) {
  switch (e) {
    case FollowerPushEligibility::NotEsp01:
      return "target is not an esp01 follower";
    case FollowerPushEligibility::NoStoredImage:
      return "no follower image stored — upload one first";
    default:
      return "eligible";
  }
}

// --- PSRAM accumulator cursor / bounds (anti-corruption, mirrors #191) ---------------

inline bool followerImageChunkOk(size_t index, size_t accumulated, size_t len,
                                 size_t cap) {
  if (index != accumulated) return false;      // no gaps / rewinds
  if (accumulated + len > cap) return false;    // must fit the PSRAM buffer
  return true;
}

// --- one-shot push phase machine ----------------------------------------------------
// On-demand: the operator retries manually, so no attempt cap / rejoin gate
// (that machinery is #276's for repeated automatic convergence). Idle →
// Uploading → Idle, with the last verdict retained for /cluster/status.

enum class FollowerPushPhase : uint8_t { Idle = 0, Uploading };

enum class FollowerPushResult : uint8_t { None = 0, Done, Failed, Rejected };

struct FollowerPushState {
  FollowerPushPhase phase = FollowerPushPhase::Idle;
  int memberIndex = -1;   // valid while Uploading
  uint32_t bytesSent = 0;
  uint32_t bytesTotal = 0;
  FollowerPushResult lastResult = FollowerPushResult::None;
};

inline bool followerPushActive(const FollowerPushState& st) {
  return st.phase != FollowerPushPhase::Idle;
}

inline void followerPushStart(FollowerPushState& st, int memberIndex,
                              uint32_t total) {
  st.phase = FollowerPushPhase::Uploading;
  st.memberIndex = memberIndex;
  st.bytesSent = 0;
  st.bytesTotal = total;
}

inline void followerPushProgress(FollowerPushState& st, uint32_t sent) {
  st.bytesSent = sent;
}

// Zeroes progress at Idle — stale bytes beside phase "idle" would read as a
// wedged push in the Cluster card.
inline void followerPushFinish(FollowerPushState& st, FollowerPushResult result) {
  st.phase = FollowerPushPhase::Idle;
  st.memberIndex = -1;
  st.bytesSent = 0;
  st.bytesTotal = 0;
  st.lastResult = result;
}

inline const char* followerPushResultName(FollowerPushResult r) {
  switch (r) {
    case FollowerPushResult::Done: return "done";
    case FollowerPushResult::Failed: return "failed";
    case FollowerPushResult::Rejected: return "rejected";
    default: return "none";
  }
}

inline const char* followerPushPhaseName(FollowerPushPhase p) {
  return p == FollowerPushPhase::Uploading ? "uploading" : "idle";
}
