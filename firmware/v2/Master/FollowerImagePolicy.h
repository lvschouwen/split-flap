#pragma once
// FollowerImagePolicy.h — pure rules for the stored row image (the
// follower-<rev>.bin the master keeps and offers to its ESP-01 row boards),
// natively tested by test_follower_image: the upload filename guard, the
// PSRAM accumulator cursor check and the hold on an image a release stored. No flash or HTTP here: the upload route is
// WebFirmware.cpp, the store FollowerImageStore.cpp.

#include <Arduino.h>

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

// --- PSRAM accumulator cursor / bounds (anti-corruption, mirrors #191) ---------------

inline bool followerImageChunkOk(size_t index, size_t accumulated, size_t len,
                                 size_t cap) {
  if (index != accumulated) return false;      // no gaps / rewinds
  if (accumulated + len > cap) return false;    // must fit the PSRAM buffer
  return true;
}

// --- an image stored by an update from a release (#583) ------------------------------

// Such an image is held: it is offered to the rows only once the master
// itself runs the release's master rev (`heldFor`, "" for an image that was
// uploaded). A master that fell back to its old image keeps the rows where
// they are.
inline bool followerImageHeld(const char* heldFor, const char* runningRev) {
  return heldFor != nullptr && heldFor[0] != 0 && strcmp(heldFor, runningRev) != 0;
}
