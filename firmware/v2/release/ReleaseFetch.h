// ReleaseFetch.h — the steps of looking for a release and of installing one
// of its images (#583), over hooks: fetching, the signature check, the hash
// and the writer are the board's (ReleaseTarget.h). The order is what this
// file is for and what Master's test_release_fetch holds it to:
//
//   look:    the manifest is read only after its signature checked out
//   install: nothing is made the next thing to run before the size and the
//            SHA-256 of every byte written matched the manifest
//
// Lives outside shared/ on purpose: no unit compiles it.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ReleaseManifest.h"

// A signature of the release key in DER, base64, with a line end.
#define RELEASE_SIGNATURE_MAX 128

struct ReleaseHooks {
  // A whole small file. The bytes read; -1 when it could not be fetched, -2
  // when it is larger than `cap`.
  virtual int get(const char* url, uint8_t* out, size_t cap) = 0;
  // Is `signature` (base64 of DER) the release key's over exactly `message`?
  virtual bool signatureOk(const uint8_t* message, size_t len, const char* signature,
                           size_t signatureLen) = 0;
  // A download: the length the site announces, -1 when it gave none or the
  // file could not be opened. read: 0 at the end, below 0 when it broke off.
  virtual long open(const char* url) = 0;
  virtual int read(uint8_t* out, size_t cap) = 0;
  virtual void close() = 0;
  // SHA-256 over what was downloaded.
  virtual void hashStart() = 0;
  virtual void hashAdd(const uint8_t* data, size_t len) = 0;
  virtual void hashEnd(uint8_t out[32]) = 0;
  // Between the pieces of a download.
  virtual void progress(uint32_t done, uint32_t size) {
    (void)done;
    (void)size;
  }
  virtual ~ReleaseHooks() {}
};

// Where an image goes. After begin() exactly one of commit() and abort() is
// called; commit() is what makes the image count, abort() leaves nothing of
// it in use.
struct ReleaseWriter {
  virtual ReleaseError begin(const ReleaseImage& image) = 0;
  virtual bool write(const uint8_t* data, size_t len, uint32_t offset) = 0;
  virtual bool commit() = 0;
  virtual void abort() = 0;
  virtual ~ReleaseWriter() {}
};

struct ReleaseLookBuffers {
  uint8_t json[RELEASE_MANIFEST_MAX];
  char signature[RELEASE_SIGNATURE_MAX];
};

// Fetches latest.json of `channel` and its signature. `out` is the release
// only when the answer is Ok.
inline ReleaseError releaseLook(ReleaseHooks& hooks, const char* channel, ReleaseLookBuffers& work,
                                ReleaseManifest& out) {
  out = ReleaseManifest();
  char url[RELEASE_URL_MAX];
  if (!releaseUrl(url, sizeof(url), channel, "latest.json")) return ReleaseError::Channel;
  const int jsonLen = hooks.get(url, work.json, sizeof(work.json));
  if (jsonLen == -2) return ReleaseError::TooLarge;
  if (jsonLen <= 0) return ReleaseError::NoAnswer;
  if (!releaseUrl(url, sizeof(url), channel, "latest.json.sig")) return ReleaseError::Channel;
  const int sigLen = hooks.get(url, (uint8_t*)work.signature, sizeof(work.signature));
  if (sigLen == -2) return ReleaseError::Signature;
  if (sigLen <= 0) return ReleaseError::NoAnswer;
  if (!hooks.signatureOk(work.json, (size_t)jsonLen, work.signature, (size_t)sigLen)) {
    return ReleaseError::Signature;
  }
  return releaseManifestRead(work.json, (size_t)jsonLen, channel, out);
}

// Is an image there, as long as the manifest says? Asked for every image of
// an update before the first of them is written: a slot is erased as its
// download begins, so what cannot be fetched is found out before that.
inline ReleaseError releaseReachable(ReleaseHooks& hooks, const char* channel,
                                     const ReleaseImage& image) {
  char url[RELEASE_URL_MAX];
  if (!releasePathOk(image.path) || !releaseUrl(url, sizeof(url), channel, image.path)) {
    return ReleaseError::Path;
  }
  const long announced = hooks.open(url);
  hooks.close();
  if (announced < 0) return ReleaseError::NoAnswer;
  return (uint32_t)announced == image.size ? ReleaseError::Ok : ReleaseError::Length;
}

// Downloads one image of a release into `writer`, through `buffer`.
inline ReleaseError releaseInstall(ReleaseHooks& hooks, const char* channel,
                                   const ReleaseImage& image, ReleaseWriter& writer,
                                   uint8_t* buffer, size_t bufferLen) {
  char url[RELEASE_URL_MAX];
  if (!releasePathOk(image.path) || !releaseUrl(url, sizeof(url), channel, image.path)) {
    return ReleaseError::Path;
  }
  const long announced = hooks.open(url);
  if (announced < 0) {
    hooks.close();
    return ReleaseError::NoAnswer;
  }
  // Judged before anything is erased for it.
  if ((uint32_t)announced != image.size) {
    hooks.close();
    return ReleaseError::Length;
  }
  ReleaseError e = writer.begin(image);
  if (e != ReleaseError::Ok) {
    hooks.close();
    return e;
  }
  hooks.hashStart();
  uint32_t done = 0;
  while (e == ReleaseError::Ok && done < image.size) {
    const size_t want = image.size - done < bufferLen ? image.size - done : bufferLen;
    const int n = hooks.read(buffer, want);
    if (n <= 0 || (size_t)n > want) {
      e = ReleaseError::CutOff;
    } else if (!writer.write(buffer, (size_t)n, done)) {
      e = ReleaseError::Write;
    } else {
      hooks.hashAdd(buffer, (size_t)n);
      done += (uint32_t)n;
      hooks.progress(done, image.size);
    }
  }
  hooks.close();
  uint8_t digest[32];
  hooks.hashEnd(digest);
  if (e == ReleaseError::Ok && memcmp(digest, image.sha256, sizeof(digest)) != 0) {
    e = ReleaseError::Hash;
  }
  if (e == ReleaseError::Ok && !writer.commit()) e = ReleaseError::Write;
  if (e != ReleaseError::Ok) writer.abort();
  return e;
}
