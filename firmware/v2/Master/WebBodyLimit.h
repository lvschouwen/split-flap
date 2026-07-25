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

// Ceiling for any body that is NOT a firmware/image upload and not the ping.
// The settings form and every other cluster-wire body are well under 1 KB (the
// largest, an 8-member /cluster/config table, is ~400 B); 2 KB is generous
// headroom that still bounds a single request to a trivially safe allocation.
static const size_t kMaxNonUploadBodyBytes = 2048;

// #386: /cluster/ping is the ONE cluster-wire body whose size scales with the
// wall — it carries the #294 digest piggyback, and URL-encoding a JSON digest
// inflates it ~65% ({, ", :, , all become %XX). A 3-member wall measured
// 1236 B raw -> 2038 B encoded -> 2137 B with you/ts/mac, so the flat 2 KB
// ceiling 413'd EVERY ping before the handler ran: contact aged to the degrade
// bar and the whole cluster cycled joined -> DEGRADED -> re-join every ~46 s.
// The ping therefore gets its own, still-bounded ceiling.
//
// 4096 is a deliberate compromise, NOT headroom for a full wall. The guard
// buffers this body pre-auth — before the ping handler's source-IP/leader
// binding runs — so ANY LAN host can trigger the allocation, and the weakest
// board must survive it. Bench-measured on the ESP-01 (.121): 18680 B free at
// rest; 50 consecutive 3913 B POSTs to /cluster/ping dipped it to 17576 B and
// it settled back to 18600 B, still clustered and rendering — no fragmentation
// creep. 8192 would be ~43% of that free heap, so it is NOT taken.
//
// Consequence, stated plainly: the digest scales at roughly 200 B/member
// (ClusterDigest.h reserve()), so a full CLUSTER_MAX_MEMBERS=8 wall lands
// around 4.6-5.4 KB encoded and its digest will be dropped on EVERY round, not
// occasionally — steady state for a big wall, not an edge case. Liveness is
// unaffected (that is the point of the budget); only the single-pane wall
// mirror goes stale. Raising this ceiling is an ESP-01 heap decision.
static const size_t kMaxPingBodyBytes = 4096;

// The ceiling that applies to `url`. Route-aware so relaxing the ping does not
// relax anything else.
inline size_t bodyLimitCeilingFor(const char* url) {
  if (url != nullptr && strcmp(url, "/cluster/ping") == 0) {
    return kMaxPingBodyBytes;
  }
  return kMaxNonUploadBodyBytes;
}

// Leader-side budget for the ping's OPTIONAL digest piggyback (#294: absent
// ⇒ ""). Liveness must never depend on an optional extra, so the leader asks
// this BEFORE attaching the digest and simply omits it when it would not fit —
// a degraded wall mirror instead of a dead cluster. `overheadLen` is the rest
// of the body (digest=, you=, ts=, mac= and separators).
inline bool pingBodyDigestFits(size_t encodedDigestLen, size_t overheadLen) {
  return encodedDigestLen + overheadLen <= kMaxPingBodyBytes;
}

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
  if (contentLength <= bodyLimitCeilingFor(url)) return false;
  return !(isMultipart && bodyLimitUrlIsUpload(url));
}
