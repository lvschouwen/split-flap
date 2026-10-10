// ReleaseManifest.h — what a release says about itself, and what a board
// refuses (#583; docs/superpowers/specs/2026-10-10-release-update-design.md,
// section 3). Pure, natively tested by Master's test_release_manifest. A
// manifest is read only after its signature checked out (ReleaseFetch.h).
//
// Lives outside shared/ on purpose: no unit compiles it.
#pragma once

#include <ArduinoJson.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ReleaseSource.h"

// A board takes no larger manifest (flashing/release.py holds the same line).
#define RELEASE_MANIFEST_MAX 2048
#define RELEASE_MANIFEST_FORMAT 1
#define RELEASE_REV_MAX 40
#define RELEASE_PATH_MAX 95
#define RELEASE_TAG_MAX 31
#define RELEASE_NOTES_MAX 127
// The revs of unit firmware that read current on the release's master, as
// "a,b,c". A list that does not fit is left empty: not known.
#define RELEASE_UNIT_REVS_MAX 127
#define RELEASE_URL_MAX 192

// Why a look or an install did not go through. Numbers are not kept anywhere.
enum class ReleaseError : uint8_t {
  Ok,
  NoClock,     // certificates cannot be judged without the time
  NoAnswer,    // the site did not give the file
  TooLarge,    // a manifest over RELEASE_MANIFEST_MAX
  Signature,   // not signed by the release key
  NotJson,
  Format,      // a format this firmware does not know
  Channel,     // signed for another channel than was asked for
  Field,       // something is missing or has the wrong shape
  Path,        // a path that is not relative or leaves the directory
  Length,      // the image is announced with another size than the manifest's
  CutOff,      // the download ended early
  Hash,        // the image is not the one the manifest names
  Write,       // it could not be written
  Busy,        // what it writes to is in use
  Memory,
};

inline const char* releaseErrorText(ReleaseError e) {
  switch (e) {
    case ReleaseError::Ok: return "ok";
    case ReleaseError::NoClock: return "the clock is not set yet";
    case ReleaseError::NoAnswer: return "the release site did not answer";
    case ReleaseError::TooLarge: return "the release description is too large";
    case ReleaseError::Signature: return "the release is not signed by the release key";
    case ReleaseError::NotJson: return "the release description cannot be read";
    case ReleaseError::Format: return "the release is in a format this firmware does not know";
    case ReleaseError::Channel: return "the release is of another channel";
    case ReleaseError::Field: return "the release description is incomplete";
    case ReleaseError::Path: return "the release names a file outside its directory";
    case ReleaseError::Length: return "the file has another size than the release says";
    case ReleaseError::CutOff: return "the download was cut off";
    case ReleaseError::Hash: return "the file is not the one the release names";
    case ReleaseError::Write: return "it could not be written";
    case ReleaseError::Busy: return "another install is running";
    case ReleaseError::Memory: return "out of memory";
  }
  return "?";
}

struct ReleaseImage {
  char rev[RELEASE_REV_MAX + 1] = {0};
  char path[RELEASE_PATH_MAX + 1] = {0};
  uint32_t size = 0;
  uint8_t sha256[32] = {0};
};

struct ReleaseManifest {
  char channel[8] = {0};
  char tag[RELEASE_TAG_MAX + 1] = {0};
  char notes[RELEASE_NOTES_MAX + 1] = {0};
  uint32_t commitTime = 0;
  ReleaseImage master;
  ReleaseImage row;
  ReleaseImage rescue;
  uint8_t rowMd5[16] = {0};
  char unitRevs[RELEASE_UNIT_REVS_MAX + 1] = {0};
};

// The directory of a channel on the site, nullptr for a name that is none.
inline const char* releaseChannelDir(const char* channel) {
  if (strcmp(channel, "stable") == 0) return "releases";
  if (strcmp(channel, "test") == 0) return "test";
  return nullptr;
}

// Relative, inside its directory, and of nothing a URL gives meaning to.
inline bool releasePathOk(const char* path) {
  const size_t n = strlen(path);
  if (n == 0 || n > RELEASE_PATH_MAX || path[0] == '/' || path[0] == '.') return false;
  for (size_t i = 0; i < n; i++) {
    const char c = path[i];
    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '-' || c == '_' || c == '.' || c == '/';
    if (!plain) return false;
    if (c == '.' && path[i + 1] == '.') return false;
    if (c == '/' && (path[i + 1] == '/' || path[i + 1] == '.' || path[i + 1] == 0)) return false;
  }
  return true;
}

// "https://<host>/<channel dir>/<name>". False when it does not fit.
inline bool releaseUrl(char* out, size_t cap, const char* channel, const char* name) {
  const char* dir = releaseChannelDir(channel);
  if (dir == nullptr) return false;
  const int n = snprintf(out, cap, "https://" RELEASE_HOST "/%s/%s", dir, name);
  return n > 0 && (size_t)n < cap;
}

namespace releasemanifest {

inline int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Exactly 2*n lowercase hex digits.
inline bool hexBytes(const char* text, uint8_t* out, size_t n) {
  if (text == nullptr || strlen(text) != 2 * n) return false;
  for (size_t i = 0; i < n; i++) {
    const int hi = hexNibble(text[2 * i]);
    const int lo = hexNibble(text[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

// A string that is there, not empty and fits.
inline bool text(JsonVariantConst v, char* out, size_t cap) {
  const char* s = v.as<const char*>();
  if (s == nullptr || s[0] == 0 || strlen(s) >= cap) return false;
  strcpy(out, s);
  return true;
}

inline ReleaseError image(JsonVariantConst v, ReleaseImage& out) {
  if (!text(v["rev"], out.rev, sizeof(out.rev))) return ReleaseError::Field;
  const char* path = v["path"].as<const char*>();
  if (path == nullptr) return ReleaseError::Field;
  if (!releasePathOk(path)) return ReleaseError::Path;
  strcpy(out.path, path);
  if (!v["size"].is<uint32_t>() || v["size"].as<uint32_t>() == 0) return ReleaseError::Field;
  out.size = v["size"].as<uint32_t>();
  if (!hexBytes(v["sha256"].as<const char*>(), out.sha256, 32)) return ReleaseError::Field;
  return ReleaseError::Ok;
}

}  // namespace releasemanifest

// Reads the bytes of a latest.json whose signature checked out. `channel` is
// the one that was asked for. `out` is whole only when the answer is Ok.
namespace releasemanifest {

inline ReleaseError read(const uint8_t* json, size_t len, const char* channel,
                         ReleaseManifest& out) {
  if (len > RELEASE_MANIFEST_MAX) return ReleaseError::TooLarge;
  JsonDocument doc;
  if (deserializeJson(doc, (const char*)json, len) != DeserializationError::Ok ||
      !doc.is<JsonObjectConst>()) {
    return ReleaseError::NotJson;
  }
  JsonVariantConst root = doc.as<JsonVariantConst>();
  if (!root["format"].is<int>() || root["format"].as<int>() != RELEASE_MANIFEST_FORMAT) {
    return ReleaseError::Format;
  }
  // Both channels are signed with the one key: without this a trial release
  // could be handed to every wall as a release.
  const char* signedFor = root["channel"].as<const char*>();
  if (signedFor == nullptr) return ReleaseError::Field;
  if (strcmp(signedFor, channel) != 0 || strlen(channel) >= sizeof(out.channel)) {
    return ReleaseError::Channel;
  }
  strcpy(out.channel, channel);
  if (!text(root["tag"], out.tag, sizeof(out.tag))) return ReleaseError::Field;
  if (!text(root["notes"], out.notes, sizeof(out.notes))) return ReleaseError::Field;
  if (!root["commitTime"].is<uint32_t>() || root["commitTime"].as<uint32_t>() == 0) {
    return ReleaseError::Field;
  }
  out.commitTime = root["commitTime"].as<uint32_t>();
  ReleaseError e = image(root["master"], out.master);
  if (e == ReleaseError::Ok) e = image(root["row"], out.row);
  if (e == ReleaseError::Ok) e = image(root["rescue"], out.rescue);
  if (e != ReleaseError::Ok) return e;
  if (!hexBytes(root["row"]["md5"].as<const char*>(), out.rowMd5, 16)) return ReleaseError::Field;
  // Not needed to install: a release without it only cannot say whether the
  // units will read outdated afterwards.
  size_t used = 0;
  for (JsonVariantConst rev : root["units"]["revs"].as<JsonArrayConst>()) {
    const char* s = rev.as<const char*>();
    const size_t n = s == nullptr ? 0 : strlen(s);
    if (n == 0 || used + n + 1 > RELEASE_UNIT_REVS_MAX) {
      used = 0;
      break;
    }
    if (used > 0) out.unitRevs[used++] = ',';
    memcpy(out.unitRevs + used, s, n);
    used += n;
  }
  out.unitRevs[used] = 0;
  return ReleaseError::Ok;
}

}  // namespace releasemanifest

inline ReleaseError releaseManifestRead(const uint8_t* json, size_t len, const char* channel,
                                        ReleaseManifest& out) {
  ReleaseManifest read;
  const ReleaseError e = releasemanifest::read(json, len, channel, read);
  // Nothing of a refused manifest is handed on.
  out = e == ReleaseError::Ok ? read : ReleaseManifest();
  return e;
}

// A release is offered when it was committed after what runs. Equal or
// earlier is up to date: a build ahead of the release is never offered it,
// and an old manifest played back offers nothing.
inline bool releaseNewer(const ReleaseManifest& m, uint32_t runningCommitTime) {
  return m.commitTime > runningCommitTime;
}

// The rescue slot and the stored row image are compared by rev.
inline bool releaseImageDiffers(const ReleaseImage& image, const char* haveRev) {
  return haveRev == nullptr || strcmp(image.rev, haveRev) != 0;
}
