// WebBodyLimit.h (#347) — pure pre-auth request-body size policy.
//
// The ESPAsyncWebServer parser accumulates a urlencoded / plain / multipart
// NON-file body into a heap String BEFORE any application handler or the
// CORS/CSRF middleware runs. A single large POST from any LAN client would
// therefore exhaust the S3's internal SRAM pre-auth (a hard-abort DoS). This
// header decides, from facts known at header-parse time (URL, is-multipart,
// declared Content-Length), whether a request's body must be rejected early
// with 413 — before a byte is buffered. The thin glue that owns the
// AsyncWebHandler subclass lives in each project's web-init TU.
//
// Scope: this guard covers bodies with a declared Content-Length. It does
// NOT cover chunked Transfer-Encoding (contentLength() == 0) — that path is
// harmless today because no route implements onBody/handleBody, so chunked
// bytes are discarded by the default no-op. The multipart NON-file field and
// header-line buffering the guard's upload exemption cannot see is bounded
// separately, inside the parser, by patch_asyncweb.py (#347).
//
// COPIED HEADER — byte-identical across Master / Rescue / FollowerEsp01 (the
// whole fleet now shares esp32async/ESPAsyncWebServer, #356). Fix bugs in
// every tree; the union upload allowlist below intentionally names routes
// that only exist on some projects (a URL that never occurs is inert).
#pragma once

#include <stddef.h>
#include <string.h>

// Ceiling for any body that is NOT a firmware/image upload. The settings form
// and every cluster-wire body are well under 1 KB (the largest, an 8-member
// /cluster/config table, is ~400 B); 2 KB is generous headroom that still
// bounds a single request to a trivially safe allocation.
static const size_t kMaxNonUploadBodyBytes = 2048;

// The only routes whose declared Content-Length legitimately exceeds the
// ceiling: multipart firmware/image uploads. Their bytes stream through a
// fixed chunk buffer in the parser (bounded), never a heap String — so they
// are exempt, but ONLY as genuine multipart (see bodyLimitExceeded). Union of
// every project's upload routes so the copies stay identical.
inline bool bodyLimitUrlIsUpload(const char* url) {
  if (url == nullptr) return false;
  return strcmp(url, "/firmware/master") == 0 ||
         strcmp(url, "/firmware/rescue") == 0 ||
         strcmp(url, "/cluster/follower-firmware") == 0;
}

// True → the request body must be rejected early (413) rather than buffered.
// A large body is allowed through ONLY when it is a genuine multipart upload
// to a known firmware/image route; a non-multipart (e.g. urlencoded) body to
// that same URL is still capped, closing the "post a huge form field to
// /firmware/master" bypass.
inline bool bodyLimitExceeded(const char* url, bool isMultipart,
                              size_t contentLength) {
  if (contentLength <= kMaxNonUploadBodyBytes) return false;
  return !(isMultipart && bodyLimitUrlIsUpload(url));
}
