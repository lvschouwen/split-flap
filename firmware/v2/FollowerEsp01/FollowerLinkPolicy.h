#pragma once
// FollowerLinkPolicy.h — the timing rules of the row board's wall link
// (#559/#564), pure and natively tested by test_follower_link. The socket
// glue lives in FollowerLink.cpp.

#include <stdint.h>
#include <string.h>

// Redial delay after a failed or dropped connection: 1 s, doubling to 8 s.
#define FOLLOWER_LINK_BACKOFF_MIN_MS 1000UL
#define FOLLOWER_LINK_BACKOFF_MAX_MS 8000UL
// A connect attempt blocks loop() for at most this long.
#define FOLLOWER_LINK_CONNECT_TIMEOUT_MS 1000UL
// Vitals go up this often while connected.
#define FOLLOWER_LINK_STATUS_INTERVAL_MS 10000UL
// The unit facts go up this often while connected, and after every job.
#define FOLLOWER_LINK_UNITS_INTERVAL_MS 30000UL
// A master that has not answered Hello within this is not a master.
#define FOLLOWER_LINK_WELCOME_TIMEOUT_MS 5000UL

inline uint32_t followerLinkNextBackoffMs(uint32_t previousMs) {
  if (previousMs < FOLLOWER_LINK_BACKOFF_MIN_MS) return FOLLOWER_LINK_BACKOFF_MIN_MS;
  const uint32_t next = previousMs * 2;
  return next > FOLLOWER_LINK_BACKOFF_MAX_MS ? FOLLOWER_LINK_BACKOFF_MAX_MS : next;
}

// millis()-wrap-safe "has `intervalMs` passed since `sinceMs`".
inline bool followerLinkElapsed(uint32_t nowMs, uint32_t sinceMs, uint32_t intervalMs) {
  return (uint32_t)(nowMs - sinceMs) >= intervalMs;
}

// The stored master address may carry a port ("host:8801") from the HTTP
// wire; the link always dials its own port, so only the host part counts.
// Copies it into out (cap bytes); false when there is none or it does not fit.
inline bool followerLinkHostPart(const char* stored, char* out, size_t cap) {
  const char* colon = strchr(stored, ':');
  const size_t n = colon ? (size_t)(colon - stored) : strlen(stored);
  if (n == 0 || n >= cap) return false;
  memcpy(out, stored, n);
  out[n] = 0;
  return true;
}

// A row paired with one master must not take orders from another: the name a
// master gives in Welcome has to be the one stored at pairing.
inline bool followerLinkMasterAccepted(const char* pairedName, const char* welcomeName) {
  return pairedName[0] != 0 && strcmp(pairedName, welcomeName) == 0;
}
