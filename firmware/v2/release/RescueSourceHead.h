// RescueSourceHead.h — the rescue sources an image was built from, and how a
// board tells whether the rescue image in its factory slot is the one its
// own build knows (#613; identity by content, #440).
//
// Every board image carries RESCUE_SOURCE_HEAD (BuildVersion.h): the rev of
// the last commit touching the rescue image's inputs — firmware/v2/Rescue,
// release, shared and buildtools — as fwbuild.py computes it. The rescue
// image embeds the same value in its binary as a marker string, so the
// master reads it off the partition and no installer has to know it. Equal
// heads = the slot holds what this build knows, whatever the repo rev of
// either image; different = outdated; no marker = of unknown vintage.
//
// Lives outside shared/ on purpose, like the rest of ../release: no unit
// compiles it. Pure; natively tested by Master's test_rescue_slot.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RESCUE_SOURCE_MARKER_PREFIX "SFRESCUE-SRC:"
#define RESCUE_SOURCE_MARKER_SUFFIX ":END"
// A short git rev: 7 by default, longer in a repository that needs it.
#define RESCUE_SOURCE_HEAD_MIN 4
#define RESCUE_SOURCE_HEAD_MAX 16

static const size_t RESCUE_SOURCE_MARKER_PREFIX_LEN = sizeof(RESCUE_SOURCE_MARKER_PREFIX) - 1;
static const size_t RESCUE_SOURCE_MARKER_SUFFIX_LEN = sizeof(RESCUE_SOURCE_MARKER_SUFFIX) - 1;
// prefix + head + suffix: what a scan window must hold whole.
static const size_t RESCUE_SOURCE_MARKER_MAX = RESCUE_SOURCE_MARKER_PREFIX_LEN +
                                               RESCUE_SOURCE_HEAD_MAX +
                                               RESCUE_SOURCE_MARKER_SUFFIX_LEN;

inline bool rescueSourceHeadChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// Reads the head out of a marker that starts at `at` with `len` bytes to
// look at. True when a whole, well-formed marker is there; `out` gets the
// head. A prefix followed by anything else is not a marker (the prefix
// alone may sit in a binary as a literal of its own).
inline bool rescueSourceHeadAt(const uint8_t* at, size_t len, char* out, size_t cap) {
  if (len < RESCUE_SOURCE_MARKER_PREFIX_LEN + RESCUE_SOURCE_HEAD_MIN +
                RESCUE_SOURCE_MARKER_SUFFIX_LEN ||
      memcmp(at, RESCUE_SOURCE_MARKER_PREFIX, RESCUE_SOURCE_MARKER_PREFIX_LEN) != 0) {
    return false;
  }
  size_t n = 0;
  const uint8_t* p = at + RESCUE_SOURCE_MARKER_PREFIX_LEN;
  while (n < RESCUE_SOURCE_HEAD_MAX && RESCUE_SOURCE_MARKER_PREFIX_LEN + n < len &&
         rescueSourceHeadChar((char)p[n])) {
    n++;
  }
  if (n < RESCUE_SOURCE_HEAD_MIN) return false;
  if (RESCUE_SOURCE_MARKER_PREFIX_LEN + n + RESCUE_SOURCE_MARKER_SUFFIX_LEN > len ||
      memcmp(p + n, RESCUE_SOURCE_MARKER_SUFFIX, RESCUE_SOURCE_MARKER_SUFFIX_LEN) != 0) {
    return false;
  }
  if (cap == 0) return false;
  if (n >= cap) n = cap - 1;
  memcpy(out, p, n);
  out[n] = 0;
  return true;
}

// Looks for the marker in `buf`. Returns the offset of a whole marker, or
// -1; `out` gets its head. A marker cut off by the end of the buffer is not
// found: the caller's next window must overlap by RESCUE_SOURCE_MARKER_MAX.
inline long rescueSourceHeadFind(const uint8_t* buf, size_t len, char* out, size_t cap) {
  if (len < RESCUE_SOURCE_MARKER_PREFIX_LEN) return -1;
  for (size_t i = 0; i + RESCUE_SOURCE_MARKER_PREFIX_LEN <= len; i++) {
    if (buf[i] != (uint8_t)RESCUE_SOURCE_MARKER_PREFIX[0]) continue;
    if (rescueSourceHeadAt(buf + i, len - i, out, cap)) return (long)i;
  }
  return -1;
}

// The head of a marker an image carries as its own string (the rescue
// image's GET /settings).
inline const char* rescueSourceHeadOfMarker(const char* marker) {
  return marker + RESCUE_SOURCE_MARKER_PREFIX_LEN;
}

inline size_t rescueSourceHeadLen(const char* marker) {
  const char* head = rescueSourceHeadOfMarker(marker);
  size_t n = 0;
  while (head[n] != 0 && head[n] != ':') n++;
  return n;
}
